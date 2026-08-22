#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>


#include "../abi/abi.h"
#include "link_platform.h"
#include "../frame/frame_pump.h"  // 阶段4: 统一帧泵 (替代 poll_thread_/paused_/force_frame_)

#include <com/sun/star/uno/Reference.hxx>
#include <com/sun/star/uno/XComponentContext.hpp>
#include <com/sun/star/lang/XComponent.hpp>
#include <com/sun/star/frame/XController.hpp>
#include <com/sun/star/frame/XDesktop.hpp>
#include <com/sun/star/frame/XFrame.hpp>
#include <com/sun/star/sheet/XViewPane.hpp>
#include <com/sun/star/sheet/XSpreadsheetView.hpp>
#include <com/sun/star/sheet/XSpreadsheets.hpp>

// 会话状态机 (V4 治理, HANDOFF 七、已知漏洞): UNO 就绪 -> started_(帧泵
// 运行) -> destroyed_(终态)。所有公开方法入口判 destroyed_, 销毁后调用一律
// no-op 返回失败值; C ABI 层另有 SessionRegistry 入口守卫 (common/
// session_registry.h) 拦截悬垂指针, 双层防护。
class CalcSession {
public:
    CalcSession();
    ~CalcSession();

    // width/height: 文档输出分辨率 (≤ 子屏位最大分辨率, 默认 0 = 1920x1080)
    bool     Create(const char* path, const char* password, const char* guid, CalcFrameCallback cb, void* opaque, int width = 0, int height = 0);
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
    bool     MoveScroll(int dx, int dy);
    bool     SetSheet(unsigned index);
    int      GetSheetCount();
    int      GetCurrentSheet();
    int      GetWidth();
    int      GetHeight();
    bool     SetScale(unsigned percent);
    unsigned GetScale();

private:
    bool PushFrame();
    bool ScrollPage(int direction);
    bool ScrollByRows(int deltaRows);
    bool ScrollByCols(int deltaCols);
    void SyncViewportState();
    bool CheckViewportChanged();  // 阶段4: ChangeFn 探测 (视口签名+force_frame_脏位, 持 mu_)

    // Platform resources (desktop/Xvfb, window, capture buffers).
    std::unique_ptr<LinkPlatform> platform_;
    SessionPlan plan_{};  // 平台策略 (Create 时取, Destroy 消费 terminate_on_destroy)

    // UNO objects.
    css::uno::Reference<css::uno::XComponentContext> ctx_;
    css::uno::Reference<css::frame::XDesktop> desktop_;
    css::uno::Reference<css::lang::XComponent> component_;
    css::uno::Reference<css::frame::XController> controller_;
    css::uno::Reference<css::frame::XFrame> frame_;
    css::uno::Reference<css::sheet::XViewPane> pane_;
    css::uno::Reference<css::sheet::XSpreadsheetView> view_;
    css::uno::Reference<css::sheet::XSpreadsheets> sheets_;

    // Callback.
    CalcFrameCallback cb_ = nullptr;
    void* opaque_ = nullptr;

    // Per-instance logger (unique name, own file under the profile dir).

    // State.
    int width_ = 0;
    int height_ = 0;
    int target_w_ = 0;        // 文档输出分辨率 (Create/SetResolution 设定)
    int target_h_ = 0;
    std::chrono::steady_clock::time_point last_resize_;  // V2: SetResolution 节流
    bool started_ = false;
    // V4 状态机: destroyed 终态标志 (Destroy 幂等 + 公开方法入口守卫)
    std::atomic<bool> destroyed_{false};

    // 阶段4: 脏位 probe (滚动/切表/缩放置位, ChangeFn 内 exchange 消费)
    std::atomic<bool> force_frame_{false};
    // Frame pump (阶段4: 统一帧泵, 替代原 poll_thread_/paused_/force_frame_)
    // calc 策略: probe=视口签名+force_frame_脏位, heartbeat=100ms, hbp=true (Pause 照推), tick=20ms
    std::unique_ptr<FramePump> pump_;
    std::mutex mu_;  // 会话锁 (UNO 操作保护; 调用 pump_ 方法时不得持有)
    sal_Int32 last_row_ = -1;
    sal_Int32 last_col_ = -1;
    std::string last_sheet_;
};
