#include "session.h"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "link_utils.h" // u2s/s2u/HideUiBlock (公共会话工具, 经验 32 平台归组)
#include <base/log.h> // OfficeLog (Linux 实现 office_runtime, Windows 实现 common)

#include <osl/file.hxx>
#include <com/sun/star/lang/XMultiComponentFactory.hpp>
#include <com/sun/star/awt/XTopWindow.hpp>
#include <com/sun/star/frame/XComponentLoader.hpp>
#include <com/sun/star/frame/XDispatch.hpp>
#include <com/sun/star/frame/XDispatchProvider.hpp>
#include <com/sun/star/frame/FrameSearchFlag.hpp>
#include <com/sun/star/frame/XLayoutManager.hpp>
#include <com/sun/star/frame/XModel.hpp>
#include <com/sun/star/util/URL.hpp>
#include <com/sun/star/util/XURLTransformer.hpp>
#include <com/sun/star/awt/XWindow.hpp>
#include <com/sun/star/beans/PropertyValue.hpp>
#include <com/sun/star/beans/XPropertySet.hpp>
#include <com/sun/star/util/XCloseable.hpp>
#include <com/sun/star/container/XNamed.hpp>
#include <com/sun/star/container/XNameAccess.hpp>
#include <com/sun/star/sheet/XViewFreezable.hpp>
#include <com/sun/star/sheet/XSpreadsheet.hpp>
#include <com/sun/star/sheet/XSpreadsheetDocument.hpp>
#include <com/sun/star/table/CellRangeAddress.hpp>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
// windows.h 的 FindWindow 宏 (-> FindWindowA) 与 LinkPlatform 接口成员同名
#undef FindWindow
#elif defined(__linux__)
#include <cstdlib>
#endif

using namespace css::uno;
using namespace css::lang;
using namespace css::container;
using namespace css::frame;
using namespace css::util;
using namespace css::beans;
using namespace css::sheet;

using link_utils::u2s;
using link_utils::s2u;

// 平台隔离设计 ([platform-isolation] Part2): 会话层零 #ifdef, 引导/加载/窗口落位由平台层承担。
// ---------------------------------------------------------------------------

CalcSession::CalcSession() = default;

CalcSession::~CalcSession() {
    Destroy();
}

bool CalcSession::PushFrame() {
    if (destroyed_ || !platform_ || !cb_)  // V4: Destroy 窗口期帧泵线程安全退出
        return false;
    platform_->HideUiFloats(); // keep the exit-float out of captures

    auto t0 = std::chrono::steady_clock::now();
    uint8_t* pixels = nullptr;
    int w = 0, h = 0;
    if (!platform_->CaptureFrame(pixels, w, h))
        return false;
    width_ = w;
    height_ = h;
    cb_(pixels, w, h, w * 4, w * h * 4, link_utils::kFrameFormatBGRA, opaque_);

    // Average capture cost, logged every 30 frames.
    static long long sum_us = 0;
    static int count = 0;
    sum_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    if (++count >= 30) {
        OfficeLogDbg("[CalcLink] capture avg %lld us", static_cast<long long>(sum_us / count));
        sum_us = 0;
        count = 0;
    }
    return true;
}

bool CalcSession::ScrollByRows(int deltaRows) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!pane_.is())
        return false;
    try {
        sal_Int32 before = pane_->getFirstVisibleRow();
        pane_->setFirstVisibleRow(before + deltaRows);
        sal_Int32 after = pane_->getFirstVisibleRow();
        OfficeLog("[CalcLink.Scroll] session=%p rows %d -> %d (delta %d)",
                  (void*)this, (int)before, (int)after, deltaRows);
    } catch (const css::uno::Exception& e) {
        OfficeLogWarn("[CalcLink] ScrollByRows failed: %s", u2s(e.Message).c_str());
        return false;
    }
    return true;
}

