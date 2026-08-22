// attack_state_corruption_probe.cpp — 状态机破坏攻击探针
// 目标：破坏状态机导致卡死/崩溃
#include <base/abi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <unistd.h>

static void OnFrame(const uint8_t* data, int32_t w, int32_t h, int32_t rp,
                    int32_t size, int32_t format, void* opaque) {
    (void)data; (void)w; (void)h; (void)rp; (void)size; (void)format; (void)opaque;
}

// 攻击1: 状态机顺序破坏（乱序调用）
void attack_state_sequence_corruption(const char* xlsx, const char* pptx) {
    printf("Starting state sequence corruption attack\n");
    fflush(stdout);
    
    void* calc_s = CalcSessionCreate(xlsx, "", "attack_state_corruption_probe", OnFrame, nullptr, 1920, 1080);
    void* impress_s = ImpressSessionCreate(pptx, "", "attack_state_corruption_probe", OnFrame, nullptr, 1920, 1080);
    
    if (!calc_s || !impress_s) {
        printf("Setup failed\n");
        return;
    }
    
    // 乱序调用：Stop before Start
    printf("Testing Stop before Start...\n");
    CalcSessionStop(calc_s);
    ImpressSessionStop(impress_s);
    
    // Resume before Start
    printf("Testing Resume before Start...\n");
    CalcSessionResume(calc_s);
    ImpressSessionResume(impress_s);
    
    // Pause before Start
    printf("Testing Pause before Start...\n");
    CalcSessionPause(calc_s);
    ImpressSessionPause(impress_s);
    
    // 现在正常Start
    CalcSessionStart(calc_s);
    ImpressSessionStart(impress_s);
    
    // Start while running
    printf("Testing Start while running...\n");
    for (int i = 0; i < 100; i++) {
        CalcSessionStart(calc_s);
        ImpressSessionStart(impress_s);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    // Stop while already stopped
    printf("Testing Stop while stopped...\n");
    for (int i = 0; i < 100; i++) {
        CalcSessionStop(calc_s);
        ImpressSessionStop(impress_s);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    CalcSessionDestroy(calc_s);
    ImpressSessionDestroy(impress_s);
    
    printf("State sequence corruption attack completed\n");
    fflush(stdout);
}

// 攻击2: 极速状态切换攻击
void attack_rapid_state_switching(const char* xlsx, const char* pptx, int iterations) {
    printf("Starting rapid state switching attack (%d iterations)\n", iterations);
    fflush(stdout);
    
    void* calc_s = CalcSessionCreate(xlsx, "", "attack_state_corruption_probe", OnFrame, nullptr, 1920, 1080);
    void* impress_s = ImpressSessionCreate(pptx, "", "attack_state_corruption_probe", OnFrame, nullptr, 1920, 1080);
    
    if (!calc_s || !impress_s) {
        printf("Setup failed\n");
        return;
    }
    
    CalcSessionStart(calc_s);
    ImpressSessionStart(impress_s);
    
    // 极速状态切换
    for (int i = 0; i < iterations; i++) {
        CalcSessionPause(calc_s);
        ImpressSessionPause(calc_s);
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        
        CalcSessionResume(calc_s);
        ImpressSessionResume(calc_s);
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        
        CalcSessionStop(calc_s);
        ImpressSessionStop(impress_s);
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        
        CalcSessionStart(calc_s);
        ImpressSessionStart(impress_s);
        std::this_thread::sleep_for(std::chrono::microseconds(10));
        
        if (i % 1000 == 0) {
            printf("State switching iteration %d\n", i);
            fflush(stdout);
        }
    }
    
    CalcSessionStop(calc_s);
    ImpressSessionStop(impress_s);
    CalcSessionDestroy(calc_s);
    ImpressSessionDestroy(impress_s);
    
    printf("Rapid state switching completed\n");
    fflush(stdout);
}

// 攻击3: 跨状态操作攻击（在不该操作的状态下操作）
void attack_cross_state_operations(const char* xlsx, const char* pptx) {
    printf("Starting cross-state operations attack\n");
    fflush(stdout);
    
    void* calc_s = CalcSessionCreate(xlsx, "", "attack_state_corruption_probe", OnFrame, nullptr, 1920, 1080);
    void* impress_s = ImpressSessionCreate(pptx, "", "attack_state_corruption_probe", OnFrame, nullptr, 1920, 1080);
    
    if (!calc_s || !impress_s) {
        printf("Setup failed\n");
        return;
    }
    
    // 在Stop状态下执行页面操作
    printf("Testing page operations in stopped state...\n");
    CalcSessionNextPage(calc_s);
    CalcSessionPreviousPage(calc_s);
    ImpressSessionNextPage(impress_s);
    ImpressSessionPreviousPage(impress_s);
    
    // 现在Start
    CalcSessionStart(calc_s);
    ImpressSessionStart(impress_s);
    
    // 在Pause状态下执行页面操作
    printf("Testing page operations in paused state...\n");
    CalcSessionPause(calc_s);
    ImpressSessionPause(impress_s);
    
    for (int i = 0; i < 100; i++) {
        CalcSessionNextPage(calc_s);
        ImpressSessionNextPage(impress_s);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    // 在暂停状态下调用UpdateFrame
    printf("Testing UpdateFrame in paused state...\n");
    for (int i = 0; i < 100; i++) {
        CalcSessionUpdateFrame(calc_s);
        ImpressSessionUpdateFrame(impress_s);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    CalcSessionStop(calc_s);
    ImpressSessionStop(impress_s);
    CalcSessionDestroy(calc_s);
    ImpressSessionDestroy(impress_s);
    
    printf("Cross-state operations attack completed\n");
    fflush(stdout);
}

// 攻击4: 并发状态操作攻击（多线程同时操作状态）
void attack_concurrent_state_operations(const char* xlsx, const char* pptx, int thread_count) {
    printf("Starting concurrent state operations attack (%d threads)\n", thread_count);
    fflush(stdout);
    
    void* calc_s = CalcSessionCreate(xlsx, "", "attack_state_corruption_probe", OnFrame, nullptr, 1920, 1080);
    void* impress_s = ImpressSessionCreate(pptx, "", "attack_state_corruption_probe", OnFrame, nullptr, 1920, 1080);
    
    if (!calc_s || !impress_s) {
        printf("Setup failed\n");
        return;
    }
    
    CalcSessionStart(calc_s);
    ImpressSessionStart(impress_s);
    
    std::vector<std::thread> threads;
    
    // 多线程同时操作状态
    for (int i = 0; i < thread_count; i++) {
        threads.emplace_back([calc_s, impress_s, i]() {
            for (int j = 0; j < 1000; j++) {
                switch (i % 4) {
                    case 0: 
                        CalcSessionPause(calc_s);
                        ImpressSessionPause(impress_s);
                        break;
                    case 1:
                        CalcSessionResume(calc_s);
                        ImpressSessionResume(impress_s);
                        break;
                    case 2:
                        CalcSessionStop(calc_s);
                        ImpressSessionStop(impress_s);
                        CalcSessionStart(calc_s);
                        ImpressSessionStart(impress_s);
                        break;
                    case 3:
                        CalcSessionUpdateFrame(calc_s);
                        ImpressSessionUpdateFrame(impress_s);
                        break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
    }
    
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    
    CalcSessionStop(calc_s);
    ImpressSessionStop(impress_s);
    CalcSessionDestroy(calc_s);
    ImpressSessionDestroy(impress_s);
    
    printf("Concurrent state operations attack completed\n");
    fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <xlsx> <pptx> [attack_type=1|2|3|4|all] [param=1000]\n", argv[0]);
        fprintf(stderr, "  attack_type: 1=sequence corruption, 2=rapid switching, 3=cross-state, 4=concurrent, all=all\n");
        fprintf(stderr, "  param: iterations for type2, thread count for type4\n");
        return 2;
    }
    
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int attack_type = (argc >= 4) ? 
        (strcmp(argv[3], "all") == 0 ? 4 : atoi(argv[3])) : 1;
    int param = (argc >= 5) ? atoi(argv[4]) : 1000;
    
    printf("=== STATE CORRUPTION ATTACK PROBE ===\n");
    printf("xlsx=%s pptx=%s attack_type=%d param=%d\n", xlsx, pptx, attack_type, param);
    fflush(stdout);
    
    int score = 0;
    
    // 攻击1: 状态机顺序破坏
    if (attack_type == 1 || attack_type == 4) {
        printf("\n--- Attack 1: State Sequence Corruption ---\n");
        fflush(stdout);
        
        try {
            attack_state_sequence_corruption(xlsx, pptx);
            printf(">>> Attack 1: No crash (0 point)\n");
        } catch (...) {
            printf(">>> ATTACK 1 SUCCESS: Exception/crash detected (1 point)\n");
            score++;
        }
        fflush(stdout);
    }
    
    // 攻击2: 极速状态切换
    if (attack_type == 2 || attack_type == 4) {
        printf("\n--- Attack 2: Rapid State Switching ---\n");
        fflush(stdout);
        
        try {
            attack_rapid_state_switching(xlsx, pptx, param);
            printf(">>> Attack 2: No crash (0 point)\n");
        } catch (...) {
            printf(">>> ATTACK 2 SUCCESS: Exception/crash detected (1 point)\n");
            score++;
        }
        fflush(stdout);
    }
    
    // 攻击3: 跨状态操作
    if (attack_type == 3 || attack_type == 4) {
        printf("\n--- Attack 3: Cross-State Operations ---\n");
        fflush(stdout);
        
        try {
            attack_cross_state_operations(xlsx, pptx);
            printf(">>> Attack 3: No crash (0 point)\n");
        } catch (...) {
            printf(">>> ATTACK 3 SUCCESS: Exception/crash detected (1 point)\n");
            score++;
        }
        fflush(stdout);
    }
    
    // 攻击4: 并发状态操作
    if (attack_type == 4) {
        printf("\n--- Attack 4: Concurrent State Operations ---\n");
        fflush(stdout);
        
        try {
            attack_concurrent_state_operations(xlsx, pptx, 16);
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
