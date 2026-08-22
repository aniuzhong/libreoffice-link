// attack_ultimate_storm_probe.cpp — 终极纯 ABI 风暴
// 只调 C ABI 接口，不 fork、不删文件、不改信号量
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
static void OnFrame(const uint8_t* d, int32_t w, int32_t h, int32_t rp,
                    int32_t s, int32_t f, void* o) {
    s_frames.fetch_add(1, std::memory_order_relaxed);
    (void)d;(void)w;(void)h;(void)rp;(void)s;(void)f;(void)o;
}

// 攻击1: 极速 Create→Destroy 循环（纯生命周期压力）
void attack_rapid_cd(const char* xlsx, const char* pptx) {
    printf("[ULT-1] Rapid create-destroy 2000x\n"); fflush(stdout);
    for (int i = 0; i < 2000; i++) {
        void* c = CalcSessionCreate(xlsx, "", "attack_ult", OnFrame, nullptr, 1920, 1080);
        if (c) CalcSessionDestroy(c);
        void* p = ImpressSessionCreate(pptx, "", "attack_ult", OnFrame, nullptr, 1920, 1080);
        if (p) ImpressSessionDestroy(p);
        if (i % 200 == 0) { printf("[ULT-1] %d/2000\n", i); fflush(stdout); }
    }
    printf("[ULT-1] done\n"); fflush(stdout);
}

// 攻击2: slot 耗尽 + 并发 Create 风暴
void attack_slot_exhaust(const char* xlsx, const char* pptx) {
    printf("[ULT-2] Slot exhaust + concurrent create\n"); fflush(stdout);
    std::vector<void*> holders;
    for (int i = 0; i < 8; i++) {
        void* s = (i % 2 == 0)
            ? CalcSessionCreate(xlsx, "", "attack_ult", OnFrame, nullptr, 1920, 1080)
            : ImpressSessionCreate(pptx, "", "attack_ult", OnFrame, nullptr, 1920, 1080);
        if (s) {
            if (i % 2 == 0) CalcSessionStart(s); else ImpressSessionStart(s);
            holders.push_back(s);
        }
    }
    printf("[ULT-2] holding %zu slots\n", holders.size()); fflush(stdout);

    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 16; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* s = (r % 2 == 0)
                    ? CalcSessionCreate(xlsx, "", "attack_ult", OnFrame, nullptr, 1920, 1080)
                    : ImpressSessionCreate(pptx, "", "attack_ult", OnFrame, nullptr, 1920, 1080);
                if (s) {
                    if (r % 2 == 0) CalcSessionDestroy(s); else ImpressSessionDestroy(s);
                }
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(10));
    stop.store(true);
    for (auto& t : ts) t.join();
    for (auto s : holders) {
        if (holders.size() % 2 == 0) CalcSessionDestroy(s); else ImpressSessionDestroy(s);
    }
    printf("[ULT-2] done\n"); fflush(stdout);
}

