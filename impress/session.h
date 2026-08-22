#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../abi/abi.h"
#include "link_platform.h"
#include "frame_pump.h"

#include <com/sun/star/uno/Reference.hxx>
#include <com/sun/star/uno/XComponentContext.hpp>
#include <com/sun/star/lang/XComponent.hpp>
#include <com/sun/star/frame/XController.hpp>
#include <com/sun/star/frame/XDesktop.hpp>
#include <com/sun/star/frame/XFrame.hpp>
#include <com/sun/star/presentation/XPresentation2.hpp>
#include <com/sun/star/presentation/XSlideShowController.hpp>

// Impress 会话 (与 CalcSession 同构): 引导 -> Hidden 加载 pptx -> XPresentation2
// 放映 -> 轮询抓帧。放映形态: Linux 窗口化 + slot 缩窗 (全屏会占据 Xvfb 大屏,
// 经验 1); Windows 全屏 (IsFullScreen=true, LO 自管, 经验 39)。
// 会话状态机 (V4 治理, HANDOFF 七): created_(放映就绪) -> destroyed_(终态)。
// 公开方法入口判 destroyed_, 销毁后调用一律 no-op; C ABI 层另有
// SessionRegistry 入口守卫 (common/session_registry.h) 双层防护。
class ImpressSession {
public:
    ImpressSession();
    ~ImpressSession();

    // width/height: 文档输出分辨率 (≤ 子屏位最大分辨率, 默认 0 = 1920x1080)
    bool     Create(const char* path, const char* password, const char* guid, ImpressFrameCallback cb, void* opaque, int width = 0, int height = 0);
    bool     SetResolution(int width, int height);
    void     Destroy();
    bool     Start();
    bool     Stop();
    bool     Pause();
    bool     Resume();
    // 锁序约束 (V5): UpdateFrame 持 frame_mutex_, 不得在其他方法 (NextPage/
    //   Stop 等, 持 mu_) 的调用栈内嵌套调用 — 否则 mu_→frame_mutex_ 反转
    //   帧泵的 frame_mutex_→mu_, 构成死锁。同理, 帧回调 cb_ 内不得调用任何
    //   session API (cb_ 在 frame_mutex_ 持有期间执行)。
    bool     UpdateFrame();
    bool     NextPage();
    bool     PreviousPage();
    bool     GoToPage(int page);
    int      GetCurrentPage();
    int      GetPageCount();
    int      GetWidth();
    int      GetHeight();
    bool     SetMute(bool mute); // 静音专项 (方案 A): 按 media window_id 精确隔离

private:
    bool PushFrame();
    void SyncCurrentPage();  // 后台校准: 用 LO 真实值更新缓存 (PushFrame 中调用)

    // Platform resources (office_runtime/Xvfb, slot window, capture buffers).
    std::unique_ptr<LinkPlatform> platform_;
    SessionPlan plan_{};  // 平台策略 (Create 时取, Destroy 消费 terminate_on_destroy)

    // UNO objects.
    css::uno::Reference<css::uno::XComponentContext> ctx_;
    css::uno::Reference<css::frame::XDesktop> desktop_; // Windows: Destroy terminate
    css::uno::Reference<css::lang::XComponent> component_;
    css::uno::Reference<css::frame::XController> controller_;
    css::uno::Reference<css::frame::XFrame> frame_;
    css::uno::Reference<css::presentation::XPresentation2> presentation_;
    css::uno::Reference<css::presentation::XSlideShowController> slideshow_;

    // Callback.
    ImpressFrameCallback cb_ = nullptr;
    void* opaque_ = nullptr;

    // State.
    int width_ = 0;
    int height_ = 0;
    int target_w_ = 0;        // 文档输出分辨率 (Create/SetResolution 设定)
    int target_h_ = 0;
    std::chrono::steady_clock::time_point last_resize_;  // V2: SetResolution 节流 (高频 resize 合并)
    int page_count_ = 0;
    int current_page_ = 0;  // 本地缓存当前页号, NextPage/GoToPage 时同步更新, GetCurrentPage 直接返回
    std::chrono::steady_clock::time_point last_nav_;  // 上次导航时间, SyncCurrentPage 用于抑制回退
    bool created_ = false;
    // V4 状态机: destroyed 终态标志 (Destroy 幂等 + 公开方法入口守卫)
    std::atomic<bool> destroyed_{false};

    // Frame pump (阶段2: 统一帧泵, 替代原 poll_thread_/paused_/force_frame_)
    std::unique_ptr<FramePump> pump_;
    std::mutex mu_;  // 会话锁 (UNO 操作保护; 调用 pump_ 方法时不得持有)
};
