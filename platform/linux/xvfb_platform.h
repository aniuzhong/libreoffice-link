// xvfb_platform.h — Linux 平台实现 (Xvfb 共享内核模式): office_runtime
// (Xvfb + 共享 LO 内核 + slot) + 窗口查找/落位/缩窗 + XShm 抓帧。
// 单类 + 规则参数化 (文档类型差异 = 数据), 不再需要子类:
//   XvfbSessionPlatform(WindowMatchRule, tag) — calc/impress 共用;
//   MatchWindow 保留为 virtual (默认走规则), 作为未来类型特有逻辑的扩展点。
// 迁移自 calc/linux/calc_platform.cpp (原 calc/impress 两份 77% 重复的公共侧)。
#pragma once

#include "../link_platform.h"
#include "../../runtime/runtime.h" // OfficeRuntime (Xvfb/内核/slot)

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

// 窗口树扫描结果 (匹配规则与 MatchWindow 的输入)
struct WinInfo {
    Window wid = 0;
    std::string name, cls;
    int w = 0, h = 0;
};

// XShm 抓帧状态 (每平台实例一份; 窗口尺寸变化时重建段)
struct ShmState {
    Display* dpy = nullptr;        // 所属显示 (XShmDetach 需要)
    XShmSegmentInfo seg{};
    XImage* img = nullptr;
    int w = 0, h = 0;
    bool available = false; // 扩展查询过一次 (失败则回退 XGetImage)
    bool probed = false;

    ~ShmState() { Release(); }
    void Release();
    // 按窗口尺寸 (惰性) 创建/重建段; 失败返回 false (回退 XGetImage)
    bool Ensure(Display* d, Window win, int width, int height);
};

// 窗口匹配规则 (文档类型差异数据化; 命中任一关键词即算目标窗口)。
struct WindowMatchRule {
    std::vector<std::string> cls_keywords;  // 窗口 class 子串 (如 calc: "soffice"/"calc"/"office")
    std::vector<std::string> name_keywords; // 窗口名子串 (如 calc: "Calc"; impress 无)
    int min_w = 640;                        // 最小尺寸过滤 (排除 splash/dialog)
    int min_h = 480;
};

// Linux 引导段实现 (office_runtime BootLock)
class LinuxBootSection : public BootSection {
public:
    LinuxBootSection();
    ~LinuxBootSection() override { Release(); }
    void Release() override;
private:
    bool released_ = false;
    OfficeRuntime::BootLock boot_lock_;  // 包装 office_runtime BootLock
};

class XvfbSessionPlatform : public LinkPlatform {
public:
    XvfbSessionPlatform(const WindowMatchRule& rule, const char* tag);
    ~XvfbSessionPlatform() override { Cleanup(); }

    // ---- 平台隔离设计新增接口 (design-platform-isolation.md Part 2 D) ----
    SessionPlan Plan() override;
    std::unique_ptr<BootSection> BeginBoot() override;
    bool DiscoverWindow() override;
    bool FormWindow(int w, int h) override;
    void ApplyNativeFullscreen() override {}  // Linux 无快捷键注入需求
    void OnSessionEnd() override {}          // Linux 共享内核, 无 terminate 需求
    
    // ---- 原有接口 (保持兼容) ----
    std::string GetLinkDir() override;
    std::string GetProfileDir(const std::string& guid) override;
    std::string PrepareEnvironment(const std::string& link_dir,
                                   const std::string& guid) override;
    css::uno::Reference<css::uno::XComponentContext> EnsureKernel() override;
    void SnapshotWindows() override;
    bool FindWindow() override;
    bool SizeWindowToSlot(int width, int height) override;
    bool SetWindowSize(int width, int height) override;
    bool CaptureFrame(uint8_t*& pixels, int& width, int& height) override;
    std::vector<long> GetMediaWindowIds() override;  // 方案 A: XQueryTree 枚举 win_ 子树
    void ApplyFullscreenKeystroke() override {}  // Linux 无快捷键注入需求
    void HideUiFloats() override {}              // Linux 无 "Full Screen" 浮窗
    void HideUiExtras(const css::uno::Reference<css::frame::XFrame>&,
                      const css::uno::Reference<css::lang::XMultiComponentFactory>&,
                      const css::uno::Reference<css::uno::XComponentContext>&) override {}  // Linux: LO Xvfb 无头环境 UI 默认 vis=0, 无需平台修补
    void Cleanup() override;

protected:
    // 窗口匹配 (默认走 rule_; 未来类型特有逻辑可覆盖)
    virtual bool MatchWindow(const WinInfo& w) const;
    // 日志前缀 (文档类型标识, 如 "calc"/"impress")
    virtual const char* Tag() const { return tag_; }

private:
    WindowMatchRule rule_;
    const char* tag_;

    OfficeRuntime& runtime_ = OfficeRuntime::Instance(); // 进程级共享运行时
    std::string display_;
    void* dpy_ = nullptr; // Display*
    Window win_ = 0;
    int slot_ = -1;
    std::vector<uint8_t> cap_bgra_;
    std::set<Window> preexisting_; // 加载文档前已存在的窗口
    ShmState shm_;                 // XShm 抓帧状态 (窗口尺寸变化自动重建)
};
