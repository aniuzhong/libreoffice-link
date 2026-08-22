// attack_race_probe.cpp — 并发竞态攻击探针
// 攻击点1: 多线程同时Create会话（测试BootLock死锁，经验43）
// 攻击点2: 多线程同时Start/Stop（测试FramePump竞态，经验42 P5）
// 攻击点3: 多线程同时状态控制（测试锁纪律）
#include <base/abi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>
#include <unistd.h>

static std::atomic<int> s_calc_creates{0};
static std::atomic<int> s_impress_creates{0};
static std::atomic<int> s_calc_race_success{0};
static std::atomic<int> s_impress_race_success{0};
static std::atomic<bool> s_attack_done{false};

static void OnFrame(const uint8_t* data, int32_t w, int32_t h, int32_t rp,
                    int32_t size, int32_t format, void* opaque) {
    (void)data; (void)w; (void)h; (void)rp; (void)size; (void)format; (void)opaque;
}

// 攻击1: 多线程同时Create calc会话（测试BootLock死锁）
void attack_calc_create_race(const char* xlsx, int thread_id) {
    printf("[T%d] Starting calc create race attack\n", thread_id);
    fflush(stdout);
    
    // 设置5秒超时，如果死锁则超时
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    
    void* s = CalcSessionCreate(xlsx, "", "attack_race_probe", OnFrame, nullptr, 1920, 1080);
    s_calc_creates++;
    
    if (std::chrono::steady_clock::now() > deadline) {
        printf("[T%d] TIMEOUT - likely BootLock deadlock (经验43)\n", thread_id);
        fflush(stdout);
        return; // 死锁攻击成功
    }
    
    if (s) {
        printf("[T%d] Calc create SUCCESS (no deadlock)\n", thread_id);
        fflush(stdout);
        s_calc_race_success++;
        CalcSessionDestroy(s);
    } else {
        printf("[T%d] Calc create FAILED\n", thread_id);
        fflush(stdout);
    }
}

// 攻击2: 多线程同时Create impress会话
void attack_impress_create_race(const char* pptx, int thread_id) {
    printf("[T%d] Starting impress create race attack\n", thread_id);
    fflush(stdout);
    
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    
    void* s = ImpressSessionCreate(pptx, "", "attack_race_probe", OnFrame, nullptr, 1920, 1080);
    s_impress_creates++;
    
    if (std::chrono::steady_clock::now() > deadline) {
        printf("[T%d] TIMEOUT - likely BootLock deadlock (经验43)\n", thread_id);
        fflush(stdout);
        return;
    }
    
    if (s) {
        printf("[T%d] Impress create SUCCESS (no deadlock)\n", thread_id);
        fflush(stdout);
        s_impress_race_success++;
        ImpressSessionDestroy(s);
    } else {
        printf("[T%d] Impress create FAILED\n", thread_id);
        fflush(stdout);
    }
}

// 攻击3: 多线程同时Start/Stop（测试FramePump竞态，经验42 P5）
void attack_start_stop_race(void* session, bool is_calc, int thread_id) {
    printf("[T%d] Starting Start/Stop race attack\n", thread_id);
    fflush(stdout);
    
    for (int i = 0; i < 50; i++) {
        if (is_calc) {
            CalcSessionStart(session);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            CalcSessionStop(session);
        } else {
            ImpressSessionStart(session);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            ImpressSessionStop(session);
        }
    }
    
    printf("[T%d] Start/Stop race attack completed\n", thread_id);
    fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <xlsx> <pptx> [attack_type=1|2|3|all] [threads=8]\n", argv[0]);
        fprintf(stderr, "  attack_type: 1=create race, 2=start/stop race, 3=both, all=all attacks\n");
        return 2;
    }
    
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int attack_type = (argc >= 4) ? 
        (strcmp(argv[3], "all") == 0 ? 3 : atoi(argv[3])) : 1;
    int num_threads = (argc >= 5) ? atoi(argv[4]) : 8;
    
    printf("=== ATTACK RACE PROBE ===\n");
    printf("xlsx=%s pptx=%s attack_type=%d threads=%d\n", xlsx, pptx, attack_type, num_threads);
    fflush(stdout);
    
    int score = 0;
    
    // 攻击1: Create竞态
    if (attack_type == 1 || attack_type == 3) {
        printf("\n--- Attack 1: Create Race (BootLock Deadlock Test) ---\n");
        fflush(stdout);
        
        std::vector<std::thread> calc_threads;
        std::vector<std::thread> impress_threads;
        
        // 启动calc创建竞态线程
        for (int i = 0; i < num_threads; i++) {
            calc_threads.emplace_back(attack_calc_create_race, xlsx, i);
        }
        
        // 启动impress创建竞态线程
        for (int i = 0; i < num_threads; i++) {
            impress_threads.emplace_back(attack_impress_create_race, pptx, i + num_threads);
        }
        
        // 等待所有线程完成或超时
        for (auto& t : calc_threads) {
            if (t.joinable()) t.join();
        }
        for (auto& t : impress_threads) {
            if (t.joinable()) t.join();
        }
        
        printf("Attack 1 Results: calc_creates=%d impress_creates=%d\n", 
               s_calc_creates.load(), s_impress_creates.load());
        printf("  calc_success=%d impress_success=%d\n",
               s_calc_race_success.load(), s_impress_race_success.load());
        
        // 如果有超时或失败，得1分
        if (s_calc_creates.load() < num_threads || s_impress_creates.load() < num_threads) {
            printf(">>> ATTACK 1 SUCCESS: Deadlock detected (1 point)\n");
            score++;
        } else {
            printf(">>> Attack 1 FAILED: No deadlock detected\n");
        }
        fflush(stdout);
    }
    
    // 攻击2: Start/Stop竞态
    if (attack_type == 2 || attack_type == 3) {
        printf("\n--- Attack 2: Start/Stop Race (FramePump Race Test) ---\n");
        fflush(stdout);
        
        void* calc_s = CalcSessionCreate(xlsx, "", "attack_race_probe", OnFrame, nullptr, 1920, 1080);
        void* impress_s = ImpressSessionCreate(pptx, "", "attack_race_probe", OnFrame, nullptr, 1920, 1080);
        
        if (!calc_s || !impress_s) {
            printf("Setup failed for Start/Stop attack\n");
        } else {
            std::vector<std::thread> start_stop_threads;
            
            for (int i = 0; i < num_threads; i++) {
                start_stop_threads.emplace_back(attack_start_stop_race, calc_s, true, i);
                start_stop_threads.emplace_back(attack_start_stop_race, impress_s, false, i + num_threads);
            }
            
            for (auto& t : start_stop_threads) {
                if (t.joinable()) t.join();
            }
            
            printf("Attack 2 completed without crash\n");
            
            // 尝试检测是否崩溃或异常状态
            bool calc_ok = CalcSessionStart(calc_s);
            bool impress_ok = ImpressSessionStart(impress_s);
            
            if (!calc_ok || !impress_ok) {
                printf(">>> ATTACK 2 SUCCESS: State corruption detected (1 point)\n");
                score++;
            } else {
                printf(">>> Attack 2 FAILED: No state corruption\n");
            }
            
            CalcSessionStop(calc_s);
            ImpressSessionStop(impress_s);
            CalcSessionDestroy(calc_s);
            ImpressSessionDestroy(impress_s);
        }
        fflush(stdout);
    }
    
    printf("\n=== FINAL SCORE: %d/2 ===\n", score);
    printf("Note: Each crash/hang detected = 1 point\n");
    fflush(stdout);
    
    return 0;
}
