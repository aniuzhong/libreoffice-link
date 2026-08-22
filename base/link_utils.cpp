// link_utils.cpp — 跨平台会话工具实现 (见 link_utils.h)
#include "link_utils.h"

#include <com/sun/star/awt/XTopWindow.hpp>
#include <com/sun/star/awt/XWindow.hpp>
#include <com/sun/star/frame/FrameSearchFlag.hpp>
#include <com/sun/star/frame/XDispatch.hpp>
#include <com/sun/star/frame/XLayoutManager.hpp>
#include <com/sun/star/util/URL.hpp>
#include <com/sun/star/util/XURLTransformer.hpp>
#include <com/sun/star/beans/PropertyValue.hpp>
#include <osl/thread.hxx>
#include <rtl/string.hxx>

#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <cppuhelper/bootstrap.hxx>
#else
#include "../runtime/runtime.h" // KernelHost Linux 实现 (G 缝)
#include <cstdlib> // setenv
#endif

#include "office_paths.h" // KernelHost Windows profile 路径 (G 缝)

#include <filesystem>

#include "log.h" // OfficeLog (声明下沉 common; Linux 实现唯一在 office_runtime.so,
                 // Windows 实现唯一在 common (win_office_log.cpp))

namespace link_utils {

std::string u2s(const rtl::OUString& s) {
    rtl::OString o = rtl::OUStringToOString(s, RTL_TEXTENCODING_UTF8);
    return std::string(o.getStr(), o.getLength());
}

rtl::OUString s2u(const std::string& s) {
    rtl::OString o(s.c_str(), static_cast<sal_Int32>(s.size()));
    return rtl::OStringToOUString(o, RTL_TEXTENCODING_UTF8);
}

#ifdef _WIN32

std::string GetLinkDir() {
    char buf[MAX_PATH] = { 0 };
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&GetLinkDir), &self);
    if (!self)
        self = GetModuleHandleA(nullptr);
    GetModuleFileNameA(self ? self : GetModuleHandleA(nullptr), buf, MAX_PATH);
    char* slash = strrchr(buf, '\\');
    if (slash)
        *slash = 0;
    return buf;
}

css::uno::Reference<css::uno::XComponentContext> BootstrapSession(
    const std::string& link_dir, const std::string& profile, const std::string& desktop) {
    try {
        return cppu::bootstrap(s2u(link_dir), s2u(profile), s2u(desktop));
    } catch (const cppu::BootstrapException& e) {
        OfficeLogErr("[Common.Boot] bootstrap failed (link_dir=%s profile=%s desktop=%s): %s",
                  link_dir.c_str(), profile.c_str(), desktop.c_str(),
                  u2s(e.getMessage()).c_str());
        return nullptr;
    }
}

std::wstring u2w(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(len > 0 ? len - 1 : 0, 0);
    if (len > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], len);
    return out;
}

#else  // !_WIN32 (Linux: 共享内核路径, 见头文件注释)

std::string GetLinkDir() {
    return std::string();
}

css::uno::Reference<css::uno::XComponentContext> BootstrapSession(
    const std::string&, const std::string&, const std::string&) {
    return nullptr;
}

#endif

// UI 元素表 (自省 + 隐藏共用): 覆盖 LO 各文档类型的可见 UI 元素。
// 2026-08-14 补齐: Impress 的绘图工具栏 (drawbar, 顶部椭圆/矩形图标) 与
// Sidebar Properties 面板 (PropertiesDeck, 右侧 Layout/Slide 属性) ——
// 此前只隐藏 menubar, 这两个残留 (用户实测)。
namespace {
// UI 隐藏时序 (HideUiBlock 使用; 经验 26/32)
constexpr int kUiSettleMs = 1000;   // setMenuBar(null) 前等 UI 稳定
constexpr int kHideRetryMs = 500;   // hideElement 轮间间隔 (等 UI 异步构建)
constexpr int kHideRounds = 4;      // hideElement 重试轮数

struct UiElem { const char* name; const char* label; };
// 注: 公式栏 (fx/Σ 输入行) 不在本表 — 它是 SFX docking window 而非 LayoutManager
// toolbar 元素, hideElement 无效; 真实控制 = .uno:InputLineVisible dispatch
// (calc_session P9 后, 经验 44)。留表内条目会误导排查。
const UiElem kUiElems[] = {
    { "private:resource/menubar/menubar", "menubar" },
    { "private:resource/toolbar/standardbar", "toolbar_std" },
    { "private:resource/toolbar/formatbar", "toolbar_fmt" },
    { "private:resource/toolbar/drawbar", "toolbar_draw" },      // Impress/Draw 绘图工具栏
    { "private:resource/statusbar/statusbar", "statusbar" },
    { "private:resource/sidebar/Sidebar", "sidebar" },
    { "private:resource/sidebar/PropertiesDeck", "sidebar_props" }, // 右侧 Layout/Slide 属性面板
};
}