bool CalcSession::ScrollByCols(int deltaCols) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!pane_.is())
        return false;
    try {
        sal_Int32 before = pane_->getFirstVisibleColumn();
        pane_->setFirstVisibleColumn(before + deltaCols);
        sal_Int32 after = pane_->getFirstVisibleColumn();
        OfficeLog("[CalcLink.Scroll] session=%p cols %d -> %d (delta %d)",
                  (void*)this, (int)before, (int)after, deltaCols);
    } catch (const css::uno::Exception& e) {
        OfficeLogWarn("[CalcLink] ScrollByCols failed: %s", u2s(e.Message).c_str());
        return false;
    }
    return true;
}

// Remember the current viewport state so the poller does not re-push.
void CalcSession::SyncViewportState() {
    try {
        if (pane_.is()) {
            last_row_ = pane_->getFirstVisibleRow();
            last_col_ = pane_->getFirstVisibleColumn();
        }
        if (view_.is()) {
            Reference<css::sheet::XSpreadsheet> active = view_->getActiveSheet();
            Reference<XNamed> n(active, UNO_QUERY);
            last_sheet_ = u2s(n->getName());
        }
    } catch (const css::uno::Exception& e) {
        OfficeLogDbg("[CalcLink] SyncViewportState failed: %s", u2s(e.Message).c_str());
    }
}

// 阶段4: ChangeFn 探测 (视口签名 + force_frame_脏位, 持 mu_)
// 原 PollThread 内联逻辑提取: 检查视口 first row/col + sheet 名变化,
// 同时消费 force_frame_ 脏位 (滚动/切表/缩放置位)。
bool CalcSession::CheckViewportChanged() {
    std::lock_guard<std::mutex> lk(mu_);
    bool changed = force_frame_.exchange(false);
    try {
        if (pane_.is()) {
            sal_Int32 r = pane_->getFirstVisibleRow();
            sal_Int32 c = pane_->getFirstVisibleColumn();
            if (r != last_row_ || c != last_col_) {
                last_row_ = r;
                last_col_ = c;
                changed = true;
            }
        }
        if (view_.is()) {
            Reference<css::sheet::XSpreadsheet> active = view_->getActiveSheet();
            Reference<XNamed> n(active, UNO_QUERY);
            std::string name = u2s(n->getName());
            if (name != last_sheet_) {
                last_sheet_ = name;
                changed = true;
            }
        }
    } catch (const css::uno::Exception& e) {
        OfficeLogDbg("[CalcLink] CheckViewportChanged failed: %s", u2s(e.Message).c_str());
        changed = true;  // 探测失败保守推帧
    }
    return changed;
}

