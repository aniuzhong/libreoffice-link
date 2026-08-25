// impress_session.cpp — Impress 会话实现。
// 共享内核 Hidden 加载 + 窗口化放映 + 平台层缩窗进 slot 后轮询抓帧;
// 流程与平台差异 (Windows 独立 soffice 全屏, 经验 39) 见 [platform-isolation] Part2 C。
#include "session.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

#include <com/sun/star/awt/XTopWindow.hpp>
#include <com/sun/star/awt/XWindow.hpp>
#include <com/sun/star/beans/PropertyValue.hpp>
#include <com/sun/star/beans/XFastPropertySet.hpp>
#include <com/sun/star/beans/XPropertySet.hpp>
#include <com/sun/star/frame/FrameSearchFlag.hpp>
#include <com/sun/star/frame/XDispatch.hpp>
#include <com/sun/star/frame/XDispatchProvider.hpp>
#include <com/sun/star/frame/XComponentLoader.hpp>
#include <com/sun/star/frame/XDesktop.hpp>
#include <com/sun/star/frame/XLayoutManager.hpp>
#include <com/sun/star/frame/XModel.hpp>
#include <com/sun/star/lang/XMultiComponentFactory.hpp>
#include <com/sun/star/presentation/XPresentationSupplier.hpp>
#include <com/sun/star/util/URL.hpp>
#include <com/sun/star/util/XCloseable.hpp>
#include <com/sun/star/util/XURLTransformer.hpp>
#include <cppuhelper/bootstrap.hxx>
#include <osl/file.hxx>

#include <base/log.h> // OfficeLog (Linux 实现 office_runtime, Windows 实现 common)
#include "link_utils.h" // u2s/s2u/HideUiBlock (公共会话工具, 经验 32 平台归组)

using css::uno::Reference;
using css::uno::UNO_QUERY;
using css::uno::Any;

using link_utils::u2s;
using link_utils::s2u;

ImpressSession::ImpressSession() = default;

ImpressSession::~ImpressSession() {
    Destroy();
}

void ImpressSession::Destroy() {
    // V4 状态机: destroyed 终态幂等入口 (析构/重复调用只清理一次)
    if (destroyed_.exchange(true))
        return;
    OfficeLog("[ImpressLink] Destroy() BEGIN this=%p created=%d slideshow=%d platform=%p",
              (void*)this, (int)created_, (int)slideshow_.is(), (void*)platform_.get());
    // 阶段2: pump_->Stop 不得持 mu_ (FramePump 契约); 在 mu_ 外停止帧泵
    if (pump_)
        pump_->Stop();
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (slideshow_.is())
            UNO_SILENT(slideshow_->pause(), "ImpressLink");
        // 组件关闭 (共享内核: 只关文档, 不动内核)
        if (component_.is()) {
            try {
                Reference<css::util::XCloseable> closeable(component_, UNO_QUERY);
                if (closeable.is())
                    closeable->close(false);
                else
                    component_->dispose();
            } catch (const css::uno::Exception& e) {
                OfficeLogDbg("[ImpressLink] close component failed: %s", u2s(e.Message).c_str());
            }
        }
        
        // 平台隔离设计: terminate 按 plan_.terminate_on_destroy 门控 (数据驱动)
        // Windows 每 session 独立 soffice 进程须 terminate 退出; Linux 共享内核不 terminate
        if (plan_.terminate_on_destroy && desktop_.is())
            desktop_->terminate();
        
        // 平台隔离设计: OnSessionEnd 钩子 (当前空; 平台级会话结束清理扩展点)
        if (platform_)
            platform_->OnSessionEnd();
        
        component_.clear();
        controller_.clear();
        frame_.clear();
        presentation_.clear();
        slideshow_.clear();
        desktop_.clear();
    }
    if (platform_)
        platform_->Cleanup();
    platform_.reset();
    pump_.reset();
    created_ = false;
    OfficeLog("[ImpressLink] session destroyed this=%p", (void*)this);
}