// 攻击3: 8 路并发，每路 Create→Start→密集操作→Destroy
void attack_8way_ops(const char* xlsx, const char* pptx) {
    printf("[ULT-3] 8-way concurrent ops\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 8; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                bool useCalc = (r % 2 == 0);
                const char* path = useCalc ? xlsx : pptx;
                void* s = useCalc
                    ? CalcSessionCreate(path, "", "attack_ult", OnFrame, nullptr, 1920, 1080)
                    : ImpressSessionCreate(path, "", "attack_ult", OnFrame, nullptr, 1920, 1080);
                if (!s) continue;
                if (useCalc) CalcSessionStart(s); else ImpressSessionStart(s);

                for (int i = 0; i < 10; i++) {
                    if (useCalc) {
                        CalcSessionSetResolution(s, 640 + (i*200), 480 + (i*150));
                        CalcSessionNextPage(s);
                        CalcSessionUpdateFrame(s);
                        CalcSessionSetScale(s, 100 + (i*30));
                        CalcSessionMoveScroll(s, 1, -1);
                    } else {
                        ImpressSessionSetResolution(s, 640 + (i*200), 480 + (i*150));
                        ImpressSessionNextPage(s);
                        ImpressSessionUpdateFrame(s);
                        ImpressSessionSetMute(s, i & 1);
                        ImpressSessionGoToPage(s, i % 5);
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }

                if (useCalc) CalcSessionDestroy(s); else ImpressSessionDestroy(s);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(12));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[ULT-3] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// 攻击4: 单 session 12 路并发操作
void attack_12way_session(const char* pptx) {
    printf("[ULT-4] 12-way single session\n"); fflush(stdout);
    void* p = ImpressSessionCreate(pptx, "", "attack_ult", OnFrame, nullptr, 1920, 1080);
    if (!p) { printf("create failed\n"); return; }
    ImpressSessionStart(p);

    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < 12; t++) {
        ts.emplace_back([&, t]() {
            while (!stop.load()) {
                switch (t % 6) {
                    case 0: ImpressSessionNextPage(p); break;
                    case 1: ImpressSessionPreviousPage(p); break;
                    case 2: ImpressSessionGoToPage(p, t % 5); break;
                    case 3: ImpressSessionSetResolution(p, 640 + (t*150), 480 + (t*120)); break;
                    case 4: ImpressSessionSetMute(p, t & 1); break;
                    case 5: ImpressSessionUpdateFrame(p); break;
                }
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& x : ts) x.join();
    ImpressSessionDestroy(p);
    printf("[ULT-4] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// 攻击5: 交替极速 resize（20000 次）
void attack_resize_20k(const char* xlsx, const char* pptx) {
    printf("[ULT-5] 20000x alternating resize\n"); fflush(stdout);
    void* c = CalcSessionCreate(xlsx, "", "attack_ult", OnFrame, nullptr, 1920, 1080);
    void* p = ImpressSessionCreate(pptx, "", "attack_ult", OnFrame, nullptr, 1920, 1080);
    if (!c || !p) { printf("create failed\n"); return; }
    CalcSessionStart(c);
    ImpressSessionStart(p);

    for (int i = 0; i < 20000; i++) {
        int w = 640 + ((i * 137) % 3000);
        int h = 480 + ((i * 173) % 2000);
        CalcSessionSetResolution(c, w, h);
        ImpressSessionSetResolution(p, w, h);
        if (i % 2000 == 0) { printf("[ULT-5] %d/20000\n", i); fflush(stdout); }
    }
    CalcSessionDestroy(c);
    ImpressSessionDestroy(p);
    printf("[ULT-5] done\n"); fflush(stdout);
}

// 攻击6: 先 rapid create-destroy 预热，再 slot 耗尽 + 并发
void attack_warmup_then_exhaust(const char* xlsx, const char* pptx) {
    printf("[ULT-6] Warmup then exhaust\n"); fflush(stdout);
    // 先 500 轮 Create→Destroy 预热
    for (int i = 0; i < 500; i++) {
        void* c = CalcSessionCreate(xlsx, "", "attack_ult", OnFrame, nullptr, 1920, 1080);
        if (c) CalcSessionDestroy(c);
        if (i % 100 == 0) { printf("[ULT-6] warmup %d/500\n", i); fflush(stdout); }
    }

    // 再占满 slot
    std::vector<void*> holders;
    for (int i = 0; i < 8; i++) {
        void* s = CalcSessionCreate(xlsx, "", "attack_ult", OnFrame, nullptr, 1920, 1080);
        if (s) { CalcSessionStart(s); holders.push_back(s); }
    }
    printf("[ULT-6] holding %zu slots after warmup\n", holders.size()); fflush(stdout);

    // 并发 Create（slot 耗尽）
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 16; r++) {
        ts.emplace_back([&]() {
            while (!stop.load()) {
                void* s = CalcSessionCreate(xlsx, "", "attack_ult", OnFrame, nullptr, 1920, 1080);
                if (s) CalcSessionDestroy(s);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    for (auto& t : ts) t.join();
    for (auto s : holders) CalcSessionDestroy(s);
    printf("[ULT-6] done\n"); fflush(stdout);
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

    printf("=== ULTIMATE STORM ATTACK ===\n"); fflush(stdout);
    if (att == 0 || att == 1) { try { attack_rapid_cd(xlsx, pptx); } catch (...) { printf(">>> ULT-1 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 2) { try { attack_slot_exhaust(xlsx, pptx); } catch (...) { printf(">>> ULT-2 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 3) { try { attack_8way_ops(xlsx, pptx); } catch (...) { printf(">>> ULT-3 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 4) { try { attack_12way_session(pptx); } catch (...) { printf(">>> ULT-4 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 5) { try { attack_resize_20k(xlsx, pptx); } catch (...) { printf(">>> ULT-5 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 6) { try { attack_warmup_then_exhaust(xlsx, pptx); } catch (...) { printf(">>> ULT-6 CRASH (1pt)\n"); score++; } }
    printf("=== ULT FINAL SCORE: %d/6 ===\n", score); fflush(stdout);
    return 0;
}
