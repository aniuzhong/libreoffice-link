// frame_pump.h — 三链统一帧泵 (经验 42 治理; 会话基础设施, 非平台层)。
// 契约/锁纪律/生命周期不变量见 [framepump] §0; 设计决策见 [framepump] §1-§5。
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct FramePumpPlan {
    int  tick_ms = 20;                  // 探测+心跳粒度上限 (唤醒频率 = 1000/tick)
    int  heartbeat_ms = 100;            // 静态兜底推帧; 0 = 无心跳 (impress 现状)
    bool heartbeat_when_paused = false; // 暂停中心跳照推 (calc 现状 true, 迁移期保留)
    bool dedupe = true;                 // 像素级去重 (见五、帧泵专项 5.1 决策三; 只作用于周期路径)
    int  fail_backoff_ms = 200;         // 抓帧失败退避 (impress 现状值)
};

class FramePump {
public:
    // FrameFn: 抓一帧并投递(cb_); 返回成败。执行始终持 frame_mutex_。
    // ChangeFn(可选): 内容是否可能变化(calc 视口签名 / writer 脏位);
    //   nullptr = 恒真(impress 现状)。
    using FrameFn  = std::function<bool()>;
    using ChangeFn = std::function<bool()>;

    FramePump(const char* tag, FramePumpPlan plan, FrameFn frame, ChangeFn changed = nullptr);
    ~FramePump();                       // 内部 Stop

    FramePump(const FramePump&) = delete;
    FramePump& operator=(const FramePump&) = delete;

    void Start();
    void Stop();
    void Pause();
    void Resume();
    bool UpdateFrame();                 // 同步立即帧, 任意状态有效
    bool running() const { return running_.load(); }

private:
    void PollThread();

    std::string tag_;
    FramePumpPlan plan_;
    FrameFn  frame_fn_;
    ChangeFn changed_fn_;

    std::mutex              frame_mutex_;     // 串行所有 FrameFn 执行 (tick + UpdateFrame)
    std::mutex              ctrl_mutex_;      // 保护 Start/Stop/Pause/Resume 状态变更
    std::condition_variable wake_cv_;         // Stop 可立即打断 tick 等待
    std::thread             poll_thread_;

    std::atomic<bool> running_{false};  // 泵线程是否运行 (Stopped 后 false)
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_requested_{false};

    // dedupe 状态 (frame_mutex_ 保护)
    bool     has_last_frame_ = false;
    std::vector<uint8_t> last_frame_;
};
