// attack_resource_probe.cpp — 资源管理攻击探针（简化版）
// 攻击点1: 快速创建销毁会话（测试资源泄漏，经验35 Xvfb垂死窗口竞态）
// 攻击点2: 内存泄漏攻击（大量会话不销毁）
#include <base/abi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <unistd.h>

static int s_calc_sessions = 0;
static int s_impress_sessions = 0;
static int s_failed_creates = 0;

static void OnFrame(const uint8_t* data, int32_t w, int32_t h, int32_t rp,
                    int32_t size, int32_t format, void* opaque) {
    (void)data; (void)w; (void)h; (void)rp; (void)size; (void)format; (void)opaque;
}

// 攻击1: 快速创建销毁循环（测试Xvfb垂死窗口竞态，经验35）
void attack_rapid_create_destroy(const char* xlsx, const char* pptx, int iterations) {
    printf("Starting rapid create/destroy attack (%d iterations)\n", iterations);
    fflush(stdout);
    
    for (int i = 0; i < iterations; i++) {
        // 快速创建calc会话
        void* calc_s = CalcSessionCreate(xlsx, "", "attack_resource_probe", OnFrame, nullptr, 1920, 1080);
        if (calc_s) {
            s_calc_sessions++;
            CalcSessionStart(calc_s);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            CalcSessionStop(calc_s);
            CalcSessionDestroy(calc_s);
        } else {
            s_failed_creates++;
            printf("Iteration %d: Calc create FAILED\n", i);
        }
        
        // 快速创建impress会话
        void* impress_s = ImpressSessionCreate(pptx, "", "attack_resource_probe", OnFrame, nullptr, 1920, 1080);
        if (impress_s) {
            s_impress_sessions++;
            ImpressSessionStart(impress_s);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ImpressSessionStop(impress_s);
            ImpressSessionDestroy(impress_s);
        } else {
            s_failed_creates++;
            printf("Iteration %d: Impress create FAILED\n", i);
        }
        
        // 极短间隔，增加竞态概率
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        
        if (i % 10 == 0) {
            printf("Progress: %d/%d (calc_sessions=%d impress_sessions=%d failed_creates=%d)\n",
                   i, iterations, s_calc_sessions, s_impress_sessions, s_failed_creates);
            fflush(stdout);
        }
    }
    
    printf("Rapid create/destroy attack completed\n");
    fflush(stdout);
}

// 攻击2: 内存泄漏攻击（大量会话同时存在）
void attack_memory_leak(const char* xlsx, const char* pptx, int session_count) {
    printf("Starting memory leak attack (%d concurrent sessions)\n", session_count);
    fflush(stdout);
    
    std::vector<void*> calc_sessions;
    std::vector<void*> impress_sessions;
    
    for (int i = 0; i < session_count; i++) {
        void* calc_s = CalcSessionCreate(xlsx, "", "attack_resource_probe", OnFrame, nullptr, 1920, 1080);
        if (calc_s) {
            CalcSessionStart(calc_s);
            calc_sessions.push_back(calc_s);
            s_calc_sessions++;
        } else {
            s_failed_creates++;
            printf("Session %d: Calc create FAILED (resource exhaustion?)\n", i);
        }
        
        void* impress_s = ImpressSessionCreate(pptx, "", "attack_resource_probe", OnFrame, nullptr, 1920, 1080);
        if (impress_s) {
            ImpressSessionStart(impress_s);
            impress_sessions.push_back(impress_s);
            s_impress_sessions++;
        } else {
            s_failed_creates++;
            printf("Session %d: Impress create FAILED (resource exhaustion?)\n", i);
        }
        
        if (i % 5 == 0) {
            printf("Progress: %d/%d (calc=%zu impress=%zu)\n", i, session_count, 
                   calc_sessions.size(), impress_sessions.size());
            fflush(stdout);
        }
    }
    
    printf("Memory leak attack: holding %zu calc + %zu impress sessions\n",
           calc_sessions.size(), impress_sessions.size());
    printf("Sleeping 10 seconds to observe memory usage...\n");
    fflush(stdout);
    
    sleep(10);
    
    printf("Cleaning up sessions...\n");
    for (auto s : calc_sessions) {
        CalcSessionStop(s);
        CalcSessionDestroy(s);
    }
    for (auto s : impress_sessions) {
        ImpressSessionStop(s);
        ImpressSessionDestroy(s);
    }
    
    printf("Memory leak attack completed\n");
    fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <xlsx> <pptx> [attack_type=1|2|all] [param=50]\n", argv[0]);
        fprintf(stderr, "  attack_type: 1=rapid create/destroy, 2=memory leak, all=all\n");
        fprintf(stderr, "  param: iterations for type1, session_count for type2\n");
        return 2;
    }
    
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int attack_type = (argc >= 4) ? 
        (strcmp(argv[3], "all") == 0 ? 2 : atoi(argv[3])) : 1;
    int param = (argc >= 5) ? atoi(argv[4]) : 50;
    
    printf("=== ATTACK RESOURCE PROBE ===\n");
    printf("xlsx=%s pptx=%s attack_type=%d param=%d\n", xlsx, pptx, attack_type, param);
    fflush(stdout);
    
    int score = 0;
    
    // 攻击1: 快速创建销毁
    if (attack_type == 1 || attack_type == 2) {
        printf("\n--- Attack 1: Rapid Create/Destroy (Xvfb Race Test) ---\n");
        fflush(stdout);
        
        int before_failed = s_failed_creates;
        attack_rapid_create_destroy(xlsx, pptx, param);
        int after_failed = s_failed_creates;
        
        if (after_failed > before_failed) {
            printf(">>> ATTACK 1 SUCCESS: Resource race detected (%d failures) (1 point)\n", 
                   after_failed - before_failed);
            score++;
        } else {
            printf(">>> Attack 1 FAILED: No resource race detected\n");
        }
        fflush(stdout);
    }
    
    // 攻击2: 内存泄漏
    if (attack_type == 2) {
        printf("\n--- Attack 2: Memory Leak (Resource Exhaustion Test) ---\n");
        fflush(stdout);
        
        int before_failed = s_failed_creates;
        attack_memory_leak(xlsx, pptx, param);
        int after_failed = s_failed_creates;
        
        if (after_failed > before_failed) {
            printf(">>> ATTACK 2 SUCCESS: Resource exhaustion detected (%d failures) (1 point)\n",
                   after_failed - before_failed);
            score++;
        } else {
            printf(">>> Attack 2 FAILED: No resource exhaustion detected\n");
        }
        fflush(stdout);
    }
    
    printf("\n=== FINAL SCORE: %d/2 ===\n", score);
    printf("Note: Each resource race/exhaustion detected = 1 point\n");
    fflush(stdout);
    
    return 0;
}