bool CalcSession::Create(const char* path, const char* password, const char* guid, CalcFrameCallback cb, void* opaque, int width, int height) {
    cb_ = cb;
    opaque_ = opaque;
    target_w_ = (width > 0) ? width : link_utils::kDefaultWidth;
    target_h_ = (height > 0) ? height : link_utils::kDefaultHeight;

    OfficeLog("[CalcLink] Create begin this=%p path=%s", (void*)this, path ? path : "null");

    // P0: 平台工厂 + PrepareEnvironment (平台隔离设计: [platform-isolation] Part 2 C)
    platform_ = std::unique_ptr<LinkPlatform>(CreateCalcPlatform());
    if (!platform_) {
        OfficeLogErr("[CalcLink] CreateCalcPlatform failed");
        return false;
    }
    std::string calclinkDir = platform_->GetLinkDir();
    std::string desktopName = platform_->PrepareEnvironment(calclinkDir, guid ? guid : "");
    OfficeLog("[CalcLink] environment ready, desktop=%s", desktopName.c_str());

    // 获取平台策略 (数据驱动, 非 #ifdef)
    plan_ = platform_->Plan();

    // P1: BeginBoot (串行化引导+加载段, 经验 5; Linux 真锁 / Win 空)
    auto boot_section = platform_->BeginBoot();
    OfficeLog("[CalcLink] boot lock acquired this=%p", (void*)this);

    // P2: EnsureKernel + desktop_ + loader
    ctx_ = platform_->EnsureKernel();
    if (!ctx_.is()) {
        OfficeLogErr("[CalcLink] EnsureKernel failed");
        return false;
    }
    Reference<XMultiComponentFactory> factory = ctx_->getServiceManager();
    desktop_.set(factory->createInstanceWithContext("com.sun.star.frame.Desktop", ctx_), UNO_QUERY);
    Reference<XComponentLoader> loader(desktop_, UNO_QUERY);

    // P3: SnapshotWindows + Hidden 加载 (引导+加载须串行, 经验 5)
    platform_->SnapshotWindows();
    rtl::OUString docUrl;
    {
        rtl::OUString sysPath = s2u(path);
        if (osl::FileBase::getFileURLFromSystemPath(sysPath, docUrl) != osl::FileBase::E_None)
            return false;
    }
    css::uno::Sequence<css::beans::PropertyValue> loadProps(2);
    loadProps[0].Name = "Hidden";
    loadProps[0].Value <<= true;
    // ReadOnly: 播放为只读消费, 避免 LO 创建/校验源目录文档锁 (`.~lock.<name>#`),
    // 根除残留锁导致 loadComponentFromURL 静默返回 null 的缺陷 (缺陷报告)。
    loadProps[1].Name = "ReadOnly";
    loadProps[1].Value <<= true;
    component_ = loader->loadComponentFromURL(docUrl, "_blank", 0, loadProps);
    if (!component_.is()) {
        // 静默 null: 优先怀疑残留锁文件 (诊断)
        std::string lock = link_utils::GetLockFileIfExists(path);
        std::string msg = lock.empty()
            ? std::string("doc loaded FAILED (null); no lock file")
            : std::string("doc loaded FAILED (null); stale lock file detected: ") + lock;
        OfficeLogWarn("[CalcLink] %s", msg.c_str());
        return false;
    }
    OfficeLog("[CalcLink] doc loaded this=%p OK", (void*)this);

    Reference<XModel> model(component_, UNO_QUERY);
    controller_ = model->getCurrentController();
    frame_ = controller_->getFrame();

    // Unfreeze panes (frozen panes break row visibility/scrolling).
    {
        Reference<css::sheet::XViewFreezable> fz(controller_, UNO_QUERY);
        if (fz.is() && fz->hasFrozenPanes())
            fz->freezeAtPosition(0, 0);
    }

    // Hide headers/scrollbars/grid.
    Reference<XPropertySet> ps(controller_, UNO_QUERY);
    for (const char* name : { "HasColumnRowHeaders", "HasHorizontalScrollBar",
                              "HasVerticalScrollBar", "ShowGrid" }) {
        UNO_SILENT(ps->setPropertyValue(s2u(name), Any(false)), "CalcLink");
    }

    // 诊断 (ViewSettings 属性自省, ORT_DUMP_VIEW_PROPS=1): 枚举属性名排查用。
    // 注: 公式栏控制已定论 = .uno:InputLineVisible dispatch (经验 44, 下方 P9 后);
    // 属性表无公式栏项 (ShowFormulaBar 曾猜测无效, SDK IDL 无此名)。
    if (getenv("ORT_DUMP_VIEW_PROPS")) {
        try {
            Reference<css::beans::XPropertySetInfo> info = ps->getPropertySetInfo();
            css::uno::Sequence<css::beans::Property> props = info->getProperties();
            for (const auto& p : props) {
                std::string n = u2s(p.Name);
                if (n.find("ormula") != std::string::npos || n.find("nput") != std::string::npos ||
                    n.find("Bar") != std::string::npos)
                    OfficeLog("[CalcLink] view prop: %s", n.c_str());
            }
        } catch (const css::uno::Exception& e) {
            OfficeLogWarn("[CalcLink] view prop enum failed: %s", u2s(e.Message).c_str());
        }
    }

    // P4: [W@BeforeReveal] 绑定点 (calc/Win 反序定型 F 用例: setVisible 前找窗+定型)
    if (plan_.discover == WindowPoint::BeforeReveal) {
        if (!platform_->DiscoverWindow()) {
            OfficeLogErr("[CalcLink] DiscoverWindow (BeforeReveal) FAILED this=%p", (void*)this);
            return false;
        }
        OfficeLog("[CalcLink] window found (BeforeReveal) this=%p", (void*)this);
    }
    if (plan_.form == WindowPoint::BeforeReveal) {
        platform_->FormWindow(target_w_, target_h_);
    }

    // P5: setVisible 显露 (VCL 窗口在此时按最终形态创建; Win 反序已定型)
    Reference<css::awt::XWindow> xWin(frame_->getContainerWindow(), UNO_QUERY);
    if (xWin.is())
        xWin->setVisible(true);

    // Release 绑定点: P5 之后调用 (经验 5, 不得提前到 P3 之后; 窗口查找可并行)
    boot_section->Release();
    OfficeLog("[CalcLink] boot lock released this=%p", (void*)this);

    // P6: [W@AfterReveal] 绑定点 (calc/Linux: setVisible 后找窗+落位)
    if (plan_.discover == WindowPoint::AfterReveal) {
        if (!platform_->DiscoverWindow()) {
            OfficeLogErr("[CalcLink] DiscoverWindow (AfterReveal) FAILED this=%p", (void*)this);
            return false;
        }
        OfficeLog("[CalcLink] window found (AfterReveal) this=%p", (void*)this);
    }
    if (plan_.form == WindowPoint::AfterReveal) {
        platform_->FormWindow(target_w_, target_h_);
    }

    // Viewport control + sheets.
    pane_.set(controller_, UNO_QUERY);
    view_.set(controller_, UNO_QUERY);
    Reference<css::sheet::XSpreadsheetDocument> doc(component_, UNO_QUERY);
    sheets_ = doc->getSheets();

    // P9: HideUiBlock (plan_.ui_hide_needed 门控; setMenuBar 消除1px + hideElement 冗余兜底)
    if (plan_.ui_hide_needed) {
        link_utils::HideUiBlock(Reference<css::frame::XDispatchProvider>(desktop_, UNO_QUERY),
                                frame_, factory, ctx_);
    }

    // UI 修补 (公式栏等) 由平台实现自决, 见 [platform-isolation] §3.1
    platform_->HideUiExtras(frame_, factory, ctx_);

    // 阶段4: 初始化 FramePump (calc 策略参数见 [experiences] 经验42 表 / [framepump] §0)
    {
        FramePumpPlan pp;
        pp.tick_ms = 20;
        pp.heartbeat_ms = 100;
        pp.heartbeat_when_paused = true;  // calc 原 PollThread: 心跳无 paused_ 门控, Pause 照推
        pp.fail_backoff_ms = 200;
        pump_ = std::make_unique<FramePump>("calc", pp,
            [this]() { return PushFrame(); },
            [this]() { return CheckViewportChanged(); });  // 视口签名+脏位 probe
    }

    // 阶段4: 首帧 (UpdateFrame 任意状态有效, frame_mutex_ 串行)
    UpdateFrame();

    OfficeLog("[CalcLink] session created");
    OfficeLog("[CalcLink] Create DONE this=%p", (void*)this);
    return true;
}