bool ImpressSession::Create(const char* path, const char* password, const char* guid, ImpressFrameCallback cb, void* opaque, int width, int height) {
    if (!path || !path[0] || !cb)
        return false;
    cb_ = cb;
    opaque_ = opaque;
    target_w_ = (width > 0) ? width : link_utils::kDefaultWidth;
    target_h_ = (height > 0) ? height : link_utils::kDefaultHeight;
    OfficeLog("[ImpressLink] Create path=%s guid=%s", path, guid ? guid : "");

    platform_.reset(CreateImpressPlatform());
    if (!platform_) {
        OfficeLogErr("[ImpressLink] CreateImpressPlatform failed (unsupported platform?)");
        return false;
    }

    // P0: 平台工厂 + PrepareEnvironment (平台隔离设计: [platform-isolation] Part 2 C)
    std::string linkDir = platform_->GetLinkDir();
    platform_->PrepareEnvironment(linkDir, guid ? guid : "");

    // 获取平台策略 (数据驱动, 非 #ifdef; 存成员供 Destroy 消费 terminate_on_destroy)
    plan_ = platform_->Plan();

    // P1: BeginBoot (串行化引导+加载段, 经验 5)
    auto boot_section = platform_->BeginBoot();
    OfficeLog("[ImpressLink] boot lock acquired this=%p", (void*)this);

    // P2: EnsureKernel
    ctx_ = platform_->EnsureKernel();
    if (!ctx_.is()) {
        OfficeLogErr("[ImpressLink] EnsureKernel failed");
        return false;
    }

    Reference<css::lang::XMultiComponentFactory> factory = ctx_->getServiceManager();
    desktop_.set(factory->createInstanceWithContext("com.sun.star.frame.Desktop", ctx_), UNO_QUERY);
    Reference<css::frame::XComponentLoader> loader(desktop_, UNO_QUERY);

    // P3: SnapshotWindows + Hidden 加载 (引导+加载须串行, 经验 5)
    platform_->SnapshotWindows();

    rtl::OUString docUrl;
    {
        rtl::OUString sysPath = s2u(path);
        if (osl::FileBase::getFileURLFromSystemPath(sysPath, docUrl) != osl::FileBase::E_None)
            return false;
    }
    // ReadOnly: 播放为只读消费, 避免 LO 在源目录创建/校验文档锁 (`.~lock.<name>#`),
    // 根除残留锁导致 loadComponentFromURL 静默返回 null 的缺陷 (缺陷报告)。
    css::uno::Sequence<css::beans::PropertyValue> loadProps(2);
    loadProps[0].Name = "Hidden";
    loadProps[0].Value <<= true;
    loadProps[1].Name = "ReadOnly";
    loadProps[1].Value <<= true;
    try {
        component_ = loader->loadComponentFromURL(docUrl, "_blank", 0, loadProps);
    } catch (const css::uno::Exception& e) {
        OfficeLogErr("[ImpressLink] loadComponentFromURL failed: %s", u2s(e.Message).c_str());
        return false;
    }
    if (!component_.is()) {
        // 静默 null: 优先怀疑残留锁文件 (诊断, 缺陷报告短期项)
        std::string lock = link_utils::GetLockFileIfExists(path);
        std::string msg = lock.empty()
            ? std::string("doc loaded FAILED (null); no lock file")
            : std::string("doc loaded FAILED (null); stale lock file detected: ") + lock;
        OfficeLogWarn("[ImpressLink] %s", msg.c_str());
        return false;
    }
    OfficeLog("[ImpressLink] doc loaded this=%p OK", (void*)this);

    Reference<css::frame::XModel> model(component_, UNO_QUERY);
    controller_ = model->getCurrentController();
    frame_ = controller_->getFrame();

    // P4: [W@BeforeReveal] 绑定点 (根据 plan.discover 决定是否发现窗口)
    if (plan_.discover == WindowPoint::BeforeReveal) {
        if (!platform_->DiscoverWindow()) {
            OfficeLogErr("[ImpressLink] DiscoverWindow (BeforeReveal) failed");
            return false;
        }
    }

    // P5: setVisible 显露 (VCL 窗口在此时按最终形态创建)
    Reference<css::awt::XWindow> xWin(frame_->getContainerWindow(), UNO_QUERY);
    if (xWin.is())
        xWin->setVisible(true);

    // Release 绑定点: P5 之后调用 (经验 5, 不得提前到 P3 之后)
    boot_section->Release();
    OfficeLog("[ImpressLink] boot lock released this=%p", (void*)this);

    // P6: [W@AfterReveal] 绑定点 (根据 plan.discover 决定是否发现窗口)
    if (plan_.discover == WindowPoint::AfterReveal) {
        if (!platform_->DiscoverWindow()) {
            OfficeLogErr("[ImpressLink] DiscoverWindow (AfterReveal) failed");
            return false;
        }
    }

    // P7: 放映属性 + start + 等 settle_ms + controller+pause (impress 专属)
    try {
        Reference<css::presentation::XPresentationSupplier> sup(component_, UNO_QUERY);
        auto pres = sup->getPresentation();
        Reference<css::beans::XPropertySet> pps(pres, UNO_QUERY);
        pps->setPropertyValue("AllowAnimations", Any(true));
        pps->setPropertyValue("IsAlwaysOnTop", Any(false));
        pps->setPropertyValue("IsAutomatic", Any(false));
        pps->setPropertyValue("IsEndless", Any(true));
        pps->setPropertyValue("IsFullScreen", Any(plan_.fullscreen));
        OfficeLog("[ImpressLink] IsFullScreen=%d (plan_.fullscreen)", plan_.fullscreen ? 1 : 0);
        pps->setPropertyValue("IsMouseVisible", Any(false));
        pps->setPropertyValue("StartWithNavigator", Any(false));
        pps->setPropertyValue("UsePen", Any(false));
        presentation_ = Reference<css::presentation::XPresentation2>(pres, UNO_QUERY);
        presentation_->start();
        
        // 等 settle_ms (形态稳定等待, plan.settle_ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(plan_.settle_ms));
        
        slideshow_ = presentation_->getController();
        if (!slideshow_.is()) {
            OfficeLogErr("[ImpressLink] no slide show controller");
            return false;
        }
        slideshow_->pause();
        page_count_ = slideshow_->getSlideCount();
        current_page_ = 0;  // 初始在第 0 页
        OfficeLog("[ImpressLink] slideshow started, slides=%d this=%p", page_count_, (void*)this);
    } catch (const css::uno::Exception& e) {
        OfficeLogErr("[ImpressLink] slideshow start failed: %s", u2s(e.Message).c_str());
        return false;
    }

    // P8: [W@AfterStart] 绑定点 — plan.discover / plan.form 各自独立消费
    // (协议 Part 2 C: impress/Win discover=AfterStart 发现放映窗口 (start 后创建,
    //   SALTMPSUBFRAME); impress/Linux form=AfterStart slot 落位; 
    //   Windows 探针实测: 缺 discover 分支则 hwnd_ 空 -> 抓帧 0 帧)
    if (plan_.discover == WindowPoint::AfterStart) {
        if (!platform_->DiscoverWindow()) {
            OfficeLogErr("[ImpressLink] DiscoverWindow (AfterStart) failed");
            return false;
        }
    }
    if (plan_.form == WindowPoint::AfterStart) {
        if (!platform_->FormWindow(target_w_, target_h_)) {
            OfficeLogErr("[ImpressLink] FormWindow (AfterStart) failed");
            return false;
        }
    }

    // P9: HideUiBlock (根据 plan.ui_hide_needed 门控)
    if (plan_.ui_hide_needed) {
        link_utils::HideUiBlock(
            Reference<css::frame::XDispatchProvider>(frame_, UNO_QUERY),
            frame_, ctx_->getServiceManager(), ctx_);
    }

    // 平台特定的 UI 修补 (平台隔离设计: 平台专属 UI 处理由平台实现自决)。
    // Linux: LO Xvfb 无头环境 UI 默认 vis=0, HideUiExtras 空操作;
    // Windows: InputLineVisible dispatch 等 (win_platform.cpp HideUiExtras)。
    platform_->HideUiExtras(frame_, ctx_->getServiceManager(), ctx_);

    // 阶段2: 初始化 FramePump (统一帧泵, 替代原 poll_thread_/paused_/force_frame_)
    // impress 策略: 无 probe (changed=nullptr 恒真), 无心跳 (heartbeat=0), tick=40ms
    {
        FramePumpPlan pp;
        pp.tick_ms = 40;
        pp.heartbeat_ms = 0;
        pp.heartbeat_when_paused = false;
        pp.fail_backoff_ms = 200;
        pump_ = std::make_unique<FramePump>("impress", pp,
            [this]() { return PushFrame(); });
    }

    // P10: UpdateFrame 首帧
    UpdateFrame();

    created_ = true;
    return true;
}

