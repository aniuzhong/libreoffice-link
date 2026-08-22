// attack_combo_storm_probe.cpp — 组合风暴攻击
// 把已知能卡死的攻击组合在一起，看能不能制造崩溃
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

// 攻击1: 先跑 5000 次 resize 风暴，然后立即 Create→Destroy 循环
void attack_resize_then_cd(const char* xlsx, const char* pptx) {
    printf("[COMBO-1] Resize storm then create-destroy\n"); fflush(stdout);
    // 阶段1: resize 风暴（已知卡死）
    void* p = ImpressSessionCreate(pptx, "", "attack_combo", OnFrame, nullptr, 1920, 1080);
    if (!p) { printf("create failed\n"); return; }
    ImpressSessionStart(p);
    for (int i = 0; i < 5000; i++) {
        int w = (i % 2) ? 1280 : 2560;
        int h = (i % 2) ? 720 : 1440;
        ImpressSessionSetResolution(p, w, h);
    }
    ImpressSessionDestroy(p);
    printf("[COMBO-1] resize done, now create-destroy\n"); fflush(stdout);

    // 阶段2: 立即 Create→Destroy 循环
    for (int i = 0; i < 500; i++) {
        void* c = CalcSessionCreate(xlsx, "", "attack_combo", OnFrame, nullptr, 1920, 1080);
        if (c) CalcSessionDestroy(c);
    }
    printf("[COMBO-1] done\n"); fflush(stdout);
}

// 攻击2: 先 Create→Destroy 循环 500 轮，然后立即 resize 风暴
void attack_cd_then_resize(const char* xlsx, const char* pptx) {
    printf("[COMBO-2] Create-destroy then resize storm\n"); fflush(stdout);
    for (int i = 0; i < 500; i++) {
        void* c = CalcSessionCreate(xlsx, "", "attack_combo", OnFrame, nullptr, 1920, 1080);
        if (c) CalcSessionDestroy(c);
    }
    printf("[COMBO-2] cd done, now resize\n"); fflush(stdout);

    void* p = ImpressSessionCreate(pptx, "", "attack_combo", OnFrame, nullptr, 1920, 1080);
    if (!p) { printf("create failed\n"); return; }
    ImpressSessionStart(p);
    for (int i = 0; i < 5000; i++) {
        int w = (i % 2) ? 1280 : 2560;
        int h = (i % 2) ? 720 : 1440;
        ImpressSessionSetResolution(p, w, h);
    }
    ImpressSessionDestroy(p);
    printf("[COMBO-2] done\n"); fflush(stdout);
}

// 攻击3: 8 路并发，每路 Create→Start→密集 resize→Destroy
void attack_8way_resize_storm(const char* xlsx, const char* pptx) {
    printf("[COMBO-3] 8-way concurrent resize storm\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 8; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                bool useCalc = (r % 2 == 0);
                const char* path = useCalc ? xlsx : pptx;
                void* s = useCalc
                    ? CalcSessionCreate(path, "", "attack_combo", OnFrame, nullptr, 1920, 1080)
                    : ImpressSessionCreate(path, "", "attack_combo", OnFrame, nullptr, 1920, 1080);
                if (!s) continue;
                if (useCalc) CalcSessionStart(s); else ImpressSessionStart(s);

                // 密集 resize
                for (int i = 0; i < 100; i++) {
                    int w = (i % 2) ? 640 : 3840;
                    int h = (i % 2) ? 480 : 2160;
                    if (useCalc) CalcSessionSetResolution(s, w, h);
                    else ImpressSessionSetResolution(s, w, h);
                }

                if (useCalc) CalcSessionDestroy(s); else ImpressSessionDestroy(s);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(12));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[COMBO-3] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// 攻击4: 单 session 同时 resize + 翻页 + SetMute（三路并发）
void attack_triple_ops_storm(const char* pptx) {
    printf("[COMBO-4] Triple ops storm\n"); fflush(stdout);
    void* p = ImpressSessionCreate(pptx, "", "attack_combo", OnFrame, nullptr, 1920, 1080);
    if (!p) { printf("create failed\n"); return; }
    ImpressSessionStart(p);

    std::atomic<bool> stop{false};
    std::thread resizer([&]() {
        while (!stop.load()) {
            ImpressSessionSetResolution(p, 640 + (rand() % 3000), 480 + (rand() % 2000));
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    std::thread nav([&]() {
        while (!stop.load()) {
            ImpressSessionNextPage(p);
            ImpressSessionPreviousPage(p);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
    std::thread muter([&]() {
        int v = 0;
        while (!stop.load()) {
            ImpressSessionSetMute(p, v++ & 1);
            std::this_thread::sleep_for(std::chrono::microseconds(80));
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    resizer.join(); nav.join(); muter.join();
    ImpressSessionDestroy(p);
    printf("[COMBO-4] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// 攻击5: 先占满 slot，然后对每个 holder 并发 resize + 翻页
void attack_holders_ops_storm(const char* xlsx, const char* pptx) {
    printf("[COMBO-5] Holders ops storm\n"); fflush(stdout);
    std::vector<void*> holders;
    for (int i = 0; i < 8; i++) {
        void* s = (i % 2 == 0)
            ? CalcSessionCreate(xlsx, "", "attack_combo", OnFrame, nullptr, 1920, 1080)
            : ImpressSessionCreate(pptx, "", "attack_combo", OnFrame, nullptr, 1920, 1080);
        if (s) {
            if (i % 2 == 0) CalcSessionStart(s); else ImpressSessionStart(s);
            holders.push_back(s);
        }
    }
    printf("[COMBO-5] holding %zu slots\n", holders.size()); fflush(stdout);

    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (size_t i = 0; i < holders.size(); i++) {
        ts.emplace_back([&, i]() {
            void* s = holders[i];
            bool isCalc = (i % 2 == 0);
            while (!stop.load()) {
                if (isCalc) {
                    CalcSessionSetResolution(s, 640 + (rand() % 3000), 480 + (rand() % 2000));
                    CalcSessionNextPage(s);
                    CalcSessionUpdateFrame(s);
                } else {
                    ImpressSessionSetResolution(s, 640 + (rand() % 3000), 480 + (rand() % 2000));
                    ImpressSessionNextPage(s);
                    ImpressSessionUpdateFrame(s);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& t : ts) t.join();
    for (auto s : holders) {
        if (holders.size() % 2 == 0) CalcSessionDestroy(s); else ImpressSessionDestroy(s);
    }
    printf("[COMBO-5] done\n"); fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <xlsx> <pptx> [att=1-5|all]\n", argv[0]);
        return 2;
    }
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int att = (argc >= 4 && strcmp(argv[3], "all") == 0) ? 0 : atoi(argv[3]);
    int score = 0;

    printf("=== COMBO STORM ATTACK ===\n"); fflush(stdout);
    if (att == 0 || att == 1) { try { attack_resize_then_cd(xlsx, pptx); } catch (...) { printf(">>> COMBO-1 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 2) { try { attack_cd_then_resize(xlsx, pptx); } catch (...) { printf(">>> COMBO-2 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 3) { try { attack_8way_resize_storm(xlsx, pptx); } catch (...) { printf(">>> COMBO-3 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 4) { try { attack_triple_ops_storm(pptx); } catch (...) { printf(">>> COMBO-4 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 5) { try { attack_holders_ops_storm(xlsx, pptx); } catch (...) { printf(">>> COMBO-5 CRASH (1pt)\n"); score++; } }
    printf("=== COMBO FINAL SCORE: %d/5 ===\n", score); fflush(stdout);
    return 0;
}
