// win_platform.h — Windows 平台实现: 每 session 独立 soffice 进程 +
// 独立桌面 (CreateDesktop), 不参与 office_runtime (Linux 共享内核模式专属)。
// 迁移自 calc/windows/calc_platform.cpp (接口统一为 LinkPlatform);
// 文档类型差异 = 构造参数 (profile 子目录名), calc/impress 共用同一实现。
// 契约 (2026-08-17 平台归位): PrepareEnvironment 完成环境+profile seed
// (office\user UI 隐藏配置模板, 原 calc_session 会话层, Linux 死开销已消除);
// EnsureKernel 完成每 session 三参 bootstrap (原会话层 #ifdef _WIN32 下沉,
// link_utils::BootstrapSession), 会话层统一调用, 无平台分支。
#pragma once

#include "../link_platform.h"

#include <cstdint>
#include <string>

class WindowsPlatform : public LinkPlatform {
public:
    explicit WindowsPlatform(const char* profile_subdir);
    ~WindowsPlatform() override { Cleanup(); }

    // ---- 平台隔离设计新增接口 (design-platform-isolation.md Part 2 D) ----
    // Plan(): calc/impress 策略按 profile_subdir_ 区分 (文档类型差异数据化)。
    //   impress: 全屏放映 (LO 自管窗口, form=None), settle 2500ms, ui_hide=false
    //   calc: 窗口化 (反序定型 F 用例, discover=form=BeforeReveal), ui_hide=true
    // BeginBoot(): Windows 每 session 独立进程, 无共享内核串行需求 → 空实现
    // DiscoverWindow(): 复用 FindWindow() (pidfile → EnumDesktopWindows)
    // FormWindow(): 复用 SizeWindowToSlot() → SetWindowSize() (改 style+SetWindowPos;
    //   隐藏态执行, 契约允许, 见 link_platform.h FormWindow 注释)
    // ApplyNativeFullscreen(): 复用 ApplyFullscreenKeystroke() (hidden desktop 下 no-op)
    // OnSessionEnd(): 空 (terminate 由会话层按 plan.terminate_on_destroy 门控;
    //   平台不持有 desktop_ UNO 对象, 优雅 terminate 在会话层)
    SessionPlan Plan() override;
    std::unique_ptr<BootSection> BeginBoot() override;
    bool DiscoverWindow() override;
    bool FormWindow(int w, int h) override;
    void ApplyNativeFullscreen() override;
    void OnSessionEnd() override {}

    // ---- 原有接口 (保持兼容) ----
    std::string GetLinkDir() override;
    std::string GetProfileDir(const std::string& guid) override;
    std::string PrepareEnvironment(const std::string& link_dir,
                                   const std::string& guid) override;
    css::uno::Reference<css::uno::XComponentContext> EnsureKernel() override;
    void SnapshotWindows() override; // 每 session 独立桌面, 空实现
    bool FindWindow() override;      // 按 profile pidfile 找 soffice 主窗口
    bool SizeWindowToSlot(int width, int height) override;
    bool SetWindowSize(int width, int height) override;
    bool CaptureFrame(uint8_t*& pixels, int& width, int& height) override;
    std::vector<long> GetMediaWindowIds() override { return {}; }  // Win 独立进程天然隔离, 空
    void ApplyFullscreenKeystroke() override;
    void HideUiFloats() override;
    void HideUiExtras(const css::uno::Reference<css::frame::XFrame>& frame,
                      const css::uno::Reference<css::lang::XMultiComponentFactory>& factory,
                      const css::uno::Reference<css::uno::XComponentContext>& ctx) override;
    void Cleanup() override;

private:
    const char* profile_subdir_; // "calclink"/"impresslink"
    std::string guid_;
    std::string link_dir_;
    std::string desk_name_;
    void* hwnd_ = nullptr;   // HWND
    void* desk_ = nullptr;   // HDESK
    void* cap_dc_ = nullptr; // HDC
    void* cap_bmp_ = nullptr; // HBITMAP
    uint8_t* cap_pixels_ = nullptr;
    int cap_w_ = 0;
    int cap_h_ = 0;
    bool desktop_switched_ = false; // SetThreadDesktop 只切一次 (帧率优化)
};
