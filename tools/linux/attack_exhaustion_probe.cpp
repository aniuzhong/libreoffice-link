// attack_exhaustion_probe.cpp — 资源耗尽攻击探针
// 目标：耗尽系统资源导致卡死/崩溃
#include <abi/abi.h>

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

// 攻击1: 耗尽slot资源（创建大量会话不销毁）
void attack_slot_exhaustion(const char* xlsx, const char* pptx, int session_count) {
    printf("Starting slot exhaustion attack (%d sessions)\n", session_count);
    fflush(stdout);
    
    std::vector<void*> calc_sessions;
    std::vector<void*> impress_sessions;
    
    for (int i = 0; i < session_count; i++) {
        void* calc_s = CalcSessionCreate(xlsx, "", "attack_exhaustion_probe", OnFrame, nullptr, 1920, 1080);
        if (calc_s) {
            CalcSessionStart(calc_s);
            calc_sessions.push_back(calc_s);
        }
        
        void* impress_s = ImpressSessionCreate(pptx, "", "attack_exhaustion_probe", OnFrame, nullptr, 1920, 1080);
        if (impress_s) {
            ImpressSessionStart(impress_s);
            impress_sessions.push_back(impress_s);
        }
        
        if (i % 10 == 0) {
            printf("Created %d calc + %d impress sessions\n", (int)calc_sessions.size(), (int)impress_sessions.size());
            fflush(stdout);
        }
        
        // 如果创建失败，说明资源耗尽
        if (!calc_s && !impress_s) {
            printf("Slot exhaustion point reached at %d sessions\n", i);
            break;
        }
    }
    
    printf("Holding %zu calc + %zu impress sessions (waiting for crash)\n", 
           calc_sessions.size(), impress_sessions.size());
    fflush(stdout);
    
    // 持续持有资源，观察是否崩溃
    sleep(30);
    
    printf("No crash after 30s, cleaning up...\n");
    for (auto s : calc_sessions) {
        CalcSessionStop(s);
        CalcSessionDestroy(s);
    }
    for (auto s : impress_sessions) {
        ImpressSessionStop(s);
        ImpressSessionDestroy(s);
    }
}

// 攻击2: 极端分辨率攻击（尝试超过16位坐标限制）
void attack_extreme_resolution_exhaustion(const char* xlsx, const char* pptx) {
    printf("Starting extreme resolution exhaustion attack\n");
    fflush(stdout);
    
    // 尝试极端大分辨率
    int extreme_sizes[] = {100000, 200000, 500000, 1000000};
    
    for (int size : extreme_sizes) {
        printf("Testing resolution: %dx%d\n", size, size);
        fflush(stdout);
        
        void* calc_s = CalcSessionCreate(xlsx, "", "attack_exhaustion_probe", OnFrame, nullptr, size, size);
        if (calc_s) {
            printf("  Calc accepted %dx%d\n", size, size);
            CalcSessionDestroy(calc_s);
        } else {
            printf("  Calc rejected %dx%d\n", size, size);
        }
        
        void* impress_s = ImpressSessionCreate(pptx, "", "attack_exhaustion_probe", OnFrame, nullptr, size, size);
        if (impress_s) {
            printf("  Impress accepted %dx%d\n", size, size);
            ImpressSessionDestroy(impress_s);
        } else {
            printf("  Impress rejected %dx%d\n", size, size);
        }
    }
}

// 攻击3: 内存压力攻击（连续创建大分辨率会话）
void attack_memory_pressure(const char* xlsx, const char* pptx, int count) {
    printf("Starting memory pressure attack (%d sessions)\n", count);
    fflush(stdout);
    
    std::vector<void*> sessions;
    
    for (int i = 0; i < count; i++) {
        // 使用较大分辨率增加内存消耗
        void* calc_s = CalcSessionCreate(xlsx, "", "attack_exhaustion_probe", OnFrame, nullptr, 3840, 2160);
        if (calc_s) {
            CalcSessionStart(calc_s);
            sessions.push_back(calc_s);
        }
        
        void* impress_s = ImpressSessionCreate(pptx, "", "attack_exhaustion_probe", OnFrame, nullptr, 3840, 2160);
        if (impress_s) {
            ImpressSessionStart(impress_s);
            sessions.push_back(impress_s);
        }
        
        if (i % 5 == 0) {
            printf("Created %d sessions, memory pressure increasing...\n", (int)sessions.size());
            fflush(stdout);
        }
        
        // 如果内存耗尽导致创建失败
        if (!calc_s && !impress_s) {
            printf("Memory exhaustion point reached\n");
            break;
        }
    }
    
    printf("Holding %zu sessions under memory pressure\n", sessions.size());
    sleep(20);
    
    for (auto s : sessions) {
        CalcSessionStop(s);
        CalcSessionDestroy(s);
    }
}