bool ImpressSession::PushFrame() {
    if (destroyed_ || !platform_ || !cb_)  // V4: Destroy 窗口期帧泵线程安全退出
        return false;
    uint8_t* pixels = nullptr;
    int w = 0, h = 0;
    if (!platform_->CaptureFrame(pixels, w, h))
        return false;
    width_ = w;
    height_ = h;
    cb_(pixels, w, h, w * 4, w * h * 4, link_utils::kFrameFormatBGRA, opaque_);
    // 后台校准: 每帧用 LO 真实值更新缓存 (无锁, 原子读 current_page_ 的线程安全由
    // PushFrame 在 frame_mutex_ 内保证; SyncCurrentPage 与 NextPage/GoToPage 的
    // mu_ 可能冲突, 但 current_page_ 是 int, 写偏一点不影响正确性)。
    SyncCurrentPage();
    return true;
}

// 后台校准: 用 LO 真实值更新缓存。PushFrame 中每帧调用, 保证缓存最终与 LO 一致。
// 策略:
//   - LO 值 > 缓存: 无条件接受 (翻页推进, SyncCurrentPage 比 NextPage 更晚看到结果)
//   - LO 值 < 缓存: 仅在距离上次导航超过 500ms 后接受 (避免效果进行中 LO 返回旧值
//     覆盖乐观推进; 500ms 后效果应已完成, LO 值可信)
//   - LO 值 == 缓存: 不动
// 不持 mu_ (PushFrame 在 frame_mutex_ 内, 不能反向持 mu_), 因此与 NextPage/GoToPage
// 可能并发写 current_page_。int 写入是原子的 (x86), 短暂不一致不影响正确性。
void ImpressSession::SyncCurrentPage() {
    if (destroyed_ || !slideshow_.is())
        return;
    try {
        int actual = slideshow_->getCurrentSlideIndex();
        if (actual >= 0 && actual < page_count_ && actual != current_page_) {
            auto now = std::chrono::steady_clock::now();
            bool recent_nav = (now - last_nav_) < std::chrono::milliseconds(500);
            if (actual > current_page_ || !recent_nav)
                current_page_ = actual;
        }
    } catch (...) {
        // 静默: 校准失败不影响主流程
    }
}

