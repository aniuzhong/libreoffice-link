// attack_race_abi.cpp — C ABI 接口破坏比赛
// 攻击向量: Start/Stop/Pause/Resume 高频交替 + 并发 Destroy
// 原理: Destroy 的 close() 与 Start 的 resume() 并发, LO 内部 UNO 对象半销毁
#include <base/abi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <unistd.h>

static std::atomic<long long> s_frames{0};
static void OnFrame(const uint8_t*, int32_t, int32_t, int32_t, int32_t, int32_t, void*) {
    s_frames.fetch_add(1, std::memory_order_relaxed);
}

// 攻击1: Start/Stop 高频交替 + 并发 Destroy
// 线程A: Start→Stop→Start→Stop→... 无限循环
// 线程B: 随机延迟后 Destroy
int attack_start_stop_destroy(const char* path, bool is_calc) {
    printf("[A1] Start/Stop storm + concurrent Destroy\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::atomic<int> score{0};

    for (int round = 0; round < 20; round++) {
        void* s = is_calc
            ? CalcSessionCreate(path, "", "abi_race", OnFrame, nullptr, 1920, 1080)
            : ImpressSessionCreate(path, "", "abi_race", OnFrame, nullptr, 1920, 1080);
        if (!s) continue;

        if (is_calc) CalcSessionStart(s); else ImpressSessionStart(s);

        // 线程A: Start/Stop 高频交替
        std::thread storm([&]() {
            int i = 0;
            while (!stop.load() && i < 5000) {
                if (is_calc) {
                    CalcSessionStop(s);
                    CalcSessionStart(s);
                } else {
                    ImpressSessionStop(s);
                    ImpressSessionStart(s);
                }
                i++;
            }
        });

        // 线程B: 随机延迟后 Destroy
        std::thread killer([&]() {
            std::this_thread::sleep_for(std::chrono::microseconds(rand() % 5000));
            if (is_calc) CalcSessionDestroy(s); else ImpressSessionDestroy(s);
            stop.store(true);
        });

        storm.join();
        killer.join();
        stop.store(false);
        printf("[A1] round %d done\n", round); fflush(stdout);
    }
    printf("[A1] done\n"); fflush(stdout);
    return 0;
}

// 攻击2: 多 session 并发 Start/Stop + 跨 session Destroy
// 多个 session 同时 Start/Stop, 然后随机 Destroy 其中一个
int attack_multi_session_cross_destroy(const char* path, bool is_calc) {
    printf("[A2] Multi-session cross Destroy\n"); fflush(stdout);
    std::atomic<bool> stop{false};

    for (int round = 0; round < 10; round++) {
        // 创建 4 个 session
        void* sessions[4];
        for (int i = 0; i < 4; i++) {
            sessions[i] = is_calc
                ? CalcSessionCreate(path, "", "abi_race", OnFrame, nullptr, 1920, 1080)
                : ImpressSessionCreate(path, "", "abi_race", OnFrame, nullptr, 1920, 1080);
            if (sessions[i]) {
                if (is_calc) CalcSessionStart(sessions[i]); else ImpressSessionStart(sessions[i]);
            }
        }

        // 4 个线程各自操作自己的 session
        std::vector<std::thread> ts;
        for (int i = 0; i < 4; i++) {
            ts.emplace_back([&, i]() {
                void* s = sessions[i];
                if (!s) return;
                for (int j = 0; j < 2000; j++) {
                    if (is_calc) {
                        CalcSessionNextPage(s);
                        CalcSessionPreviousPage(s);
                        CalcSessionUpdateFrame(s);
                        CalcSessionSetResolution(s, 640 + (j%5)*320, 480 + (j%5)*240);
                    } else {
                        ImpressSessionNextPage(s);
                        ImpressSessionPreviousPage(s);
                        ImpressSessionUpdateFrame(s);
                        ImpressSessionSetResolution(s, 640 + (j%5)*320, 480 + (j%5)*240);
                    }
                }
            });
        }

        // 主线程随机 Destroy 一个 session
        std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 100 + 10));
        int kill_idx = rand() % 4;
        if (sessions[kill_idx]) {
            if (is_calc) CalcSessionDestroy(sessions[kill_idx]); else ImpressSessionDestroy(sessions[kill_idx]);
            sessions[kill_idx] = nullptr;
        }

        for (auto& t : ts) t.join();
        for (int i = 0; i < 4; i++) {
            if (sessions[i]) {
                if (is_calc) CalcSessionDestroy(sessions[i]); else ImpressSessionDestroy(sessions[i]);
            }
        }
        printf("[A2] round %d done\n", round); fflush(stdout);
    }
    printf("[A2] done\n"); fflush(stdout);
    return 0;
}

