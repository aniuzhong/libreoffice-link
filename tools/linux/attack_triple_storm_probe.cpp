// attack_triple_storm.cpp — 三路并发风暴攻击
// 同时高频 resize + Start/Stop + Destroy，最大化竞态窗口命中率
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

// 攻击1: 单 session 三路并发 — resize + start/stop + destroy
void attack_triple_storm_single(const char* pptx) {
    printf("[TRIPLE-1] Single session triple storm\n"); fflush(stdout);
    std::atomic<bool> stop{false};

    void* p = ImpressSessionCreate(pptx, "", "attack_triple", OnFrame, nullptr, 1920, 1080);
    if (!p) { printf("create failed\n"); return; }
    ImpressSessionStart(p);

    // 线程1: 高频 resize
    std::thread resizer([&]() {
        int w = 640, h = 480;
        while (!stop.load()) {
            w = (w == 640) ? 3840 : 640;
            h = (h == 480) ? 2160 : 480;
            ImpressSessionSetResolution(p, w, h);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // 线程2: 高频 Start/Stop
    std::thread starter([&]() {
        while (!stop.load()) {
            ImpressSessionStop(p);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            ImpressSessionStart(p);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    // 线程3: 高频翻页
    std::thread nav([&]() {
        while (!stop.load()) {
            ImpressSessionNextPage(p);
            ImpressSessionPreviousPage(p);
            ImpressSessionGoToPage(p, 0);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    // 线程4: 高频 UpdateFrame
    std::thread capturer([&]() {
        while (!stop.load()) {
            ImpressSessionUpdateFrame(p);
        }
    });

    // 线程5: 高频 SetMute
    std::thread muter([&]() {
        int v = 0;
        while (!stop.load()) {
            ImpressSessionSetMute(p, v++ & 1);
            std::this_thread::sleep_for(std::chrono::microseconds(150));
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(8));
    stop.store(true);
    resizer.join(); starter.join(); nav.join(); capturer.join(); muter.join();
    ImpressSessionDestroy(p);
    printf("[TRIPLE-1] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// 攻击2: 多 session 并发创建 + 每 session 三路风暴
void attack_triple_storm_multi(const char* xlsx, const char* pptx) {
    printf("[TRIPLE-2] Multi-session triple storm\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;

    for (int r = 0; r < 6; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* s = (r % 2 == 0)
                    ? CalcSessionCreate(xlsx, "", "attack_triple", OnFrame, nullptr, 1920, 1080)
                    : ImpressSessionCreate(pptx, "", "attack_triple", OnFrame, nullptr, 1920, 1080);
                if (!s) continue;
                if (r % 2 == 0) CalcSessionStart(s); else ImpressSessionStart(s);

                // 每 session 内部三路并发
                std::thread t1([&, s, r]() {
                    for (int i = 0; i < 20 && !stop.load(); i++) {
                        if (r % 2 == 0) {
                            CalcSessionSetResolution(s, 640 + (i*200), 480 + (i*150));
                            CalcSessionUpdateFrame(s);
                        } else {
                            ImpressSessionSetResolution(s, 640 + (i*200), 480 + (i*150));
                            ImpressSessionUpdateFrame(s);
                        }
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                });
                std::thread t2([&, s, r]() {
                    for (int i = 0; i < 20 && !stop.load(); i++) {
                        if (r % 2 == 0) {
                            CalcSessionStop(s);
                            std::this_thread::sleep_for(std::chrono::microseconds(50));
                            CalcSessionStart(s);
                        } else {
                            ImpressSessionStop(s);
                            std::this_thread::sleep_for(std::chrono::microseconds(50));
                            ImpressSessionStart(s);
                        }
                    }
                });
                std::thread t3([&, s, r]() {
                    for (int i = 0; i < 20 && !stop.load(); i++) {
                        if (r % 2 == 0) {
                            CalcSessionNextPage(s);
                            CalcSessionMoveScroll(s, 1, 1);
                        } else {
                            ImpressSessionNextPage(s);
                            ImpressSessionGoToPage(s, i % 5);
                        }
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                });
                t1.join(); t2.join(); t3.join();

                if (r % 2 == 0) CalcSessionDestroy(s); else ImpressSessionDestroy(s);
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(15));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[TRIPLE-2] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// 攻击3: 极速 Create→Destroy 循环 + 每轮内并发操作
void attack_rapid_create_ops(const char* xlsx, const char* pptx) {
    printf("[TRIPLE-3] Rapid create+ops+destroy\n"); fflush(stdout);
    for (int i = 0; i < 500; i++) {
        void* c = CalcSessionCreate(xlsx, "", "attack_triple", OnFrame, nullptr, 1920, 1080);
        void* p = ImpressSessionCreate(pptx, "", "attack_triple", OnFrame, nullptr, 1920, 1080);
        if (c) {
            CalcSessionStart(c);
            CalcSessionSetResolution(c, 1280, 720);
            CalcSessionNextPage(c);
            CalcSessionUpdateFrame(c);
            CalcSessionDestroy(c);
        }
        if (p) {
            ImpressSessionStart(p);
            ImpressSessionSetResolution(p, 1280, 720);
            ImpressSessionNextPage(p);
            ImpressSessionUpdateFrame(p);
            ImpressSessionDestroy(p);
        }
        if (i % 50 == 0) { printf("[TRIPLE-3] %d/500\n", i); fflush(stdout); }
    }
    printf("[TRIPLE-3] done\n"); fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <xlsx> <pptx> [att=1|2|3|all]\n", argv[0]);
        return 2;
    }
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int att = (argc >= 4 && strcmp(argv[3], "all") == 0) ? 0 : atoi(argv[3]);
    int score = 0;

    printf("=== TRIPLE STORM ATTACK ===\n"); fflush(stdout);
    if (att == 0 || att == 1) { try { attack_triple_storm_single(pptx); } catch (...) { printf(">>> TRIPLE-1 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 2) { try { attack_triple_storm_multi(xlsx, pptx); } catch (...) { printf(">>> TRIPLE-2 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 3) { try { attack_rapid_create_ops(xlsx, pptx); } catch (...) { printf(">>> TRIPLE-3 CRASH (1pt)\n"); score++; } }
    printf("=== TRIPLE FINAL SCORE: %d/3 ===\n", score); fflush(stdout);
    return 0;
}