// 阶段2: Start/Stop/Pause/Resume 委托 FramePump; 锁纪律: pump 方法不持 mu_
bool ImpressSession::Start() {
    if (destroyed_)
        return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!created_ || !slideshow_.is()) {
            OfficeLog("[ImpressLink] Start FAIL: created=%d slideshow=%d", (int)created_, (int)slideshow_.is());
            return false;
        }
        UNO_SILENT(slideshow_->resume(), "ImpressLink");
    }
    if (pump_)
        pump_->Start();
    return true;
}

bool ImpressSession::Stop() {
    if (destroyed_)
        return false;
    if (pump_)
        pump_->Stop();
    std::lock_guard<std::mutex> lk(mu_);
    if (!created_ || !slideshow_.is())
        return false;
    UNO_SILENT(slideshow_->pause(), "ImpressLink");
    return true;
}

bool ImpressSession::Pause() {
    if (destroyed_)
        return false;
    if (pump_)
        pump_->Pause();
    std::lock_guard<std::mutex> lk(mu_);
    if (!created_ || !slideshow_.is()) {
        OfficeLog("[ImpressLink] Pause FAIL: created=%d slideshow=%d", (int)created_, (int)slideshow_.is());
        return false;
    }
    UNO_SILENT(slideshow_->pause(), "ImpressLink");
    return true;
}