void DumpUiState(const css::uno::Reference<css::frame::XFrame>& frame,
                 const css::uno::Reference<css::lang::XMultiComponentFactory>& factory,
                 const css::uno::Reference<css::uno::XComponentContext>& ctx,
                 const char* tag) {
    using css::uno::Reference;
    using css::uno::UNO_QUERY;
    // 1. frame 激活态 (UNO XWindow 无 isVisible, 可见性只能靠窗口尺寸/像素层自省)
    try {
        OfficeLogDbg("[Common.UIHide] state[%s]: frame_active=%d",
                  tag, frame->isActive() ? 1 : 0);
    } catch (const css::uno::Exception&) {
        OfficeLogDbg("[Common.UIHide] state[%s]: frame 查询失败", tag);
    }
    // 2. LayoutManager 各 UI 元素可见状态
    try {
        Reference<css::frame::XLayoutManager> lm(
            factory->createInstanceWithContext("com.sun.star.frame.LayoutManager", ctx), UNO_QUERY);
        if (!lm.is()) {
            OfficeLogDbg("[Common.UIHide] state[%s]: no LayoutManager service", tag);
            return;
        }
        lm->attachFrame(frame);
        for (const auto& e : kUiElems) {
            try {
                OfficeLogDbg("[Common.UIHide] state[%s]: %s=%d", tag, e.label,
                          lm->isElementVisible(rtl::OUString::createFromAscii(e.name)) ? 1 : 0);
            } catch (const css::uno::Exception&) {
                OfficeLogWarn("[Common.UIHide] state[%s]: %s=ERR", tag, e.label);
            }
        }
    } catch (const css::uno::Exception&) {
        OfficeLogDbg("[Common.UIHide] state[%s]: LayoutManager 查询失败", tag);
    }
}