// 攻击4: 并发资源争用（多线程同时创建大量会话）
void attack_concurrent_resource_race(const char* xlsx, const char* pptx, int thread_count, int sessions_per_thread) {
    printf("Starting concurrent resource race (%d threads, %d sessions each)\n", thread_count, sessions_per_thread);
    fflush(stdout);
    
    std::vector<std::thread> threads;
    std::atomic<int> total_created{0};
    std::atomic<int> total_failed{0};
    
    for (int t = 0; t < thread_count; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < sessions_per_thread; i++) {
                void* calc_s = CalcSessionCreate(xlsx, "", "attack_exhaustion_probe", OnFrame, nullptr, 1920, 1080);
                if (calc_s) {
                    total_created++;
                    CalcSessionStart(calc_s);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    CalcSessionStop(calc_s);
                    CalcSessionDestroy(calc_s);
                } else {
                    total_failed++;
                }
                
                void* impress_s = ImpressSessionCreate(pptx, "", "attack_exhaustion_probe", OnFrame, nullptr, 1920, 1080);
                if (impress_s) {
                    total_created++;
                    ImpressSessionStart(impress_s);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    ImpressSessionStop(impress_s);
                    ImpressSessionDestroy(impress_s);
                } else {
                    total_failed++;
                }
            }
        });
    }
    
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    
    printf("Concurrent race completed: created=%d failed=%d\n", total_created.load(), total_failed.load());
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <xlsx> <pptx> [attack_type=1|2|3|4|all] [param=32]\n", argv[0]);
        fprintf(stderr, "  attack_type: 1=slot exhaustion, 2=extreme resolution, 3=memory pressure, 4=concurrent race, all=all\n");
        fprintf(stderr, "  param: session count for type1/3, thread count for type4\n");
        return 2;
    }
    
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int attack_type = (argc >= 4) ? 
        (strcmp(argv[3], "all") == 0 ? 4 : atoi(argv[3])) : 1;
    int param = (argc >= 5) ? atoi(argv[4]) : 32;
    
    printf("=== RESOURCE EXHAUSTION ATTACK PROBE ===\n");
    printf("xlsx=%s pptx=%s attack_type=%d param=%d\n", xlsx, pptx, attack_type, param);
    fflush(stdout);
    
    int score = 0;
    
    // 攻击1: slot耗尽
    if (attack_type == 1 || attack_type == 4) {
        printf("\n--- Attack 1: Slot Exhaustion ---\n");
        fflush(stdout);
        
        try {
            attack_slot_exhaustion(xlsx, pptx, param);
            printf(">>> Attack 1: No crash (0 point)\n");
        } catch (...) {
            printf(">>> ATTACK 1 SUCCESS: Exception/crash detected (1 point)\n");
            score++;
        }
        fflush(stdout);
    }
    
    // 攻击2: 极端分辨率
    if (attack_type == 2 || attack_type == 4) {
        printf("\n--- Attack 2: Extreme Resolution ---\n");
        fflush(stdout);
        
        try {
            attack_extreme_resolution_exhaustion(xlsx, pptx);
            printf(">>> Attack 2: No crash (0 point)\n");
        } catch (...) {
            printf(">>> ATTACK 2 SUCCESS: Exception/crash detected (1 point)\n");
            score++;
        }
        fflush(stdout);
    }
    
    // 攻击3: 内存压力
    if (attack_type == 3 || attack_type == 4) {
        printf("\n--- Attack 3: Memory Pressure ---\n");
        fflush(stdout);
        
        try {
            attack_memory_pressure(xlsx, pptx, param);
            printf(">>> Attack 3: No crash (0 point)\n");
        } catch (...) {
            printf(">>> ATTACK 3 SUCCESS: Exception/crash detected (1 point)\n");
            score++;
        }
        fflush(stdout);
    }
    
    // 攻击4: 并发资源争用
    if (attack_type == 4) {
        printf("\n--- Attack 4: Concurrent Resource Race ---\n");
        fflush(stdout);
        
        try {
            attack_concurrent_resource_race(xlsx, pptx, 16, 20);
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
