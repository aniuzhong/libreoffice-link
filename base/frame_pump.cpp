// frame_pump.cpp — 三链统一帧泵实现 (经验 42 治理)
#include "frame_pump.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

FramePump::FramePump(const char* tag, FramePumpPlan plan, FrameFn frame, ChangeFn changed)
    : tag_(tag ? tag : ""), plan_(plan), frame_fn_(std::move(frame)), changed_fn_(std::move(changed)) {
}

FramePump::~FramePump() {
    Stop();
}

void FramePump::Start() {
    std::lock_guard<std::mutex> lk(ctrl_mutex_);
    // Start: 幂等且无条件重置 paused_ (经验 41/42; 契约见 [framepump] §0)
    paused_ = false;
    if (running_.load())
        return;  // 幂等
    running_ = true;
    stop_requested_ = false;
    poll_thread_ = std::thread(&FramePump::PollThread, this);
}

void FramePump::Stop() {
    {
        std::lock_guard<std::mutex> lk(ctrl_mutex_);
        if (!running_.load())
            return;  // 幂等
        stop_requested_ = true;
    }
    wake_cv_.notify_all();
    // V6 修复: 泵线程内调 Stop (帧回调→Destroy) 时禁止 join 自己 (见 [HANDOFF] 八 V6)
    if (poll_thread_.joinable() &&
        std::this_thread::get_id() != poll_thread_.get_id()) {
        poll_thread_.join();
    }
    running_ = false;
}

void FramePump::Pause() {
    paused_ = true;
}

void FramePump::Resume() {
    paused_ = false;
}

bool FramePump::UpdateFrame() {
    // 同步立即帧, 任意状态有效; 与 tick 串行 (frame_mutex_)
    std::lock_guard<std::mutex> lk(frame_mutex_);
    if (!frame_fn_)
        return false;
    bool ok = frame_fn_();
    // 显式请求不去重; 仅更新 dedupe 基线 (完整实现需 FrameFn 返回像素指针, 阶段二增量)
    if (ok) {
        has_last_frame_ = true;
    }
    return ok;
}

void FramePump::PollThread() {
    auto last_push = std::chrono::steady_clock::now();
    const auto tick = std::chrono::milliseconds(plan_.tick_ms > 0 ? plan_.tick_ms : 20);
    const auto heartbeat = std::chrono::milliseconds(plan_.heartbeat_ms > 0 ? plan_.heartbeat_ms : 0);
    const auto backoff = std::chrono::milliseconds(plan_.fail_backoff_ms > 0 ? plan_.fail_backoff_ms : 200);

    while (!stop_requested_.load()) {
        // tick 等待 (可被 Stop 打断)
        {
            std::unique_lock<std::mutex> lk(ctrl_mutex_);
            if (wake_cv_.wait_for(lk, tick, [this] { return stop_requested_.load(); }))
                break;  // Stop 打断
        }

        bool is_paused = paused_.load();
        // 暂停且不照推心跳 → 跳过
        if (is_paused && !plan_.heartbeat_when_paused)
            continue;

        std::lock_guard<std::mutex> flk(frame_mutex_);
        if (!frame_fn_)
            continue;

        bool changed = true;  // 默认恒真 (impress 无 probe)
        if (changed_fn_) {
            try {
                changed = changed_fn_();
            } catch (...) {
                changed = true;  // 探测失败保守推帧
            }
        }

        // 心跳: 静态兜底
        auto now = std::chrono::steady_clock::now();
        bool heartbeat_due = (plan_.heartbeat_ms > 0) && (now - last_push >= heartbeat);

        if (!changed && !heartbeat_due)
            continue;

        bool ok = false;
        try {
            ok = frame_fn_();
        } catch (...) {
            ok = false;
        }

        if (ok) {
            last_push = now;
        } else {
            std::unique_lock<std::mutex> lk(ctrl_mutex_);
            wake_cv_.wait_for(lk, backoff, [this] { return stop_requested_.load(); });
        }
    }
}
