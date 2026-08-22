// attack_bootlock_storm_probe.cpp — BootLock 60s 超时级联攻击
// 目标: 大量并发 Create 触发 BootLock 60s 超时 → 强制释放 → 不一致状态 → 崩溃
// 已有 attack_race_probe 只用了 8 线程, 这里用 32+ 线程 + 交错 calc/impress/writer
// 让 BootLock 信号量反复超时, 强制释放后遗留不一致 ctx → 后续操作崩溃
#include <base/abi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

static std::atomic<long long> s_frames{0};
static void OnFrame(const uint8_t* d, int32_t w, int32_t h, int32_t rp,
                    int32_t s, int32_t f, void* o) {
    s_frames.fetch_add(1, std::memory_order_relaxed);
    (void)d;(void)w;(void)h;(void)rp;(void)s;(void)f;(void)o;
}
static void OnWriterFrame(const uint8_t* d, int32_t w, int32_t h, int32_t rp,
                          int32_t s, int32_t f, void* o) {
    s_frames.fetch_add(1, std::memory_order_relaxed);
    (void)d;(void)w;(void)h;(void)rp;(void)s;(void)f;(void)o;
}

// 攻击1: 32 线程并发 Create (calc+impress 混合), 让 BootLock 信号量反复超时
void attack_bootlock_massive_concurrent(const char* xlsx, const char* pptx) {
    printf("[BOOT-1] 32-thread concurrent create (BootLock storm)\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int i = 0; i < 32; i++) {
        ts.emplace_back([&, i]() {
            while (!stop.load()) {
                void* s = nullptr;
                switch (i % 2) {
                    case 0: s = CalcSessionCreate(xlsx, "", "attack_boot", OnFrame, nullptr, 1920, 1080); break;
                    case 1: s = ImpressSessionCreate(pptx, "", "attack_boot", OnFrame, nullptr, 1920, 1080); break;
                }
                if (!s) continue;
                if (i % 2 == 0) { CalcSessionStart(s); CalcSessionDestroy(s); }
                else { ImpressSessionStart(s); ImpressSessionDestroy(s); }
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(15));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[BOOT-1] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// 攻击2: 先耗尽 slot (8 个), 再并发 Create 触发 slot 耗尽 + BootLock 双重压力
void attack_bootlock_slot_exhaustion(const char* xlsx, const char* pptx) {
    printf("[BOOT-2] slot exhaustion + BootLock storm\n"); fflush(stdout);
    std::vector<void*> sessions;
    // 先占满 8 个 slot
    for (int i = 0; i < 8; i++) {
        void* s = CalcSessionCreate(xlsx, "", "attack_boot", OnFrame, nullptr, 1920, 1080);
        if (s) { CalcSessionStart(s); sessions.push_back(s); }
    }
    printf("[BOOT-2] occupied %zu slots\n", sessions.size()); fflush(stdout);
    // 再并发 Create (slot 耗尽 → 快速失败, 但 BootLock 仍串行)
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int i = 0; i < 16; i++) {
        ts.emplace_back([&]() {
            while (!stop.load()) {
                void* s = CalcSessionCreate(xlsx, "", "attack_boot", OnFrame, nullptr, 1920, 1080);
                if (s) { CalcSessionDestroy(s); }
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& t : ts) t.join();
    for (auto s : sessions) CalcSessionDestroy(s);
    printf("[BOOT-2] done\n"); fflush(stdout);
}

// 攻击3: 跨进程 BootLock 干扰 — 快速 fork 子进程同时 Create, 让信号量状态混乱
void attack_bootlock_fork_bomb(const char* xlsx) {
    printf("[BOOT-3] fork + BootLock race\n"); fflush(stdout);
    // 主进程先创建一个会话, 持有内核
    void* main_s = CalcSessionCreate(xlsx, "", "attack_boot", OnFrame, nullptr, 1920, 1080);
    if (!main_s) { printf("[BOOT-3] main create failed\n"); return; }
    CalcSessionStart(main_s);
    // 子进程同时 Create (共享内核, 但 BootLock 跨进程)
    for (int i = 0; i < 10; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            // 子进程
            void* s = CalcSessionCreate(xlsx, "", "attack_boot", OnFrame, nullptr, 1920, 1080);
            if (s) CalcSessionDestroy(s);
            _exit(0);
        }
    }
    // 等待子进程
    for (int i = 0; i < 10; i++) wait(nullptr);
    CalcSessionDestroy(main_s);
    printf("[BOOT-3] done\n"); fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { fprintf(stderr, "usage: %s <xlsx> <pptx> [att=1|2|3|all]\n", argv[0]); return 2; }
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int att = (argc >= 4 && strcmp(argv[3], "all") == 0) ? 0 : atoi(argv[3]);
    int score = 0;
    printf("=== BOOTLOCK STORM ATTACK ===\n"); fflush(stdout);
    if (att == 0 || att == 1) { try { attack_bootlock_massive_concurrent(xlsx, pptx); } catch (...) { printf(">>> BOOT-1 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 2) { try { attack_bootlock_slot_exhaustion(xlsx, pptx); } catch (...) { printf(">>> BOOT-2 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 3) { try { attack_bootlock_fork_bomb(xlsx); } catch (...) { printf(">>> BOOT-3 CRASH (1pt)\n"); score++; } }
    printf("=== BOOTLOCK FINAL SCORE: %d/3 ===\n", score); fflush(stdout);
    return 0;
}