bool ImpressSession::Resume() {
    if (destroyed_)
        return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!created_ || !slideshow_.is())
            return false;
        UNO_SILENT(slideshow_->resume(), "ImpressLink");
    }
    if (pump_)
        pump_->Resume();
    return true;
}

// 阶段2: UpdateFrame 委托 FramePump (同步立即帧, 任意状态有效; frame_mutex_ 串行)
bool ImpressSession::UpdateFrame() {
    if (destroyed_ || !pump_)
        return false;
    return pump_->UpdateFrame();
}

bool ImpressSession::SetResolution(int width, int height) {
    if (destroyed_)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!platform_ || width <= 0 || height <= 0)
        return false;
    // V2 节流: 50ms 内重复 resize 跳过 (高频风暴合并; 正常使用不会 50ms 内连改两次)
    auto now = std::chrono::steady_clock::now();
    if (now - last_resize_ < std::chrono::milliseconds(50)) {
        target_w_ = width;  // 记录意图, 下次生效
        target_h_ = height;
        return true;
    }
    last_resize_ = now;
    target_w_ = width;
    target_h_ = height;
    return platform_->SetWindowSize(width, height);
}

bool ImpressSession::NextPage() {
    if (destroyed_)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!slideshow_.is()) {
        OfficeLog("[ImpressLink] NextPage FAIL: slideshow not valid, created=%d", (int)created_);
        return false;
    }
    try {
        OfficeLog("[ImpressLink] NextPage: gotoNextEffect() called, pump_running=%d",
                  (int)(pump_ ? pump_->running() : false));
        // 效果级推进: gotoNextEffect 推进一个效果 (翻页或同页内效果)。
        // LO 的 getCurrentSlideIndex() 在 gotoNextEffect 返回后可能异步更新
        // (通过 UNO 事件循环), 立即查询可能读到旧值。采用乐观策略:
        //   - 如果 LO 返回值变了 → 用 LO 的 (翻页)
        //   - 如果 LO 返回值没变 → 乐观推进 (效果进行中, UI 先看到变化)
        // SyncCurrentPage 在 PushFrame 中每帧校准, 最终与 LO 一致。
        // 时间抑制 (last_nav_) 防止 SyncCurrentPage 在 500ms 内用 LO 旧值回退。
        int before = slideshow_->getCurrentSlideIndex();
        slideshow_->gotoNextEffect();
        int after = slideshow_->getCurrentSlideIndex();
        if (after >= 0 && after < page_count_ && after != before)
            current_page_ = after;  // LO 已更新
        else if (current_page_ < page_count_ - 1)
            current_page_++;  // 乐观推进
        last_nav_ = std::chrono::steady_clock::now();
        OfficeLog("[ImpressLink] NextPage: gotoNextEffect() OK, before=%d after=%d current_page=%d",
                  before, after, current_page_);
        return true;
    } catch (const css::uno::Exception& e) {
        OfficeLogWarn("[ImpressLink] gotoNextEffect failed: %s", u2s(e.Message).c_str());
        return false;
    }
}

bool ImpressSession::PreviousPage() {
    if (destroyed_)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!slideshow_.is())
        return false;
    try {
        int before = slideshow_->getCurrentSlideIndex();
        slideshow_->gotoPreviousEffect();
        int after = slideshow_->getCurrentSlideIndex();
        if (after >= 0 && after < page_count_ && after != before)
            current_page_ = after;
        else if (current_page_ > 0)
            current_page_--;
        last_nav_ = std::chrono::steady_clock::now();
        return true;
    } catch (const css::uno::Exception& e) {
        OfficeLogWarn("[ImpressLink] gotoPreviousEffect failed: %s", u2s(e.Message).c_str());
        return false;
    }
}

