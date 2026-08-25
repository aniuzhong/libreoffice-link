// link_platform.h — 统一平台接口。
// 平台按环境归组: Linux = Xvfb 共享内核模式 (office_runtime + X11 抓帧);
// Windows = 每 session 独立 soffice 进程 + 独立桌面 (CreateDesktop, calc/impress 共用, 经验 39)。
// 设计(命名/归组/文档类型差异数据化)见 [platform-isolation] Part2。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <com/sun/star/frame/XFrame.hpp>
#include <com/sun/star/lang/XMultiComponentFactory.hpp>
#include <com/sun/star/uno/Reference.hxx>
#include <com/sun/star/uno/XComponentContext.hpp>

// 窗口工作绑定点 (相对核心里程碑; 平台声明白己的窗口工作发生处)
enum class WindowPoint { None, BeforeReveal, AfterReveal, AfterStart };

// 平台策略声明 (数据, 非代码): 启动时一次性取, 核心原样消费并打日志
struct SessionPlan {
    WindowPoint discover;        // 窗口发现绑定点
    WindowPoint form;            // 窗口定型(落位/样式)绑定点; None = LO 自管(全屏)
    bool  fullscreen;            // 放映 IsFullScreen (Linux false=窗口化+slot, 经验 1)
    int   settle_ms;             // start 后形态稳定等待 (Win 实测 1200 不够须 2500)
    bool  ui_hide_needed;        // 全屏放映 LO 自管则 false
    bool  terminate_on_destroy;  // 每 session 独立进程才 true
};

// 引导段 RAII: 构造 = 进入串行区, Release() = 核心在 setVisible(P5) 之后显式调用
// (早释点本身是协议: "窗口查找可并行", 经验 5; 须等于当前 calc_session.cpp:318,
//  不得提前到 P3/SnapshotWindows 之后 —— 否则 reintroduce 经验 5 并发崩溃)
class BootSection {
public:
    virtual ~BootSection() = default;
    virtual void Release() = 0;
};

class LinkPlatform {
public:
    virtual ~LinkPlatform() = default;

    // ---- 平台隔离设计新增接口 ([platform-isolation] Part 2 D) ----
    // 平台策略声明 (数据, 非代码): 启动时一次性取, 核心原样消费并打日志
    virtual SessionPlan Plan() = 0;

    // 引导段 RAII: 构造 = 进入串行区, Release() = 核心在 setVisible(P5) 之后显式调用
    virtual std::unique_ptr<BootSection> BeginBoot() = 0;

    // 契约样例 (接口注释写时机与不变量, 平台实现者读合同不读对端代码):
    // DiscoverWindow: 在 plan.discover 绑定点被调; 须已 SnapshotWindows。
    // FormWindow: 在 plan.form 绑定点被调; **放映运行中的窗口几何操作只允许
    //   发生在此实现内** (UNO setPosSize 运行中黑屏, 经验 26); 允许在隐藏态执行
    //   (Win 改 style+SetWindowPos 于 setVisible 前定型)。
    virtual bool DiscoverWindow() = 0;
    virtual bool FormWindow(int w, int h) = 0;   // 吸收 SizeWindowToSlot
    virtual void ApplyNativeFullscreen() = 0;    // 能力钩子, 默认空 (Win 快捷键注入)
    virtual void OnSessionEnd() = 0;             // 能力钩子 (Win terminate; Linux 空)

    // ---- 原有接口 (保持兼容) ----
    // 本 link 所在目录 (== soffice program dir when deployed)。
    virtual std::string GetLinkDir() = 0;

    // Per-instance profile directory (created)。Windows: office_paths 统一
    // %LOCALAPPDATA%\office-link\desktops\<link>\<guid> (bootstrap 消费);
    // Linux: 共享内核用独立 profile (office_paths::xvfb_profile), 返回空串。
    virtual std::string GetProfileDir(const std::string& guid) = 0;