void CalcSession::Destroy() {
    // V4 状态机: destroyed 终态幂等入口 (析构/重复调用只清理一次)
    if (destroyed_.exchange(true))
        return;
    OfficeLog("[CalcLink] Destroy begin this=%p", (void*)this);
    // 阶段4: pump_->Stop 不得持 mu_ (FramePump 契约); 锁外停止帧泵
    if (pump_)
        pump_->Stop();
    if (!ctx_.is()) {
        OfficeLog("[CalcLink] Destroy early (no ctx) this=%p", (void*)this);
        platform_.reset();
        pump_.reset();
        return;
    }
    OfficeLog("[CalcLink] closing component this=%p", (void*)this);
    try {
        Reference<XCloseable> close(component_, UNO_QUERY);
        if (close.is())
            close->close(false);
        OfficeLog("[CalcLink] component closed this=%p", (void*)this);
        // 平台隔离设计: terminate 按 plan_.terminate_on_destroy 门控 (数据驱动, 非 #ifdef)
        // Windows 每 session 独立 soffice 进程须 terminate 退出; Linux 共享内核不 terminate
        if (plan_.terminate_on_destroy && desktop_.is())
            desktop_->terminate();
    } catch (const css::uno::Exception& e) {
        OfficeLogDbg("[CalcLink] Destroy close/terminate failed: %s", u2s(e.Message).c_str());
    }
    component_.clear();
    controller_.clear();
    frame_.clear();
    pane_.clear();
    view_.clear();
    sheets_.clear();
    desktop_.clear();
    ctx_.clear();
    OfficeLog("[CalcLink] platform reset this=%p", (void*)this);
    if (platform_)
        platform_.reset(); // platform Cleanup (desktop/Xvfb)
    pump_.reset();  // 阶段4: pump_ 最后释放 (FrameFn/ChangeFn 捕获 this, 需保证 this 存活到 Stop)
    OfficeLog("[CalcLink] Destroy done this=%p", (void*)this);
    started_ = false;
}

