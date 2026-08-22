// attack_deadlock_probe.cpp — 卡死/崩溃专用攻击探针
// 目标：让程序卡死或崩溃，不计资源使用
#include <base/abi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>
#include <unistd.h>

static void OnFrame(const uint8_t* data, int32_t w, int32_t h, int32_t rp,
                    int32_t size, int32_t format, void* opaque) {
    (void)data; (void)w; (void)h; (void)rp; (void)size; (void)format; (void)opaque;
}

// 攻击1: 极端并发Create攻击（大规模并发）
void attack_extreme_concurrent_create(const char* xlsx, const char* pptx, int thread_count) {
    printf("Starting EXTREME concurrent create attack (%d threads)\n", thread_count);
    fflush(stdout);
    
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    std::atomic<int> fail_count{0};
    
    // 同时启动大量线程并发创建
    for (int i = 0; i < thread_count; i++) {
        threads.emplace_back([&, i]() {
            if (i % 2 == 0) {
                void* s = CalcSessionCreate(xlsx, "", "attack_deadlock_probe", OnFrame, nullptr, 1920, 1080);
                if (s) {
                    success_count++;
                    CalcSessionDestroy(s);
                } else {
                    fail_count++;
                }
            } else {
                void* s = ImpressSessionCreate(pptx, "", "attack_deadlock_probe", OnFrame, nullptr, 1920, 1080);
                if (s) {
                    success_count++;
                    ImpressSessionDestroy(s);
                } else {
                    fail_count++;
                }
            }
        });
    }
    
    // 等待所有线程
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    
    printf("EXTREME attack completed: success=%d fail=%d\n", success_count.load(), fail_count.load());
    fflush(stdout);
}

// 攻击2: 无限循环Start/Stop攻击（制造状态混乱）
void attack_infinite_start_stop(const char* xlsx, const char* pptx) {
    printf("Starting INFINITE Start/Stop attack (will run until crash)\n");
    fflush(stdout);
    
    void* calc_s = CalcSessionCreate(xlsx, "", "attack_deadlock_probe", OnFrame, nullptr, 1920, 1080);
    void* impress_s = ImpressSessionCreate(pptx, "", "attack_deadlock_probe", OnFrame, nullptr, 1920, 1080);
    
    if (!calc_s || !impress_s) {
        printf("Setup failed\n");
        return;
    }
    
    CalcSessionStart(calc_s);
    ImpressSessionStart(impress_s);
    
    // 无限循环快速Start/Stop，制造状态混乱
    for (int i = 0; ; i++) {
        CalcSessionStop(calc_s);
        ImpressSessionStop(impress_s);
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        
        CalcSessionStart(calc_s);
        ImpressSessionStart(impress_s);
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        
        if (i % 1000 == 0) {
            printf("Iteration %d - still running\n", i);
            fflush(stdout);
        }
    }
}