// 攻击3: Pause/Resume 高频交替 + 并发 NextPage + 并发 Destroy
int attack_pause_resume_storm(const char* path, bool is_calc) {
    printf("[A3] Pause/Resume storm + NextPage + Destroy\n"); fflush(stdout);
    std::atomic<bool> stop{false};

    for (int round = 0; round < 15; round++) {
        void* s = is_calc
            ? CalcSessionCreate(path, "", "abi_race", OnFrame, nullptr, 1920, 1080)
            : ImpressSessionCreate(path, "", "abi_race", OnFrame, nullptr, 1920, 1080);
        if (!s) continue;
        if (is_calc) CalcSessionStart(s); else ImpressSessionStart(s);

        // 线程A: Pause/Resume 高频交替
        std::thread pr([&]() {
            for (int i = 0; i < 3000; i++) {
                if (is_calc) {
                    CalcSessionPause(s);
                    CalcSessionResume(s);
                } else {
                    ImpressSessionPause(s);
                    ImpressSessionResume(s);
                }
            }
        });

        // 线程B: NextPage/PreviousPage 高频
        std::thread nav([&]() {
            for (int i = 0; i < 3000; i++) {
                if (is_calc) {
                    CalcSessionNextPage(s);
                    CalcSessionPreviousPage(s);
                } else {
                    ImpressSessionNextPage(s);
                    ImpressSessionPreviousPage(s);
                }
            }
        });

        // 线程C: UpdateFrame 高频
        std::thread cap([&]() {
            for (int i = 0; i < 3000; i++) {
                if (is_calc) CalcSessionUpdateFrame(s); else ImpressSessionUpdateFrame(s);
            }
        });

        // 线程D: 随机 Destroy
        std::thread killer([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 200 + 50));
            if (is_calc) CalcSessionDestroy(s); else ImpressSessionDestroy(s);
            stop.store(true);
        });

        pr.join(); nav.join(); cap.join(); killer.join();
        stop.store(false);
        printf("[A3] round %d done\n", round); fflush(stdout);
    }
    printf("[A3] done\n"); fflush(stdout);
    return 0;
}

// 攻击4: SetMute 高频风暴 (impress only)
int attack_setmute_storm(const char* path) {
    printf("[A4] SetMute storm\n"); fflush(stdout);
    void* s = ImpressSessionCreate(path, "", "abi_race", OnFrame, nullptr, 1920, 1080);
    if (!s) { printf("[A4] create failed\n"); return 0; }
    ImpressSessionStart(s);

    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; i++) {
        ts.emplace_back([&]() {
            int v = 0;
            while (!stop.load()) {
                ImpressSessionSetMute(s, v++ & 1);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(5));
    stop.store(true);
    for (auto& t : ts) t.join();
    ImpressSessionDestroy(s);
    printf("[A4] done\n"); fflush(stdout);
    return 0;
}

// 攻击5: Calc 混合风暴 (MoveScroll + SetSheet + SetScale + SetResolution)
int attack_calc_mixed_storm(const char* path) {
    printf("[A5] Calc mixed storm\n"); fflush(stdout);
    void* s = CalcSessionCreate(path, "", "abi_race", OnFrame, nullptr, 1920, 1080);
    if (!s) { printf("[A5] create failed\n"); return 0; }
    CalcSessionStart(s);

    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int i = 0; i < 6; i++) {
        ts.emplace_back([&, i]() {
            while (!stop.load()) {
                switch (i % 4) {
                    case 0: CalcSessionMoveScroll(s, rand() % 1000, rand() % 1000); break;
                    case 1: CalcSessionSetSheet(s, rand() % 5); break;
                    case 2: CalcSessionSetScale(s, 50 + rand() % 300); break;
                    case 3: CalcSessionSetResolution(s, 640 + rand() % 1280, 480 + rand() % 720); break;
                }
                CalcSessionUpdateFrame(s);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(6));
    stop.store(true);
    for (auto& t : ts) t.join();
    CalcSessionDestroy(s);
    printf("[A5] done\n"); fflush(stdout);
    return 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { fprintf(stderr, "usage: %s <xlsx> <pptx> [att=1|2|3|4|5|all]\n", argv[0]); return 2; }
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int att = (argc >= 4 && strcmp(argv[3], "all") == 0) ? 0 : atoi(argv[3]);
    int score = 0;

    printf("=== C ABI RACE ATTACK ===\n"); fflush(stdout);

    if (att == 0 || att == 1) {
        printf("\n--- Attack 1: Start/Stop storm + Destroy ---\n"); fflush(stdout);
        try { attack_start_stop_destroy(xlsx, true); } catch (...) { printf(">>> A1 CALC CRASH (1pt)\n"); score++; }
        try { attack_start_stop_destroy(pptx, false); } catch (...) { printf(">>> A1 IMPRESS CRASH (1pt)\n"); score++; }
    }
    if (att == 0 || att == 2) {
        printf("\n--- Attack 2: Multi-session cross Destroy ---\n"); fflush(stdout);
        try { attack_multi_session_cross_destroy(xlsx, true); } catch (...) { printf(">>> A2 CALC CRASH (1pt)\n"); score++; }
        try { attack_multi_session_cross_destroy(pptx, false); } catch (...) { printf(">>> A2 IMPRESS CRASH (1pt)\n"); score++; }
    }
    if (att == 0 || att == 3) {
        printf("\n--- Attack 3: Pause/Resume storm + NextPage + Destroy ---\n"); fflush(stdout);
        try { attack_pause_resume_storm(xlsx, true); } catch (...) { printf(">>> A3 CALC CRASH (1pt)\n"); score++; }
        try { attack_pause_resume_storm(pptx, false); } catch (...) { printf(">>> A3 IMPRESS CRASH (1pt)\n"); score++; }
    }
    if (att == 0 || att == 4) {
        printf("\n--- Attack 4: SetMute storm ---\n"); fflush(stdout);
        try { attack_setmute_storm(pptx); } catch (...) { printf(">>> A4 CRASH (1pt)\n"); score++; }
    }
    if (att == 0 || att == 5) {
        printf("\n--- Attack 5: Calc mixed storm ---\n"); fflush(stdout);
        try { attack_calc_mixed_storm(xlsx); } catch (...) { printf(">>> A5 CRASH (1pt)\n"); score++; }
    }

    printf("\n=== FINAL SCORE: %d/8 ===\n", score); fflush(stdout);
    return 0;
}