// 阶段4: Start/Stop/Pause/Resume/UpdateFrame 委托 FramePump; 锁纪律: pump 方法不持 mu_
bool CalcSession::Start() {
    if (destroyed_)
        return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!component_.is())
            return false;
        started_ = true;
    }
    // 阶段0 P1 已由 FramePump::Start 契约承接 (Start=任何状态→Running 未暂停)
    if (pump_)
        pump_->Start();
    return true;
}

bool CalcSession::Stop() {
    if (destroyed_)
        return false;
    if (pump_)
        pump_->Stop();
    started_ = false;
    return true;
}

bool CalcSession::Pause() {
    if (destroyed_)
        return false;
    // 阶段4: calc 心跳 hbp=true, Pause 仅冻结视口 probe (CheckViewportChanged 仍调但不影响心跳)
    if (pump_)
        pump_->Pause();
    return true;
}

bool CalcSession::Resume() {
    if (destroyed_)
        return false;
    if (pump_)
        pump_->Resume();
    return true;
}

// 阶段4: UpdateFrame 委托 FramePump (同步立即帧, 任意状态有效; frame_mutex_ 串行)
// 原 P6 (停止后取帧黑屏) 由 FramePump 全状态 UpdateFrame 承接
bool CalcSession::UpdateFrame() {
    if (destroyed_ || !pump_)
        return false;
    return pump_->UpdateFrame();
}

bool CalcSession::NextPage() {
    if (destroyed_)
        return false;
    return ScrollPage(1);
}

bool CalcSession::PreviousPage() {
    if (destroyed_)
        return false;
    return ScrollPage(-1);
}

bool CalcSession::ScrollPage(int direction) {
    if (!pane_.is())  // V4: 销毁后 pane_ 已 clear, 裸解引用即断言崩溃
        return false;
    try {
        css::table::CellRangeAddress va = pane_->getVisibleRange();
        int rows = va.EndRow - va.StartRow + 1;
        return ScrollByRows(direction * rows);
    } catch (const css::uno::Exception& e) {
        OfficeLogDbg("[CalcLink] ScrollPage failed: %s", u2s(e.Message).c_str());
        return false;
    }
}

bool CalcSession::MoveScroll(int dx, int dy) {
    if (destroyed_)
        return false;
    bool ok = true;
    if (dx != 0)
        ok = ScrollByCols(dx) && ok;
    if (dy != 0)
        ok = ScrollByRows(dy) && ok;
    return ok;
}

