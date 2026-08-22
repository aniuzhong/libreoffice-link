// frame_pump.h — 三链统一帧泵 (经验 42 治理; 会话基础设施, 非平台层)
//
// 契约 (HANDOFF.md 经验 42 详述 / 五、帧泵专项):
//   状态机: Idle ──Start──▶ Running ⇄(Pause/Resume)⇄ Paused
//           任意状态 ──Stop──▶ Stopped; Stopped ──Start──▶ Running
//   Start:        幂等; 任何状态调用后 = Running 且未暂停
//   Stop:         幂等; join 泵线程, 排空在途帧; 之后无自动推帧
//   Pause:        冻结周期推帧 (heartbeat 是否照推 = plan 字段); 不影响 UpdateFrame
//   Resume:       恢复周期推帧
//   UpdateFrame:  同步立即帧, Running/Paused/Stopped 任何状态有效; 与 tick 串行
//
// 锁纪律 (五、帧泵专项 5.1 决策二):
//   调用泵方法时不得持有会话锁 mu_
//   FrameFn/ChangeFn 内部自取所需的短会话锁
//   全局锁序: frame_mutex_ → mu_(短), 严禁反向
//
// 生命周期不变量: 泵必须先 Stop, 会话才能清 UNO 对象/平台资源
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