// 攻击3: 跨线程状态攻击（多线程同时操作同一会话）
void attack_cross_thread_state(const char* xlsx) {
    printf("Starting cross-thread state attack\n");
    fflush(stdout);
    
    void* calc_s = CalcSessionCreate(xlsx, "", "attack_deadlock_probe", OnFrame, nullptr, 1920, 1080);
    if (!calc_s) {
        printf("Setup failed\n");
        return;
    }
    
    CalcSessionStart(calc_s);
    
    std::vector<std::thread> threads;
    
    // 多个线程同时操作同一个会话的不同功能
    for (int i = 0; i < 10; i++) {
        threads.emplace_back([calc_s, i]() {
            for (int j = 0; j < 1000; j++) {
                switch (i % 5) {
                    case 0: CalcSessionNextPage(calc_s); break;
                    case 1: CalcSessionPreviousPage(calc_s); break;
                    case 2: CalcSessionPause(calc_s); break;
                    case 3: CalcSessionResume(calc_s); break;
                    case 4: CalcSessionUpdateFrame(calc_s); break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
    }
    
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    
    CalcSessionStop(calc_s);
    CalcSessionDestroy(calc_s);
    
    printf("Cross-thread state attack completed\n");
    fflush(stdout);
}

// 攻击4: 极速创建销毁循环（最大化资源争用）
void attack_rapid_create_destroy_loop(const char* xlsx, const char* pptx, int iterations) {
    printf("Starting RAPID create/destroy loop (%d iterations)\n", iterations);
    fflush(stdout);
    
    for (int i = 0; i < iterations; i++) {
        void* calc_s = CalcSessionCreate(xlsx, "", "attack_deadlock_probe", OnFrame, nullptr, 1920, 1080);
        if (calc_s) {
            CalcSessionStart(calc_s);
            CalcSessionStop(calc_s);
            CalcSessionDestroy(calc_s);
        }
        
        void* impress_s = ImpressSessionCreate(pptx, "", "attack_deadlock_probe", OnFrame, nullptr, 1920, 1080);
        if (impress_s) {
            ImpressSessionStart(impress_s);
            ImpressSessionStop(impress_s);
            ImpressSessionDestroy(impress_s);
        }
        
        if (i % 100 == 0) {
            printf("Progress: %d/%d\n", i, iterations);
            fflush(stdout);
        }
    }
    
    printf("RAPID loop completed\n");
    fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <xlsx> <pptx> [attack_type=1|2|3|4|all] [param=64]\n", argv[0]);
        fprintf(stderr, "  attack_type: 1=extreme concurrent, 2=infinite start/stop, 3=cross-thread, 4=rapid loop, all=all\n");
        fprintf(stderr, "  param: thread count for type1, iterations for type4\n");
        return 2;
    }
    
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int attack_type = (argc >= 4) ? 
        (strcmp(argv[3], "all") == 0 ? 4 : atoi(argv[3])) : 1;
    int param = (argc >= 5) ? atoi(argv[4]) : 64;
    
    printf("=== DEADLOCK/CRASH ATTACK PROBE ===\n");
    printf("xlsx=%s pptx=%s attack_type=%d param=%d\n", xlsx, pptx, attack_type, param);
    fflush(stdout);
    
    int score = 0;
    
    // 攻击1: 极端并发
    if (attack_type == 1 || attack_type == 4) {
        printf("\n--- Attack 1: EXTREME Concurrent Create ---\n");
        fflush(stdout);
        
        try {
            attack_extreme_concurrent_create(xlsx, pptx, param);
            printf(">>> Attack 1: No crash (0 point)\n");
        } catch (...) {
            printf(">>> ATTACK 1 SUCCESS: Exception/ crash detected (1 point)\n");
            score++;
        }
        fflush(stdout);
    }
    
    // 攻击2: 无限循环
    if (attack_type == 2 || attack_type == 4) {
        printf("\n--- Attack 2: INFINITE Start/Stop (manual stop if needed) ---\n");
        fflush(stdout);
        
        try {
            attack_infinite_start_stop(xlsx, pptx);
            printf(">>> Attack 2: No crash (0 point)\n");
        } catch (...) {
            printf(">>> ATTACK 2 SUCCESS: Exception/crash detected (1 point)\n");
            score++;
        }
        fflush(stdout);
    }
    
    // 攻击3: 跨线程状态
    if (attack_type == 3 || attack_type == 4) {
        printf("\n--- Attack 3: Cross-Thread State Attack ---\n");
        fflush(stdout);
        
        try {
            attack_cross_thread_state(xlsx);
            printf(">>> Attack 3: No crash (0 point)\n");
        } catch (...) {
            printf(">>> ATTACK 3 SUCCESS: Exception/crash detected (1 point)\n");
            score++;
        }
        fflush(stdout);
    }
    
    // 攻击4: 极速循环
    if (attack_type == 4) {
        printf("\n--- Attack 4: RAPID Create/Destroy Loop ---\n");
        fflush(stdout);
        
        try {
            attack_rapid_create_destroy_loop(xlsx, pptx, param);
            printf(">>> Attack 4: No crash (0 point)\n");
        } catch (...) {
            printf(">>> ATTACK 4 SUCCESS: Exception/crash detected (1 point)\n");
            score++;
        }
        fflush(stdout);
    }
    
    printf("\n=== FINAL SCORE: %d/4 ===\n", score);
    printf("Note: Each crash/deadlock = 1 point\n");
    fflush(stdout);
    
    return 0;
}
