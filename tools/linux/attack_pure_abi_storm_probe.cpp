// attack_pure_abi_storm_probe.cpp — 纯 C ABI 接口暴力攻击
// 不 fork、不删文件、不改信号量，只调 C ABI 接口
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

// 攻击1: 8 路并发，每路 Create→Start→高频混合操作→Destroy，极短存活
void attack_8way_mixed_storm(const char* xlsx, const char* pptx) {
    printf("[ABI-1] 8-way mixed storm\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 8; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                bool useCalc = (r % 2 == 0);
                const char* path = useCalc ? xlsx : pptx;
                void* s = useCalc
                    ? CalcSessionCreate(path, "", "attack_abi", OnFrame, nullptr, 1920, 1080)
                    : ImpressSessionCreate(path, "", "attack_abi", OnFrame, nullptr, 1920, 1080);
                if (!s) continue;
                if (useCalc) CalcSessionStart(s); else ImpressSessionStart(s);

                // 极短存活期内密集操作
                for (int i = 0; i < 5; i++) {
                    if (useCalc) {
                        CalcSessionSetResolution(s, 640 + (i*300), 480 + (i*200));
                        CalcSessionNextPage(s);
                        CalcSessionUpdateFrame(s);
                        CalcSessionSetScale(s, 100 + (i*50));
                    } else {
                        ImpressSessionSetResolution(s, 640 + (i*300), 480 + (i*200));
                        ImpressSessionNextPage(s);
                        ImpressSessionUpdateFrame(s);
                        ImpressSessionSetMute(s, i & 1);
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }

                if (useCalc) CalcSessionDestroy(s); else ImpressSessionDestroy(s);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(12));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[ABI-1] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// 攻击2: 单 session 10 路并发操作（翻页+resize+scale+UpdateFrame+SetMute）
void attack_10way_single_session(const char* pptx) {
    printf("[ABI-2] 10-way single session storm\n"); fflush(stdout);
    void* p = ImpressSessionCreate(pptx, "", "attack_abi", OnFrame, nullptr, 1920, 1080);
    if (!p) { printf("create failed\n"); return; }
    ImpressSessionStart(p);

    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < 10; t++) {
        ts.emplace_back([&, t]() {
            while (!stop.load()) {
                switch (t % 5) {
                    case 0: ImpressSessionNextPage(p); break;
                    case 1: ImpressSessionPreviousPage(p); break;
                    case 2: ImpressSessionGoToPage(p, t % 5); break;
                    case 3: ImpressSessionSetResolution(p, 640 + (t*200), 480 + (t*150)); break;
                    case 4: ImpressSessionSetMute(p, t & 1); break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& x : ts) x.join();
    ImpressSessionDestroy(p);
    printf("[ABI-2] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// 攻击3: 极速 Create→Destroy 循环（无操作，纯生命周期压力）
void attack_rapid_create_destroy_loop(const char* xlsx, const char* pptx) {
    printf("[ABI-3] Rapid create-destroy loop\n"); fflush(stdout);
    for (int i = 0; i < 1000; i++) {
        void* c = CalcSessionCreate(xlsx, "", "attack_abi", OnFrame, nullptr, 1920, 1080);
        if (c) CalcSessionDestroy(c);
        void* p = ImpressSessionCreate(pptx, "", "attack_abi", OnFrame, nullptr, 1920, 1080);
        if (p) ImpressSessionDestroy(p);
        if (i % 100 == 0) { printf("[ABI-3] %d/1000\n", i); fflush(stdout); }
    }
    printf("[ABI-3] done\n"); fflush(stdout);
}

// 攻击4: 8 路并发，每路 Create→立即 Destroy（零存活期）
void attack_zero_lifetime_storm(const char* xlsx, const char* pptx) {
    printf("[ABI-4] Zero-lifetime storm\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 8; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* s = (r % 2 == 0)
                    ? CalcSessionCreate(xlsx, "", "attack_abi", OnFrame, nullptr, 1920, 1080)
                    : ImpressSessionCreate(pptx, "", "attack_abi", OnFrame, nullptr, 1920, 1080);
                if (!s) continue;
                // 立即销毁，不给任何操作机会
                if (r % 2 == 0) CalcSessionDestroy(s); else ImpressSessionDestroy(s);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(10));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[ABI-4] done\n"); fflush(stdout);
}

// 攻击5: 先 Create 8 个 session 占满 slot，再并发 Create（slot 耗尽压力）
void attack_slot_exhaust_storm(const char* xlsx, const char* pptx) {
    printf("[ABI-5] Slot exhaust storm\n"); fflush(stdout);
    std::vector<void*> holders;
    for (int i = 0; i < 8; i++) {
        void* s = (i % 2 == 0)
            ? CalcSessionCreate(xlsx, "", "attack_abi", OnFrame, nullptr, 1920, 1080)
            : ImpressSessionCreate(pptx, "", "attack_abi", OnFrame, nullptr, 1920, 1080);
        if (s) {
            if (i % 2 == 0) CalcSessionStart(s); else ImpressSessionStart(s);
            holders.push_back(s);
        }
    }
    printf("[ABI-5] holding %zu slots\n", holders.size()); fflush(stdout);

    // 并发 Create（slot 耗尽）
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 8; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* s = (r % 2 == 0)
                    ? CalcSessionCreate(xlsx, "", "attack_abi", OnFrame, nullptr, 1920, 1080)
                    : ImpressSessionCreate(pptx, "", "attack_abi", OnFrame, nullptr, 1920, 1080);
                if (s) {
                    if (r % 2 == 0) CalcSessionDestroy(s); else ImpressSessionDestroy(s);
                }
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& t : ts) t.join();
    for (auto s : holders) {
        if (holders.size() % 2 == 0) CalcSessionDestroy(s); else ImpressSessionDestroy(s);
    }
    printf("[ABI-5] done\n"); fflush(stdout);
}

// 攻击6: 交替极速 resize（calc 和 impress 交替，模拟真实压力）
void attack_alternating_resize_storm(const char* xlsx, const char* pptx) {
    printf("[ABI-6] Alternating resize storm\n"); fflush(stdout);
    void* c = CalcSessionCreate(xlsx, "", "attack_abi", OnFrame, nullptr, 1920, 1080);
    void* p = ImpressSessionCreate(pptx, "", "attack_abi", OnFrame, nullptr, 1920, 1080);
    if (!c || !p) { printf("create failed\n"); return; }
    CalcSessionStart(c);
    ImpressSessionStart(p);

    for (int i = 0; i < 10000; i++) {
        int w = 640 + ((i * 137) % 3000);
        int h = 480 + ((i * 173) % 2000);
        CalcSessionSetResolution(c, w, h);
        ImpressSessionSetResolution(p, w, h);
        if (i % 100 == 0) {
            CalcSessionUpdateFrame(c);
            ImpressSessionUpdateFrame(p);
        }
        if (i % 1000 == 0) { printf("[ABI-6] %d/10000\n", i); fflush(stdout); }
    }
    CalcSessionDestroy(c);
    ImpressSessionDestroy(p);
    printf("[ABI-6] done\n"); fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <xlsx> <pptx> [att=1-6|all]\n", argv[0]);
        return 2;
    }
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int att = (argc >= 4 && strcmp(argv[3], "all") == 0) ? 0 : atoi(argv[3]);
    int score = 0;

    printf("=== PURE ABI STORM ATTACK ===\n"); fflush(stdout);
    if (att == 0 || att == 1) { try { attack_8way_mixed_storm(xlsx, pptx); } catch (...) { printf(">>> ABI-1 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 2) { try { attack_10way_single_session(pptx); } catch (...) { printf(">>> ABI-2 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 3) { try { attack_rapid_create_destroy_loop(xlsx, pptx); } catch (...) { printf(">>> ABI-3 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 4) { try { attack_zero_lifetime_storm(xlsx, pptx); } catch (...) { printf(">>> ABI-4 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 5) { try { attack_slot_exhaust_storm(xlsx, pptx); } catch (...) { printf(">>> ABI-5 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 6) { try { attack_alternating_resize_storm(xlsx, pptx); } catch (...) { printf(">>> ABI-6 CRASH (1pt)\n"); score++; } }
    printf("=== ABI FINAL SCORE: %d/6 ===\n", score); fflush(stdout);
    return 0;
}