    // 准备运行环境并返回 bootstrap desktop 名 (Linux 为空串)。
    // Linux: office_runtime Acquire (Xvfb+slot) + XOpenDisplay (失败返回空串);
    // Windows: DPI 感知 + DLL 搜索路径 + CreateDesktopA("<guid>_desk")。
    virtual std::string PrepareEnvironment(const std::string& link_dir,
                                           const std::string& guid) = 0;

    // 引导/复用共享 LO 内核并返回上下文。
    // Linux: office_runtime 引导 (首个) 或复用, 返回共享 ctx;
    // Windows: 返回空 (bootstrap 在会话走原路径, 每 session 独立 soffice)。
    virtual css::uno::Reference<css::uno::XComponentContext> EnsureKernel() = 0;

    // 加载文档前: 记录已存在的窗口 (多文档共用内核时区分本 session 新窗口)。
    // Windows: 每 session 独立桌面, 无多窗口区分问题 (空实现)。
    virtual void SnapshotWindows() = 0;

    // 定位本 session 的主窗口 (快照之后新出现的, 匹配规则由平台实现定义)。
    // 注意: 平台隔离设计后, 建议使用 DiscoverWindow() 替代
    virtual bool FindWindow() = 0;

    // 窗口落位: 放到本 session 的子屏位内 (位左上角), 尺寸 = 文档输出分辨率。
    // 注意: 平台隔离设计后, 建议使用 FormWindow() 替代
    virtual bool SizeWindowToSlot(int width, int height) = 0;

    // 运行中切换文档输出分辨率 (窗口 resize; 抓帧缓冲按新尺寸重建)。
    virtual bool SetWindowSize(int width, int height) = 0;

    // 抓一帧进实现自有缓冲区, 输出像素指针与尺寸。BGRA, 32bpp。
    virtual bool CaptureFrame(uint8_t*& pixels, int& width, int& height) = 0;

    // 静音专项 (方案 A, ): 枚举本 session 放映主窗口下的所有子窗口 ID,
    // 用于 ffplay per-window 精确静音隔离。返回值传给子进程内的 FfplayManager
    // (setFastPropertyValue(MGR_PROP_MUTE_WINDOWS, {ids, mute})) → SetMuteAll 按
    // window_id 过滤引擎。Linux 实现: XQueryTree 递归枚举 win_ 子树 (跨进程可见);
    // Windows 返回空 (TODO: 应真实枚举本 session 媒体子窗口, 当前空实现下
    // SetMuteAll 走 no-op 分支, 不影响本进程引擎)。
    virtual std::vector<long> GetMediaWindowIds() = 0;

    // LO 全屏快捷键注入 (Windows 默认桌面场景; 隐藏桌面/Linux 为空操作)。
    virtual void ApplyFullscreenKeystroke() = 0;

    // 隐藏播放 UI 残留 (LO "Full Screen" 退出浮窗; Linux 为空操作)。
    virtual void HideUiFloats() = 0;

    // 平台特定的 UI 修补 (通用 HideUiBlock 之后调用)。
    // Linux 共享内核: LO Xvfb 无头环境 UI 默认 vis=0, 无需平台修补, 空操作;
    // Windows 独立进程: 可能需要 InputLineVisible dispatch 等平台专属
    // 修补 (是否需要/如何执行由平台实现自决, 会话层不假设)。
    virtual void HideUiExtras(const css::uno::Reference<css::frame::XFrame>& frame,
                              const css::uno::Reference<css::lang::XMultiComponentFactory>& factory,
                              const css::uno::Reference<css::uno::XComponentContext>& ctx) = 0;

    // 释放运行环境 (desktop / slot / Xvfb 引用计数)。
    virtual void Cleanup() = 0;
};

// 文档类型专属工厂 (声明于公共头, 实现在 common/<平台>/*_platforms.cpp):
// Linux: 返回 XvfbSessionPlatform 实例 (规则/日志前缀参数化);
// Windows: calc 返回 WindowsPlatform 实例, impress 暂为 stub (返回 nullptr)。
LinkPlatform* CreateCalcPlatform();
LinkPlatform* CreateImpressPlatform();
