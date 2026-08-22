// attack_mute_teardown_probe.cpp — 静音跨进程 + 内核拆解后调用攻击探针
// 已覆盖：静音链路（ffplay_inject/media_green）。未覆盖：在会话对象被部分拆解
// （Destroy 进入中、或共享内核被其他会话 owner 退出连坐）后继续调 SetMute/翻页；
// 以及大量会话并发创建触发共享内核 owner 退出连坐（HANDOFF 1.4 并行会话协作约定 (所有者退出连坐)）。
// 目标：崩溃/卡死 = 1 分。
#include <abi/abi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <unistd.h>

static std::atomic<long long> s_frames{0};
static void OnFrame(const uint8_t* d, int32_t w, int32_t h, int32_t rp,
                    int32_t s, int32_t f, void* o) {
    s_frames.fetch_add(1, std::memory_order_relaxed);
    (void)d;(void)w;(void)h;(void)rp;(void)s;(void)f;(void)o;
}

// 攻击1: 销毁进行中并发调 SetMute（UNO 远程触发子进程 ffplay.so，ctx_ 已被 clear）
void attack_mute_during_destroy(const char* pptx) {
    printf("[MUTE-1] SetMute during Destroy\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 4; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* p = ImpressSessionCreate(pptx, "", "attack_mute", OnFrame, nullptr, 1920, 1080);
                if (!p) continue;
                ImpressSessionStart(p);
                ImpressSessionSetMute(p, r % 2); // UNO 远程跨进程
                // 不等 settle，立即销毁，与 SetMute 竞态
                std::thread dt([p]() { ImpressSessionDestroy(p); });
                ImpressSessionSetMute(p, 1 - (r % 2));
                ImpressSessionNextPage(p);
                dt.join();
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(6));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[MUTE-1] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// 攻击2: owner 退出连坐 —— 大量短命会话，owner 进程退出会断开其他会话。
// 若共享内核 owner 退出后仍有会话引用 ctx_（已被 clear）则崩溃。
void attack_owner_exit_cascade(const char* xlsx, const char* pptx) {
    printf("[MUTE-2] owner-exit cascade (many short-lived sessions)\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 6; r++) {
        ts.emplace_back([&, r]() {
            bool useCalc = (r % 2 == 0);
            const char* path = useCalc ? xlsx : pptx;
            while (!stop.load()) {
                void* s = useCalc
                    ? CalcSessionCreate(path, "", "attack_mute", OnFrame, nullptr, 1920, 1080)
                    : ImpressSessionCreate(path, "", "attack_mute", OnFrame, nullptr, 1920, 1080);
                if (!s) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue; }
                if (useCalc) CalcSessionStart(s); else ImpressSessionStart(s);
                std::this_thread::sleep_for(std::chrono::milliseconds(1 + (r % 3)));
                if (useCalc) CalcSessionDestroy(s); else ImpressSessionDestroy(s);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(10));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[MUTE-2] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// 攻击3: 同一 session 反复 SetMute 翻转 + 跨进程，磁场抖动
void attack_mute_thrash(const char* pptx) {
    printf("[MUTE-3] mute thrash on one session\n"); fflush(stdout);
    void* p = ImpressSessionCreate(pptx, "", "attack_mute", OnFrame, nullptr, 1920, 1080);
    if (!p) { printf("[MUTE-3] create failed\n"); return; }
    ImpressSessionStart(p);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; i++)
        ts.emplace_back([&]() {
            int v = 0;
            while (!stop.load()) { ImpressSessionSetMute(p, v++ & 1); }
        });
    std::this_thread::sleep_for(std::chrono::seconds(5));
    stop.store(true);
    for (auto& t : ts) t.join();
    ImpressSessionDestroy(p);
    printf("[MUTE-3] done\n"); fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { fprintf(stderr, "usage: %s <xlsx> <pptx> [att=1|2|3|all]\n", argv[0]); return 2; }
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int att = (argc >= 4 && strcmp(argv[3], "all") == 0) ? 0 : atoi(argv[3]);
    int score = 0;
    printf("=== MUTE / TEARDOWN ATTACK ===\n"); fflush(stdout);
    if (att == 0 || att == 1) { try { attack_mute_during_destroy(pptx); } catch (...) { printf(">>> MUTE-1 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 2) { try { attack_owner_exit_cascade(xlsx, pptx); } catch (...) { printf(">>> MUTE-2 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 3) { try { attack_mute_thrash(pptx); } catch (...) { printf(">>> MUTE-3 CRASH (1pt)\n"); score++; } }
    printf("=== MUTE FINAL SCORE: %d/3 ===\n", score); fflush(stdout);
    return 0;
}