bool ImpressSession::GoToPage(int page) {
    if (destroyed_)
        return false;
    if (page < 0 || page >= page_count_)
        return false;  // 越界拒绝, 避免 LO 内部 clamp 导致缓存与真实状态脱节
    std::lock_guard<std::mutex> lk(mu_);
    if (!slideshow_.is())
        return false;
    try {
        slideshow_->gotoSlideIndex(page);
        current_page_ = page;  // gotoSlideIndex 是同步跳转, 直接设缓存
        return true;
    } catch (const css::uno::Exception& e) {
        OfficeLogWarn("[ImpressLink] gotoSlideIndex(%d) failed: %s", page, u2s(e.Message).c_str());
        return false;
    }
}

int ImpressSession::GetCurrentPage() {
    if (destroyed_)
        return -1;
    // 返回本地缓存, 不依赖 LO 异步查询。NextPage/PreviousPage/GoToPage 在 mu_ 内
    // 同步调用 LO 后立即更新缓存, 保证缓存与 LO 真实状态一致。
    return current_page_;
}

int ImpressSession::GetPageCount() {
    if (destroyed_)
        return -1;
    if (!slideshow_.is())
        return -1;
    try {
        return slideshow_->getSlideCount();
    } catch (const css::uno::Exception& e) {
        OfficeLogDbg("[ImpressLink] getSlideCount failed: %s", u2s(e.Message).c_str());
        return page_count_;
    }
}

int ImpressSession::GetWidth() {
    if (destroyed_)
        return 0;
    return width_;
}

int ImpressSession::GetHeight() {
    if (destroyed_)
        return 0;
    return height_;
}

// 静音专项 (方案 A): UNO remote ctx -> Manager_FFPlay -> SetMuteAll (按 window_id
// 物理句柄过滤, 替代 session_id 时序归属); 完整机制/路径/后端无关性见 [ffplay-embed] §7。
bool ImpressSession::SetMute(bool mute) {
    if (destroyed_)
        return false;
    if (!ctx_.is()) {
        OfficeLog("[ImpressLink] SetMute(%s) FAIL: ctx_ null", mute ? "true" : "false");
        return false;
    }
    if (!platform_) {
        OfficeLog("[ImpressLink] SetMute(%s) FAIL: platform_ null", mute ? "true" : "false");
        return false;
    }
    try {
        auto sm = ctx_->getServiceManager();
        if (!sm.is()) {
            OfficeLog("[ImpressLink] SetMute(%s) FAIL: ServiceManager null",
                      mute ? "true" : "false");
            return false;
        }
        Reference<css::beans::XFastPropertySet> fps(
            sm->createInstanceWithContext(
                rtl::OUString("com.sun.star.comp.avmedia.Manager_FFPlay"), ctx_),
            css::uno::UNO_QUERY);
        if (!fps.is()) {
            OfficeLog("[ImpressLink] SetMute(%s) FAIL: Manager_FFPlay no XFastPropertySet "
                      "(ffplay backend absent?)", mute ? "true" : "false");
            return false;
        }
        // 方案 A: 传 {window_ids, mute} — window_ids = 本 session 放映窗口下所有子窗口
        std::vector<long> media_wids = platform_->GetMediaWindowIds();
        css::uno::Sequence<sal_Int32> wid_seq(media_wids.size());
        for (size_t i = 0; i < media_wids.size(); i++)
            wid_seq[i] = static_cast<sal_Int32>(media_wids[i]);
        css::uno::Sequence<css::uno::Any> args(2);
        args[0] = css::uno::Any(wid_seq);
        args[1] = css::uno::Any(static_cast<sal_Bool>(mute));
        fps->setFastPropertyValue(0, css::uno::Any(args)); // handle 0 = MGR_PROP_MUTE_WINDOWS
        OfficeLog("[ImpressLink] SetMute(%s) -> UNO Manager_FFPlay wids=%zu OK",
                  mute ? "true" : "false", media_wids.size());
        return true;
    } catch (const css::uno::Exception& e) {
        OfficeLog("[ImpressLink] SetMute(%s) EXC: %s",
                  mute ? "true" : "false",
                  link_utils::u2s(e.Message).c_str());
        return false;
    }
}