bool CalcSession::SetSheet(unsigned index) {
    if (destroyed_)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!sheets_.is() || !view_.is())
        return false;
    try {
        Reference<XNameAccess> names(sheets_, UNO_QUERY);
        css::uno::Sequence<rtl::OUString> list = names->getElementNames();
        if (index >= static_cast<unsigned>(list.getLength()))
            return false;
        Reference<css::sheet::XSpreadsheet> sheet(names->getByName(list[index]), UNO_QUERY);
        view_->setActiveSheet(sheet);
        return true;
    } catch (const css::uno::Exception& e) {
        OfficeLogDbg("[CalcLink] SetSheet failed: %s", u2s(e.Message).c_str());
        return false;
    }
}

int CalcSession::GetSheetCount() {
    if (destroyed_)
        return 0;
    if (!sheets_.is())
        return 0;
    try {
        Reference<XNameAccess> names(sheets_, UNO_QUERY);
        return static_cast<int>(names->getElementNames().getLength());
    } catch (const css::uno::Exception& e) {
        OfficeLogDbg("[CalcLink] GetSheetCount failed: %s", u2s(e.Message).c_str());
        return 0;
    }
}

int CalcSession::GetCurrentSheet() {
    if (destroyed_)
        return 0;
    if (!view_.is())
        return 0;
    try {
        Reference<css::sheet::XSpreadsheet> active = view_->getActiveSheet();
        Reference<XNamed> n(active, UNO_QUERY);
        std::string cur = u2s(n->getName());
        Reference<XNameAccess> names(sheets_, UNO_QUERY);
        css::uno::Sequence<rtl::OUString> list = names->getElementNames();
        for (sal_Int32 i = 0; i < list.getLength(); ++i) {
            if (u2s(list[i]) == cur)
                return static_cast<int>(i);
        }
        return 0;
    } catch (const css::uno::Exception& e) {
        OfficeLogDbg("[CalcLink] GetCurrentSheet failed: %s", u2s(e.Message).c_str());
        return 0;
    }
}

int CalcSession::GetWidth() {
    if (destroyed_)
        return 0;
    return width_;
}

int CalcSession::GetHeight() {
    if (destroyed_)
        return 0;
    return height_;
}

bool CalcSession::SetResolution(int width, int height) {
    if (destroyed_)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!platform_ || width <= 0 || height <= 0)
        return false;
    // V2 节流: 50ms 内重复 resize 跳过 (高频风暴合并)
    auto now = std::chrono::steady_clock::now();
    if (now - last_resize_ < std::chrono::milliseconds(50)) {
        target_w_ = width;
        target_h_ = height;
        return true;
    }
    last_resize_ = now;
    target_w_ = width;
    target_h_ = height;
    return platform_->SetWindowSize(width, height);
}

bool CalcSession::SetScale(unsigned percent) {
    if (destroyed_)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!controller_.is())
        return false;
    if (percent < 10 || percent > 400)
        return false;
    try {
        // ZoomValue on the controller is the view zoom percentage.
        Reference<XPropertySet> ps(controller_, UNO_QUERY);
        ps->setPropertyValue(s2u("ZoomValue"), Any(sal_Int32(percent)));
        return true;
    } catch (const css::uno::Exception& e) {
        OfficeLogWarn("[CalcLink] SetScale failed: %s", u2s(e.Message).c_str());
        return false;
    }
}

unsigned CalcSession::GetScale() {
    if (destroyed_)
        return 0;
    if (!controller_.is())
        return 0;
    try {
        Reference<XPropertySet> ps(controller_, UNO_QUERY);
        sal_Int32 zoom = 0;
        ps->getPropertyValue(s2u("ZoomValue")) >>= zoom;
        return static_cast<unsigned>(zoom);
    } catch (const css::uno::Exception& e) {
        OfficeLogDbg("[CalcLink] GetScale failed: %s", u2s(e.Message).c_str());
        return 0;
    }
}
