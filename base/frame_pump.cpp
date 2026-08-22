// frame_pump.cpp — 三链统一帧泵实现 (经验 42 治理)
#include "frame_pump.h"

#include <cstdio>
#include <algorithm>
#include <cstring>

FramePump::FramePump(const char* tag, FramePumpPlan plan, FrameFn frame, ChangeFn changed)
    : tag_(tag ? tag : ""), plan_(plan), frame_fn_(std::move(frame)), changed_fn_(std::move(changed)) {
}

FramePump::~FramePump() {
    Stop();
}

void FramePump::Start() {
    std::lock_guard<std::mutex> lk(ctrl_mutex_);
    // 契约 (HANDOFF.md 经验 42 详述契约表): "Start=任何状态→Running 未暂停"。
    // 幂等路径也须重置 paused_ —— 暂停后用 Start 恢复(经验 41 路径)依赖此行为,
    // 否则 paused_ 残留 → tick 跳过推帧 → 画面冻结(2026-08-19 impress 回归复现)。
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
    // V6 修复: 若当前线程就是泵线程 (如帧回调里调 Destroy→Stop), join 自己
    // 是未定义行为 (通常死锁)。跳过 join, 设 stop_requested 后泵线程自己退出循环。
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
    // 显式请求不走去重 (调用方要的就是一帧)
    if (ok) {
        // 更新 dedupe 基线 (避免下一 tick 因 memcmp 相同而跳过)
        // 注: FrameFn 内部已投递 cb_, 这里只更新基线
        has_last_frame_ = true;
        // last_frame_ 不在此更新 (FrameFn 内部不知道像素地址)
        // dedupe 的完整实现需 FrameFn 返回像素指针, 阶段二增量
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

        // 探测: 内容是否可能变化
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
            // 抓帧失败退避
            std::unique_lock<std::mutex> lk(ctrl_mutex_);
            wake_cv_.wait_for(lk, backoff, [this] { return stop_requested_.load(); });
        }
    }
}
