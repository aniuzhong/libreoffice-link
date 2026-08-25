// win_platform.cpp — Windows 平台实现 (独立进程模式, 见 win_platform.h)。
// profile seed 与三参 bootstrap 自会话层下沉本实现 (经验 38④); 迁移来历见 [platform-isolation] §E。
#include "win_platform.h"

#include <Shlwapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
// windows.h 的 FindWindow 宏 (-> FindWindowA) 与 LinkPlatform 接口成员
// FindWindow 同名 (经验 32 重命名 FindCalcWindow 引入的跨平台碰撞)
#undef FindWindow
#endif

#include <com/sun/star/beans/PropertyValue.hpp>
#include <com/sun/star/frame/FrameSearchFlag.hpp>
#include <com/sun/star/frame/XDispatch.hpp>
#include <com/sun/star/util/URL.hpp>
#include <com/sun/star/util/XURLTransformer.hpp>

#include <base/link_utils.h> // GetLinkDir/BootstrapSession (三参 bootstrap 统一)
#include <base/log.h>
#include <base/office_paths.h> // .office-link 路径统一 (基目录/子路径派生)

namespace {

HWND FindMainWindowByPid(DWORD pid, HDESK hDesktop) {
    struct Ctx {
        DWORD pid;
        HWND best;
        int bestArea;
    } ctx{ pid, nullptr, 0 };
    auto proc = [](HWND h, LPARAM lp) -> BOOL {
        Ctx* c = reinterpret_cast<Ctx*>(lp);
        DWORD wpid = 0;
        GetWindowThreadProcessId(h, &wpid);
        if (wpid == c->pid) {
            RECT rc;
            GetClientRect(h, &rc);
            int area = (rc.right - rc.left) * (rc.bottom - rc.top);
            if (area > c->bestArea) {
                c->bestArea = area;
                c->best = h;
            }
        }
        return TRUE;
    };
    if (hDesktop)
        EnumDesktopWindows(hDesktop, proc, reinterpret_cast<LPARAM>(&ctx));
    else
        EnumWindows(proc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

// 放映窗口 (Impress 全屏放映): LO 幻灯片放映窗口类名 SALTMPSUBFRAME, 标题
// "Presenting: <文件名>" (旧项目 source/Communicator.cpp 验证过的定位方案,
//  impress Windows 全屏改造复用); 找不到回退 FindMainWindowByPid。
HWND FindPresentationWindow(DWORD pid, HDESK hDesktop) {
    struct Ctx {
        DWORD pid;
        HWND found = nullptr;
    } ctx{ pid };
    auto proc = [](HWND h, LPARAM lp) -> BOOL {
        Ctx* c = reinterpret_cast<Ctx*>(lp);
        DWORD wpid = 0;
        GetWindowThreadProcessId(h, &wpid);
        if (wpid != c->pid)
            return TRUE;
        char cls[128] = { 0 };
        GetClassNameA(h, cls, 127);
        if (_stricmp(cls, "SALTMPSUBFRAME") != 0)
            return TRUE;
        wchar_t title[256] = { 0 };
        GetWindowTextW(h, title, 255);
        if (wcsncmp(title, L"Presenting: ", 12) == 0) {
            c->found = h;
            return FALSE; // 命中即停
        }
        return TRUE;
    };
    if (hDesktop)
        EnumDesktopWindows(hDesktop, proc, reinterpret_cast<LPARAM>(&ctx));
    else
        EnumWindows(proc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

// 注入 LO 自带的全屏快捷键 Ctrl+Shift+J
void InjectCtrlShiftJ() {
    INPUT in[6] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = VK_SHIFT;
    in[2].type = INPUT_KEYBOARD;
    in[2].ki.wVk = 'J';
    in[3].type = INPUT_KEYBOARD;
    in[3].ki.wVk = 'J';
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    in[4].type = INPUT_KEYBOARD;
    in[4].ki.wVk = VK_SHIFT;
    in[4].ki.dwFlags = KEYEVENTF_KEYUP;
    in[5].type = INPUT_KEYBOARD;
    in[5].ki.wVk = VK_CONTROL;
    in[5].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(6, in, sizeof(INPUT));
}

// 隐藏 LO "Full Screen" 退出浮窗 (若存在)
void HideFullScreenFloat(DWORD pid) {
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        DWORD wpid = 0;
        GetWindowThreadProcessId(h, &wpid);
        if (wpid == static_cast<DWORD>(lp)) {
            wchar_t title[64] = { 0 };
            GetWindowTextW(h, title, 63);
            if (wcscmp(title, L"Full Screen") == 0)
                ShowWindow(h, SW_HIDE);
        }
        return TRUE;
    }, static_cast<LPARAM>(pid));
}

}  // namespace

WindowsPlatform::WindowsPlatform(const char* profile_subdir)
    : profile_subdir_(profile_subdir) {}

// ---- 平台隔离设计新增接口 ([platform-isolation] Part 2 D) ----
namespace {
// Windows 引导段空实现 (每 session 独立 soffice 进程, 无共享内核串行需求, 经验 5
// 不适用 Windows; 与 Linux LinuxBootSection 对称, Release() no-op)
class WinNullBootSection : public BootSection {
public:
    void Release() override {} // no-op
};
} // namespace

SessionPlan WindowsPlatform::Plan() {
    // 文档类型差异数据化: impresslink vs calclink (构造参数 profile_subdir_)
    // Windows 策略 (经验 39, 每 session 独立 soffice + 隐藏桌面):
    //   - terminate_on_destroy=true (独立进程须退出)
    //   - settle_ms: impress 放映启动后形态稳定须 2500 (实测 1200 不够);
    //                calc 无放映段, 0
    //   - ui_hide_needed: impress 全屏放映 LO 自管 → false;
    //                     calc 窗口化须 HideUiBlock → true
    //   - fullscreen: impress IsFullScreen=true (LO 自管全屏放映窗口);
    //                 calc false (窗口化, SetWindowSize 改 style 全屏)
    //   - discover/form: calc 反序定型 (F 用例, BeforeReveal; VCL 在 setVisible
    //     时按最终形态创建, 反序 menubar 隐藏失效, demo 实测);
    //     impress 放映窗口 (SALTMPSUBFRAME) 在 start 后 (P8/AfterStart) 才创建,
    //     discover=AfterStart, form=None (LO 自管全屏窗口几何, 核心不碰, 经验 26;
    //     AfterReveal 会抓到 Hidden 加载的编辑窗口 2856x1470,  探针实测)
    SessionPlan plan{};
    plan.terminate_on_destroy = true;
    const bool is_impress = (std::string(profile_subdir_) == "impresslink");
    if (is_impress) {
        plan.discover = WindowPoint::AfterStart;
        plan.form = WindowPoint::None;
        plan.fullscreen = true;
        plan.settle_ms = 2500;
        plan.ui_hide_needed = false;
    } else {
        // calc (calclink): 反序定型 (F 用例)
        plan.discover = WindowPoint::BeforeReveal;
        plan.form = WindowPoint::BeforeReveal;
        plan.fullscreen = false;
        plan.settle_ms = 0;
        plan.ui_hide_needed = true;
    }
    OfficeLog("[%s] Plan: discover=%d form=%d fullscreen=%d settle_ms=%d ui_hide=%d terminate=%d",
              profile_subdir_, static_cast<int>(plan.discover), static_cast<int>(plan.form),
              plan.fullscreen, plan.settle_ms, plan.ui_hide_needed, plan.terminate_on_destroy);
    return plan;
}

std::unique_ptr<BootSection> WindowsPlatform::BeginBoot() {
    return std::make_unique<WinNullBootSection>();
}

bool WindowsPlatform::DiscoverWindow() {
    return FindWindow(); // 复用现有实现 (pidfile → EnumDesktopWindows)
}

bool WindowsPlatform::FormWindow(int w, int h) {
    // Windows 无 slot 概念; "落位" = 改 style 去标题栏 + SetWindowPos 全屏。
    // 隐藏态执行 (calc 反序在 setVisible 前): VCL 窗口 setVisible 时按最终
    // 形态创建, 反序 menubar 隐藏才有效 (F 用例, demo 实测)。
    return SizeWindowToSlot(w, h);
}

void WindowsPlatform::ApplyNativeFullscreen() {
    ApplyFullscreenKeystroke(); // hidden desktop 下 no-op; 默认桌面场景注入 Ctrl+Shift+J
}

std::string WindowsPlatform::GetLinkDir() {
    return link_utils::GetLinkDir();
}

std::string WindowsPlatform::GetProfileDir(const std::string& guid) {
    // 路径统一 office_paths (): desktops/<link>/<guid> (名字直指
    // 每 session 独立桌面机制); 输出 forward slashes。
    std::string profile = office_paths::desktop_profile(profile_subdir_, guid);
    std::error_code ec;
    std::filesystem::create_directories(profile, ec);
    return profile;
}

std::string WindowsPlatform::PrepareEnvironment(const std::string& link_dir,
                                                const std::string& guid) {
    SetProcessDpiAwarenessContext(reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4));
    SetDllDirectoryA(link_dir.c_str());
    // 禁用 LO 的 OpenGL 渲染 (Linux 经验 21 同款哲学): 隐藏桌面无 DWM 合成,
    // GL 转场内容 PrintWindow 抓不到 -> 翻页动画白帧 ( demo 实测);
    // 转场退化为 CPU 渲染, 效果保留, 内容可抓。soffice 由 cppu::bootstrap
    // 启动, 环境快照继承本设置。
    SetEnvironmentVariableA("SAL_DISABLEGL", "1");
    guid_ = guid;
    link_dir_ = link_dir;

    // profile seed: 从部署模板 (templates/user, 仓库 git 管理的净化 xcu;
    //  起 office/user 退役) fresh copy 到每 session profile。
    // 语义与 Linux SeedKernelProfile 同一规则 (创建时回模板基线, 运行期
    // 写回不跨 session 存活), UI 三层控制的第三层 (UNO > 窗口 API > 模板)。
    {
        std::string profile = GetProfileDir(guid);
        std::string tmpl = link_dir + office_paths::user_template() + "/registrymodifications.xcu";
        std::string dst = profile + "/user/registrymodifications.xcu";
        std::replace(tmpl.begin(), tmpl.end(), '\\', '/');
        std::error_code ec;
        std::filesystem::remove_all(profile + "/user", ec); // fresh copy each session
        std::filesystem::create_directories(profile + "/user", ec);
        std::filesystem::copy_file(tmpl, dst,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
            OfficeLogWarn("[Common.WinProfile] profile seed failed: %s", ec.message().c_str());
        else
            OfficeLog("[Common.WinProfile] profile seeded from %s", tmpl.c_str());
    }

    desk_name_ = guid + "_desk";
    desk_ = CreateDesktopA(desk_name_.c_str(), nullptr, nullptr, 0,
                           GENERIC_ALL, nullptr);
    if (!desk_) {
        fprintf(stderr, "WindowsPlatform: CreateDesktopA(%s) failed, err=%d\n",
                desk_name_.c_str(), GetLastError());
        return {};
    }
    return desk_name_; // bootstrap desktop name
}

void WindowsPlatform::SnapshotWindows() {
    // Windows: 每 session 独立桌面, 无多窗口区分问题
}

css::uno::Reference<css::uno::XComponentContext> WindowsPlatform::EnsureKernel() {
    // Windows: 每 session 独立 soffice 三参 bootstrap (soffice 目录 + 独立
    // profile + 目标桌面; 原 calc_session 会话层 #ifdef _WIN32, 下沉统一)。
    if (link_dir_.empty())
        return nullptr;
    return link_utils::BootstrapSession(link_dir_, GetProfileDir(guid_), desk_name_);
}

bool WindowsPlatform::FindWindow() {
    // soffice.bin pid from the profile pidfile.
    DWORD pid = 0;
    {
        std::string pid_path = GetProfileDir(guid_) + "/pidfile";
        std::ifstream ifs(pid_path);
        if (ifs.is_open()) {
            std::string s;
            ifs >> s;
            pid = static_cast<DWORD>(std::strtoul(s.c_str(), nullptr, 10));
        }
    }
    // 优先放映窗口 (Impress 全屏: SALTMPSUBFRAME + "Presenting: "), 回退最大窗口
    hwnd_ = FindPresentationWindow(pid, static_cast<HDESK>(desk_));
    if (!hwnd_)
        hwnd_ = FindMainWindowByPid(pid, static_cast<HDESK>(desk_));
    // 诊断 (menubar 隐藏排查, ): 找到的窗口身份/形态
    if (hwnd_) {
        char cls[128] = { 0 }, title[256] = { 0 };
        GetClassNameA(static_cast<HWND>(hwnd_), cls, 127);
        GetWindowTextA(static_cast<HWND>(hwnd_), title, 255);
        RECT rc;
        GetClientRect(static_cast<HWND>(hwnd_), &rc);
        LONG_PTR style = GetWindowLongPtrW(static_cast<HWND>(hwnd_), GWL_STYLE);
        OfficeLog("[Common.WinWindow] FindWindow pid=%lu hwnd=%p cls=%s title=%s client=%dx%d "
                  "style=0x%llx vis=%d\n",
                  pid, hwnd_, cls, title, rc.right - rc.left, rc.bottom - rc.top,
                  (unsigned long long)style,
                  IsWindowVisible(static_cast<HWND>(hwnd_)) ? 1 : 0);
    } else {
        OfficeLogErr("[Common.WinWindow] FindWindow FAILED pid=%lu (pidfile %s)\n", pid,
                  (GetProfileDir(guid_) + "/pidfile").c_str());
    }
    return hwnd_ != nullptr;
}

bool WindowsPlatform::SizeWindowToSlot(int width, int height) {
    // Windows 无 slot 概念 (Linux 大屏分区), "落位" = 全屏无边框窗口 (SetWindowSize)。
    return SetWindowSize(width, height);
}

bool WindowsPlatform::SetWindowSize(int width, int height) {
    (void)width; (void)height;
    if (!hwnd_)
        return false;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    LONG_PTR style = GetWindowLongPtrW(static_cast<HWND>(hwnd_), GWL_STYLE);
    RECT rc_before;
    GetClientRect(static_cast<HWND>(hwnd_), &rc_before);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    SetWindowLongPtrW(static_cast<HWND>(hwnd_), GWL_STYLE, style);
    BOOL pos_ok = SetWindowPos(static_cast<HWND>(hwnd_), nullptr, 0, 0, screenW, screenH,
                               SWP_FRAMECHANGED | SWP_NOZORDER);
    // 诊断 (menubar 隐藏排查, ): 窗口形态修改结果
    RECT rc_after;
    GetClientRect(static_cast<HWND>(hwnd_), &rc_after);
    OfficeLog("[Common.WinWindow] SetWindowSize %dx%d -> %dx%d, style 0x%llx, SetWindowPos=%d err=%lu, "
              "client %dx%d -> %dx%d\n",
              width, height, screenW, screenH, (unsigned long long)style, pos_ok ? 1 : 0,
              GetLastError(), rc_before.right - rc_before.left,
              rc_before.bottom - rc_before.top, rc_after.right - rc_after.left,
              rc_after.bottom - rc_after.top);
    return true;
}

bool WindowsPlatform::CaptureFrame(uint8_t*& pixels, int& width, int& height) {
    if (!hwnd_)
        return false;
    // SetThreadDesktop 只切换一次 ( 2160p 帧率优化): 每帧切换桌面
    // 上下文有开销; 抓帧线程 (poll) 切换后永久绑定隐藏桌面 (该线程只抓帧,
    // 不需要回到原桌面)。
    if (desk_ && !desktop_switched_) {
        if (!SetThreadDesktop(static_cast<HDESK>(desk_))) {
            OfficeLogWarn("[Common.WinWindow] SetThreadDesktop failed err=%lu\n", GetLastError());
            return false;
        }
        desktop_switched_ = true;
    }
    RECT rc;
    GetClientRect(static_cast<HWND>(hwnd_), &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0)
        return false;

    if (!cap_dc_ || w != cap_w_ || h != cap_h_) {
        if (cap_bmp_) {
            DeleteObject(static_cast<HBITMAP>(cap_bmp_));
            cap_bmp_ = nullptr;
            cap_pixels_ = nullptr;
        }
        if (cap_dc_) {
            DeleteDC(static_cast<HDC>(cap_dc_));
            cap_dc_ = nullptr;
        }
        BITMAPINFO bmi = { 0 };
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        cap_dc_ = CreateCompatibleDC(GetWindowDC(static_cast<HWND>(hwnd_)));
        cap_bmp_ = CreateDIBSection(static_cast<HDC>(cap_dc_), &bmi, DIB_RGB_COLORS,
                                    reinterpret_cast<void**>(&cap_pixels_),
                                    nullptr, 0);
        cap_w_ = w;
        cap_h_ = h;
    }

    HGDIOBJ old = SelectObject(static_cast<HDC>(cap_dc_), static_cast<HGDIOBJ>(cap_bmp_));
    // 抓帧模式 ( 2160p 帧率优化, demo 定调默认 bitblt):
    //   bitblt (默认): 直接读窗口 GDI 表面 (~10ms) —— SAL_DISABLEGL 后 LO 纯
    //     GDI 渲染, 窗口表面持续更新 (动画期间 LO 主动重绘), 静止时读旧表面
    //     (心跳帧内容相同无妨); 失败回退 PrintWindow
    //   ORT_CAPTURE_MODE=printwindow: 切回旧模式 (PrintWindow 每帧触发 soffice
    //     全量重绘 40-90ms/帧 -> 帧率低; 兼容排查用, IDE 不便设环境变量故默认
    //     bitblt)
    const char* mode = getenv("ORT_CAPTURE_MODE");
    bool bitblt_first = !(mode && strcmp(mode, "printwindow") == 0);
    BOOL ok = FALSE;
    if (bitblt_first) {
        HDC wdc = GetWindowDC(static_cast<HWND>(hwnd_));
        ok = BitBlt(static_cast<HDC>(cap_dc_), 0, 0, w, h, wdc, 0, 0, SRCCOPY);
        ReleaseDC(static_cast<HWND>(hwnd_), wdc);
        if (!ok)
            ok = PrintWindow(static_cast<HWND>(hwnd_), static_cast<HDC>(cap_dc_), PW_RENDERFULLCONTENT);
    } else {
        ok = PrintWindow(static_cast<HWND>(hwnd_), static_cast<HDC>(cap_dc_), PW_RENDERFULLCONTENT);
        if (!ok) {
            HDC wdc = GetWindowDC(static_cast<HWND>(hwnd_));
            ok = BitBlt(static_cast<HDC>(cap_dc_), 0, 0, w, h, wdc, 0, 0, SRCCOPY);
            ReleaseDC(static_cast<HWND>(hwnd_), wdc);
        }
    }
    SelectObject(static_cast<HDC>(cap_dc_), old);
    // 首次抓帧默认留痕抓帧模式 (进程级一次; 排查时日志直接可见, 无需环境变量)
    static bool s_mode_logged = false;
    if (!s_mode_logged) {
        s_mode_logged = true;
        OfficeLog("[Common.WinWindow] capture mode=%s (default bitblt, ORT_CAPTURE_MODE=printwindow 回退)\n",
                  bitblt_first ? "bitblt" : "printwindow");
    }
    // 诊断 ( 探针首帧全白排查): PrintWindow 结果 + 窗口/桌面状态
    if (getenv("ORT_DUMP_CAPTURE")) {
        char cls[128] = { 0 }, title[256] = { 0 };
        GetClassNameA(static_cast<HWND>(hwnd_), cls, 127);
        GetWindowTextA(static_cast<HWND>(hwnd_), title, 255);
        HWND fg = GetForegroundWindow();
        OfficeLog("[Common.WinWindow] capture mode=%s ok=%d err=%lu vis=%d cls=%s title=%s fg_is_hwnd=%d\n",
                  bitblt_first ? "bitblt" : "printwindow", ok ? 1 : 0, GetLastError(),
                  IsWindowVisible(static_cast<HWND>(hwnd_)) ? 1 : 0, cls, title,
                  fg == static_cast<HWND>(hwnd_) ? 1 : 0);
    }
    if (!ok)
        return false;

    pixels = cap_pixels_;
    width = w;
    height = h;
    return true;
}

void WindowsPlatform::ApplyFullscreenKeystroke() {
    if (!hwnd_)
        return;
    if (desk_) {
        // Hidden desktop: keystroke injection verified unreachable.
        return;
    }
    // Default desktop: focus the window, then SendInput.
    HWND h = static_cast<HWND>(hwnd_);
    SetForegroundWindow(h);
    bool focused = false;
    for (int i = 0; i < 30; ++i) {
        if (GetForegroundWindow() == h) {
            focused = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!focused) {
        HWND host = GetForegroundWindow();
        if (host && host != h) {
            ShowWindow(host, SW_MINIMIZE);
            for (int i = 0; i < 30; ++i) {
                if (GetForegroundWindow() == h) {
                    focused = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            ShowWindow(host, SW_RESTORE);
        }
    }
    if (focused) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        InjectCtrlShiftJ();
    }
}

void WindowsPlatform::HideUiFloats() {
    if (!hwnd_)
        return;
    DWORD pid = 0;
    GetWindowThreadProcessId(static_cast<HWND>(hwnd_), &pid);
    if (pid)
        HideFullScreenFloat(pid);
}

void WindowsPlatform::HideUiExtras(const css::uno::Reference<css::frame::XFrame>& frame,
                                   const css::uno::Reference<css::lang::XMultiComponentFactory>& factory,
                                   const css::uno::Reference<css::uno::XComponentContext>& ctx) {
    // 公式栏 (fx/Σ 输入行) 关闭: dispatch .uno:InputLineVisible
    // (SFX docking window, hideElement 无效; 平台隔离设计: 从 calc_session
    // 共享层下沉本实现, Linux Xvfb 无头环境 UI 默认 vis=0 不需此 dispatch)。
    if (!frame.is() || !factory.is() || !ctx.is())
        return;
    css::util::URL url;
    url.Complete = link_utils::s2u(".uno:InputLineVisible");
    try {
        css::uno::Reference<css::util::XURLTransformer> tr(
            factory->createInstanceWithContext("com.sun.star.util.URLTransformer", ctx),
            css::uno::UNO_QUERY);
        if (tr.is())
            tr->parseStrict(url);
        css::uno::Reference<css::frame::XDispatch> disp =
            css::uno::Reference<css::frame::XDispatchProvider>(frame, css::uno::UNO_QUERY)
                ->queryDispatch(url, rtl::OUString(), css::frame::FrameSearchFlag::SELF);
        if (disp.is()) {
            disp->dispatch(url, css::uno::Sequence<css::beans::PropertyValue>());
            OfficeLog("[WinPlatform] InputLineVisible dispatched (公式栏隐藏)");
        } else {
            OfficeLogWarn("[WinPlatform] InputLineVisible dispatcher NOT found");
        }
    } catch (const css::uno::Exception& e) {
        OfficeLogWarn("[WinPlatform] InputLineVisible dispatch error: %s",
                      link_utils::u2s(e.Message).c_str());
    }
}

void WindowsPlatform::Cleanup() {
    if (cap_bmp_) {
        DeleteObject(static_cast<HBITMAP>(cap_bmp_));
        cap_bmp_ = nullptr;
        cap_pixels_ = nullptr;
    }
    if (cap_dc_) {
        DeleteDC(static_cast<HDC>(cap_dc_));
        cap_dc_ = nullptr;
    }
    if (desk_) {
        CloseDesktop(static_cast<HDESK>(desk_));
        desk_ = nullptr;
    }
    hwnd_ = nullptr;
    cap_w_ = 0;
    cap_h_ = 0;
}