void HideUiBlock(const css::uno::Reference<css::frame::XDispatchProvider>& prov,
                 const css::uno::Reference<css::frame::XFrame>& frame,
                 const css::uno::Reference<css::lang::XMultiComponentFactory>& factory,
                 const css::uno::Reference<css::uno::XComponentContext>& ctx) {
    using css::uno::Reference;
    using css::uno::UNO_QUERY;
    // 0. 自省: 隐藏前 UI 状态 (日志驱动排查, state[before]/state[after] 对比)
    DumpUiState(frame, factory, ctx, "before");
    // 1. FullScreen dispatch (隐藏 LO 内部全屏态)
    if (prov.is()) {
        css::util::URL url;
        url.Complete = rtl::OUString(".uno:FullScreen");
        try {
            Reference<css::util::XURLTransformer> tr(
                factory->createInstanceWithContext("com.sun.star.util.URLTransformer", ctx), UNO_QUERY);
            if (tr.is())
                tr->parseStrict(url);
        } catch (const css::uno::Exception& e) {
            OfficeLogDbg("[Common.UIHide] URLTransformer failed: %s", u2s(e.Message).c_str());
        }
        try {
            Reference<css::frame::XDispatch> disp =
                prov->queryDispatch(url, rtl::OUString(), css::frame::FrameSearchFlag::SELF);
            OfficeLogDbg("[Common.UIHide] FullScreen dispatcher %s", disp.is() ? "found" : "NOT found");
            if (disp.is())
                disp->dispatch(url, css::uno::Sequence<css::beans::PropertyValue>());
        } catch (const css::uno::Exception& e) {
            OfficeLogWarn("[Common.UIHide] FullScreen dispatch error: %s", u2s(e.Message).c_str());
        }
    }
    // 2. setMenuBar(null) (消除旧 user 配置的 UI 状态残留, 经验 26)
    Reference<css::awt::XTopWindow> top(frame->getContainerWindow(), UNO_QUERY);
    Reference<css::awt::XWindow> cont(frame->getContainerWindow(), UNO_QUERY);
    if (top.is()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kUiSettleMs));
        // 诊断 (menubar 隐藏排查, 2026-08-17): 容器窗口形态 (尺寸变化 =
        // 菜单栏/状态栏布局被移除的佐证)
        try {
            css::awt::Rectangle r = cont->getPosSize();
            OfficeLogDbg("[Common.UIHide] before setMenuBar: container pos=%d,%d size=%dx%d",
                      r.X, r.Y, r.Width, r.Height);
        } catch (const css::uno::Exception&) {
            OfficeLogDbg("[Common.UIHide] before setMenuBar: container query failed");
        }
        top->setMenuBar(nullptr);
        OfficeLogDbg("[Common.UIHide] setMenuBar(null) called");
        try {
            css::awt::Rectangle r = cont->getPosSize();
            OfficeLogDbg("[Common.UIHide] after setMenuBar: container size=%dx%d",
                      r.Width, r.Height);
        } catch (const css::uno::Exception& e) {
            OfficeLogDbg("[Common.UIHide] after setMenuBar: container query failed: %s", u2s(e.Message).c_str());
        }
    } else {
        OfficeLogDbg("[Common.UIHide] no XTopWindow on container window");
    }
    // 3. LayoutManager 隐藏全部 UI 元素 (menubar + 工具栏 + 状态栏 + Sidebar,
    //    含 Impress 绘图工具栏 drawbar 与属性面板 PropertiesDeck —— 2026-08-14
    //    实测这两项残留, 此前只隐藏 menubar; 不依赖 user 配置)
    Reference<css::frame::XLayoutManager> lm(
        factory->createInstanceWithContext("com.sun.star.frame.LayoutManager", ctx), UNO_QUERY);
    if (lm.is()) {
        lm->attachFrame(frame);
        // 无条件 hideElement + 多轮重试 (2026-08-17):
        //   ① isElementVisible 对未就绪元素恒 false (Windows 实测误报) ->
        //      hideElement 曾被跳过 -> menubar 残留被截屏; 无条件调用修复
        //   ② 窗口 UI 异步构建: 单次 hideElement 可能落在 UI 未就绪窗口期
        //      (attachFrame 时元素未注册, hide 命令丢失) -> 多轮重试覆盖;
        //      hideElement 幂等, 对已隐藏元素无害。
        for (int round = 0; round < kHideRounds; round++) {
            for (const auto& e : kUiElems) {
                rtl::OUString res = rtl::OUString::createFromAscii(e.name);
                try {
                    // 诊断: isElementVisible 尽力查询 (Windows 实测恒 false 误报)
                    int vis = -1;
                    try {
                        vis = lm->isElementVisible(res) ? 1 : 0;
                    } catch (const css::uno::Exception& ex) {
                        OfficeLogDbg("[Common.UIHide] isElementVisible(%s) failed: %s", e.label, u2s(ex.Message).c_str());
                    }
                    lm->hideElement(res);
                    OfficeLogDbg("[Common.UIHide] hideElement r%d: %s (vis=%d)", round, e.label, vis);
                } catch (const css::uno::Exception& ex) {
                    // 元素未注册等, 下轮再试
                    OfficeLogWarn("[Common.UIHide] %s layout error r%d: %s", e.label, round, u2s(ex.Message).c_str());
                }
            }
            if (round < kHideRounds - 1)
                std::this_thread::sleep_for(std::chrono::milliseconds(kHideRetryMs));
        }
    } else {
        OfficeLogDbg("[Common.UIHide] no LayoutManager service");
    }
    // 4. 自省: 隐藏后 UI 状态 (对比 before, 确认隐藏生效)
    DumpUiState(frame, factory, ctx, "after");
}

// ---- to_path (G 缝, 设计 E 表上收) ----
std::filesystem::path to_path(const std::string& utf8) {
#ifdef _WIN32
    return std::filesystem::path(u2w(utf8));
#else
    return std::filesystem::path(utf8);
#endif
}

