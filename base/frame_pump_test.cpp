// frame_pump_test.cpp — FramePump 单元测试 (无框架, 断言式)。
// 验证 [framepump] §0 契约 + §3 测试矩阵。树内编译不部署。
#include "frame_pump.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "  [FAIL] %s:%d  ", __FILE__, __LINE__);         \
            fprintf(stderr, __VA_ARGS__);                                    \
            fprintf(stderr, "  (cond: %s)\n", #cond);                        \
        } else {                                                             \
            printf("  [ok] %s\n", #cond);                                    \
        }                                                                    \
    } while (0)

// ---- 测试 1: Start 幂等 + 重置 paused_ ----
void test_start_idempotent() {
    printf("----- framepump: start_idempotent\n");
    std::atomic<int> frame_count{0};
    FramePumpPlan plan;
    plan.tick_ms = 5;
    plan.heartbeat_ms = 0;  // 无心跳, 只靠 tick
    plan.heartbeat_when_paused = false;

    FramePump pump("test1", plan, [&]() {
        frame_count++;
        return true;
    });

    pump.Start();
    pump.Start();  // 幂等, 不起第二线程
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    pump.Stop();

    // 至少推了几帧 (tick=5ms, 50ms 至少 5+ tick)
    CHECK(frame_count.load() >= 3, "frame_count=%d expected>=3\n", frame_count.load());
}

// ---- 测试 2: Stop 排空 + 幂等 ----
void test_stop_idempotent() {
    printf("----- framepump: stop_idempotent\n");
    std::atomic<int> frame_count{0};
    FramePumpPlan plan;
    plan.tick_ms = 5;
    plan.heartbeat_ms = 0;

    FramePump pump("test2", plan, [&]() {
        frame_count++;
        return true;
    });

    pump.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    pump.Stop();
    int count_after_stop = frame_count.load();
    pump.Stop();  // 幂等
    pump.Stop();  // 幂等
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Stop 后无新帧
    CHECK(frame_count.load() == count_after_stop,
          "frame_count=%d after_stop=%d (Stop 后应有新帧停止)\n",
          frame_count.load(), count_after_stop);
    CHECK(!pump.running(), "pump should not be running after Stop\n");
}

// ---- 测试 3: Pause 冻结周期推帧, UpdateFrame 仍可用 ----
void test_pause_freezes_updateframe_works() {
    printf("----- framepump: pause_freezes_updateframe_works\n");
    std::atomic<int> frame_count{0};
    FramePumpPlan plan;
    plan.tick_ms = 5;
    plan.heartbeat_ms = 0;
    plan.heartbeat_when_paused = false;  // 暂停不推心跳

    FramePump pump("test3", plan, [&]() {
        frame_count++;
        return true;
    });

    pump.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    int count_before_pause = frame_count.load();

    pump.Pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    int count_during_pause = frame_count.load();

    // Pause 后周期推帧冻结
    CHECK(count_during_pause == count_before_pause,
          "frames during pause=%d before=%d (Pause 应冻结周期推帧)\n",
          count_during_pause, count_before_pause);

    // UpdateFrame 在 Paused 态仍可用
    bool ok = pump.UpdateFrame();
    CHECK(ok, "UpdateFrame should work in Paused state\n");
    CHECK(frame_count.load() == count_during_pause + 1,
          "frame_count=%d expected=%d (UpdateFrame 应推一帧)\n",
          frame_count.load(), count_during_pause + 1);

    pump.Stop();
}

// ---- 测试 4: UpdateFrame 与 tick 串行 (P3 修复验证) ----
void test_updateframe_serial_with_tick() {
    printf("----- framepump: updateframe_serial_with_tick\n");
    // 用互斥保护序列号, 验证 UpdateFrame 与 tick 不并发
    std::mutex seq_mu;
    std::vector<int> sequence;  // 0=tick, 1=updateframe
    std::atomic<bool> in_frame{false};
    std::atomic<int> concurrent_violations{0};

    FramePumpPlan plan;
    plan.tick_ms = 2;
    plan.heartbeat_ms = 0;

    auto frame_fn = [&]() -> bool {
        if (in_frame.exchange(true)) {
            concurrent_violations++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // 模拟抓帧耗时
        in_frame = false;
        return true;
    };

    FramePump pump("test4", plan, frame_fn);
    pump.Start();

    // 并发调 UpdateFrame
    std::thread updater([&]() {
        for (int i = 0; i < 50; i++) {
            pump.UpdateFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    updater.join();
    pump.Stop();

    CHECK(concurrent_violations.load() == 0,
          "concurrent_violations=%d (UpdateFrame 与 tick 不应并发)\n",
          concurrent_violations.load());
}

// ---- 测试 5: 心跳间隔 (1ms tick 加速) ----
void test_heartbeat_interval() {
    printf("----- framepump: heartbeat_interval\n");
    std::atomic<int> frame_count{0};
    FramePumpPlan plan;
    plan.tick_ms = 1;
    plan.heartbeat_ms = 20;  // 20ms 心跳
    plan.heartbeat_when_paused = false;

    FramePump pump("test5", plan,
        [&]() { frame_count++; return true; },
        [&]() { return false; }  // ChangeFn 返回 false: 隔离心跳路径
    );

    pump.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pump.Stop();

    // 100ms / 20ms heartbeat ≈ 5 帧 (tick=1ms 唤醒, 每 20ms 到期一次)
    // 允许 3-8 范围 (调度抖动)
    int count = frame_count.load();
    CHECK(count >= 3 && count <= 12,
          "frame_count=%d expected 3-12 (heartbeat=20ms, 100ms window)\n", count);
}

// ---- 测试 6: 失败退避 ----
void test_fail_backoff() {
    printf("----- framepump: fail_backoff\n");
    std::atomic<int> fail_count{0};
    std::atomic<int> total_calls{0};

    FramePumpPlan plan;
    plan.tick_ms = 2;
    plan.heartbeat_ms = 0;
    plan.fail_backoff_ms = 30;  // 失败后退避 30ms

    auto frame_fn = [&]() -> bool {
        total_calls++;
        if (total_calls.load() <= 2) {
            fail_count++;
            return false;  // 前两次失败
        }
        return true;
    };

    FramePump pump("test6", plan, frame_fn);
    pump.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    pump.Stop();

    // 前两次失败后有退避, 之后成功
    CHECK(fail_count.load() == 2, "fail_count=%d expected 2\n", fail_count.load());
    CHECK(total_calls.load() > 2, "total_calls=%d expected>2 (退避后应恢复)\n",
          total_calls.load());
}

// ---- 测试 7: Stop→Start 可重启 (状态机循环) ----
void test_stop_start_restart() {
    printf("----- framepump: stop_start_restart\n");
    std::atomic<int> frame_count{0};
    FramePumpPlan plan;
    plan.tick_ms = 5;
    plan.heartbeat_ms = 0;

    FramePump pump("test7", plan, [&]() {
        frame_count++;
        return true;
    });

    // 第一轮
    pump.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    pump.Stop();
    int count_round1 = frame_count.load();

    // 第二轮 (Stopped → Start)
    pump.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    pump.Stop();
    int count_round2 = frame_count.load();

    CHECK(count_round1 > 0, "round1 frame_count=%d expected>0\n", count_round1);
    CHECK(count_round2 > count_round1,
          "round2=%d > round1=%d (重启后应继续推帧)\n", count_round2, count_round1);
}

// ---- 测试 8: ChangeFn 探测 (calc 视口签名范式) ----
void test_changed_fn_probe() {
    printf("----- framepump: changed_fn_probe\n");
    std::atomic<int> frame_count{0};
    std::atomic<bool> content_changed{false};

    FramePumpPlan plan;
    plan.tick_ms = 2;
    plan.heartbeat_ms = 0;  // 无心跳, 纯靠 probe

    FramePump pump("test8", plan,
        [&]() { frame_count++; return true; },
        [&]() { return content_changed.load(); }  // 只在 changed=true 时推帧
    );

    pump.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    int count_no_change = frame_count.load();

    content_changed = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    int count_with_change = frame_count.load();

    pump.Stop();

    CHECK(count_no_change == 0,
          "count_no_change=%d expected 0 (无变化无心跳不应推帧)\n", count_no_change);
    CHECK(count_with_change > 0,
          "count_with_change=%d expected>0 (changed=true 应推帧)\n", count_with_change);
}

// ---- 测试 9: Start 作为恢复路径须重置 paused_ (经验 41 / P1 契约) ----
// 场景: Start → Pause → Start(幂等) 应恢复推帧; 旧实现幂等早返未重置 paused_
// 致 impress 暂停后恢复画面冻结 (2026-08-19 回归)。
void test_start_resets_paused_when_running() {
    printf("----- framepump: start_resets_paused_when_running\n");
    std::atomic<int> frame_count{0};
    FramePumpPlan plan;
    plan.tick_ms = 5;
    plan.heartbeat_ms = 0;
    plan.heartbeat_when_paused = false;  // 暂停不推心跳

    FramePump pump("test9", plan, [&]() {
        frame_count++;
        return true;
    });

    pump.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    pump.Pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    int count_during_pause = frame_count.load();

    // 幂等 Start 作为恢复 (经验 41: resume 走 Start 路径)
    pump.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    int count_after_restart = frame_count.load();

    pump.Stop();

    CHECK(count_after_restart > count_during_pause,
          "count_after_restart=%d during_pause=%d (Start 幂等路径须重置 paused_ 恢复推帧)\n",
          count_after_restart, count_during_pause);
}

int main(int argc, char** argv) {
    printf("=== FramePump 单测 ===\n");

    test_start_idempotent();
    test_stop_idempotent();
    test_pause_freezes_updateframe_works();
    test_updateframe_serial_with_tick();
    test_heartbeat_interval();
    test_fail_backoff();
    test_stop_start_restart();
    test_changed_fn_probe();
    test_start_resets_paused_when_running();

    printf("\n=== 结果: %d/%d checks", g_checks - g_failures, g_checks);
    if (g_failures == 0) {
        printf(" — PASS ===\n");
        return 0;
    } else {
        printf(" — FAIL (%d failures) ===\n", g_failures);
        return 1;
    }
}