// ---- KernelHost (G 缝, design-platform-isolation.md Part 2 G) ----
// writer 无 LinkPlatform 层, 引导缝收进本工具。双平台各一 Impl (机制安居)。
struct KernelHost::Impl {
    std::string profile_subdir;
    std::string guid;
#ifdef _WIN32
    std::string profile; // Windows: per-session profile 目录
#else
    bool acquired = false;          // Linux: OfficeRuntime::Acquire 引用计数
    std::unique_ptr<OfficeRuntime::BootLock> boot_lock; // Linux: 串行化引导+加载
#endif
};

KernelHost::KernelHost(const char* profile_subdir, const char* guid)
    : impl_(std::make_unique<Impl>()) {
    impl_->profile_subdir = profile_subdir ? profile_subdir : "";
    impl_->guid = guid ? guid : "";
#ifdef _WIN32
    // Windows: per-session profile 目录 (writer 离屏管线, 桌面名空)
    impl_->profile = office_paths::desktop_profile(impl_->profile_subdir, impl_->guid);
    std::error_code ec;
    std::filesystem::create_directories(to_path(impl_->profile), ec);
#else
    // Linux: 共享运行时 (Xvfb+内核) Acquire; writer 无窗口但内核进程 VCL 需 X。
    // 不 AllocSlot (无窗口/无抓帧)。同进程已有 calc/impress 会话则直接复用。
    OfficeRuntimeConfig cfg; // 默认值见 runtime.h (max_docs=8, 3840×2160)
    impl_->acquired = OfficeRuntime::Instance().Acquire(cfg);
    if (!impl_->acquired) {
        OfficeLogErr("[KernelHost] Acquire failed");
    }
#endif
}

KernelHost::~KernelHost() {
#ifdef _WIN32
    // Windows: 独立 soffice 进程, terminate 由会话层 (ShouldTerminateOnDestroy 门控);
    // profile 目录随 soffice 进程退出遗留 (下次 fresh), 无显式清理。
#else
    // Linux: Release 共享运行时引用 (末个 session 释放 Xvfb+内核)
    impl_->boot_lock.reset(); // 确保释放 (若未 Release)
    if (impl_->acquired) {
        OfficeRuntime::Instance().Release();
        impl_->acquired = false;
    }
#endif
}

void KernelHost::BeginBoot() {
#ifdef _WIN32
    // Windows: 每 session 独立 soffice 进程, 无共享内核串行需求 (经验 5 不适用)
#else
    if (!impl_->acquired)
        return;
    impl_->boot_lock = std::make_unique<OfficeRuntime::BootLock>();
    OfficeLog("[KernelHost] boot lock acquired");
#endif
}

void KernelHost::Release() {
#ifdef _WIN32
    // Windows: 无引导锁
#else
    if (impl_->boot_lock) {
        impl_->boot_lock->Unlock(); // 引导+加载完成, 窗口查找可并行 (经验 5)
        impl_->boot_lock.reset();
        OfficeLog("[KernelHost] boot lock released");
    }
#endif
}

css::uno::Reference<css::uno::XComponentContext> KernelHost::ObtainCtx() {
#ifdef _WIN32
    // Windows: 每 session 独立 soffice 三参 bootstrap (空桌面名, writer 离屏)
    auto ctx = BootstrapSession(GetLinkDir(), impl_->profile, "");
    if (!ctx.is())
        OfficeLogErr("[KernelHost] BootstrapSession failed");
    return ctx;
#else
    if (!impl_->acquired)
        return nullptr;
    if (!OfficeRuntime::Instance().EnsureKernel()) {
        OfficeLogErr("[KernelHost] EnsureKernel failed");
        return nullptr;
    }
    // LANG: LO type detection 依赖 locale (env -i 类环境会 type detection
    // failed, 经验 25 陷阱; 不覆盖已有值)
    setenv("LANG", "zh_CN.UTF-8", 0);
    return OfficeRuntime::Instance().kernel();
#endif
}

bool KernelHost::ShouldTerminateOnDestroy() const {
#ifdef _WIN32
    return true;  // Windows: 每 session 独立 soffice 进程须 terminate 退出
#else
    return false; // Linux: 共享内核, 不 terminate
#endif
}

}  // namespace link_utils
