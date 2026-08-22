// attack_shm_race_probe.cpp — XShm 段重建与抓帧竞态攻击
// 目标: ShmState::Ensure 的 Release()+重建 与 CaptureFrame 的 XShmGetImage 无锁并发
// 导致 use-after-free / 崩溃。SetWindowSize 触发 Ensure (XShmDetach+shmdt+shmctl),
// 而 FramePump 线程持 frame_mutex_ 调 CaptureFrame 读同一 shm.img。
// 两把锁不同 (mu_ vs frame_mutex_), 存在窗口期。
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

// 攻击1: 多线程并发 resize + UpdateFrame (XShm 段重建 vs 抓帧)
// SetWindowSize 持 mu_ 调 Ensure(Release+重建), CaptureFrame 持 frame_mutex_ 读 shm.img
// 两锁不重合 → 窗口期: Ensure 的 Release 释放了 shm 段, CaptureFrame 的 XShmGetImage 读已释放内存
void attack_shm_resize_capture_race(const char* xlsx, const char* pptx) {
    printf("[SHM-1] XShm resize vs capture race\n"); fflush(stdout);
    void* c = CalcSessionCreate(xlsx, "", "attack_shm", OnFrame, nullptr, 1920, 1080);
    void* p = ImpressSessionCreate(pptx, "", "attack_shm", OnFrame, nullptr, 1920, 1080);
    if (!c || !p) { printf("[SHM-1] create failed\n"); return; }
    CalcSessionStart(c);
    ImpressSessionStart(p);

    std::atomic<bool> stop{false};
    // 线程1: 高频 resize (触发 XShm 段重建)
    std::thread resizer([&]() {
        int dims[][2] = {{640,480},{1280,720},{1920,1080},{2560,1440},{800,600},{3840,2160}};
        int idx = 0;
        while (!stop.load()) {
            auto& d = dims[idx % 6];
            CalcSessionSetResolution(c, d[0], d[1]);
            ImpressSessionSetResolution(p, d[0], d[1]);
            idx++;
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });
    // 线程2: 高频 UpdateFrame (触发 CaptureFrame 读 shm.img)
    std::thread capturer([&]() {
        while (!stop.load()) {
            CalcSessionUpdateFrame(c);
            ImpressSessionUpdateFrame(p);
        }
    });
    // 线程3: 高频 Start/Stop (触发 FramePump 启停, 改变 frame_mutex_ 持有者)
    std::thread starter([&]() {
        while (!stop.load()) {
            CalcSessionStop(c);
            ImpressSessionStop(p);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            CalcSessionStart(c);
            ImpressSessionStart(p);
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(10));
    stop.store(true);
    resizer.join(); capturer.join(); starter.join();
    CalcSessionDestroy(c);
    ImpressSessionDestroy(p);
    printf("[SHM-1] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// 攻击2: 多 session 并发 resize + 并发 Destroy (XShm 段在 Cleanup 中被释放,
// 另一 session 的 CaptureFrame 可能读到已 detach 的段)
void attack_shm_cross_session_race(const char* xlsx, const char* pptx) {
    printf("[SHM-2] cross-session XShm race\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int i = 0; i < 6; i++) {
        ts.emplace_back([&, i]() {
            while (!stop.load()) {
                bool useCalc = (i % 2 == 0);
                const char* path = useCalc ? xlsx : pptx;
                void* s = useCalc
                    ? CalcSessionCreate(path, "", "attack_shm", OnFrame, nullptr, 1920, 1080)
                    : ImpressSessionCreate(path, "", "attack_shm", OnFrame, nullptr, 1920, 1080);
                if (!s) continue;
                if (useCalc) CalcSessionStart(s); else ImpressSessionStart(s);
                // 高频 resize + UpdateFrame 混合
                for (int j = 0; j < 50; j++) {
                    if (useCalc) {
                        CalcSessionSetResolution(s, 640 + (j%5)*320, 480 + (j%5)*240);
                        CalcSessionUpdateFrame(s);
                    } else {
                        ImpressSessionSetResolution(s, 640 + (j%5)*320, 480 + (j%5)*240);
                        ImpressSessionUpdateFrame(s);
                    }
                }
                if (useCalc) CalcSessionDestroy(s); else ImpressSessionDestroy(s);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::seconds(12));
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[SHM-2] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// 攻击3: 单 session 极端 resize 模式 — 交替极小/极大/0/负, 触发 Ensure 反复 Release+重建
void attack_shm_extreme_resize_pattern(const char* xlsx) {
    printf("[SHM-3] extreme resize pattern\n"); fflush(stdout);
    void* c = CalcSessionCreate(xlsx, "", "attack_shm", OnFrame, nullptr, 1920, 1080);
    if (!c) { printf("[SHM-3] create failed\n"); return; }
    CalcSessionStart(c);
    // 交替合法/非法尺寸, 让 Ensure 反复 Release+重建
    for (int i = 0; i < 2000; i++) {
        int w, h;
        switch (i % 8) {
            case 0: w=1; h=1; break;
            case 1: w=32767; h=32767; break;  // 16 位坐标上限
            case 2: w=1920; h=1080; break;
            case 3: w=3840; h=2160; break;
            case 4: w=640; h=480; break;
            case 5: w=8000; h=6000; break;    // 超大但合法
            case 6: w=1920; h=1080; break;
            case 7: w=16; h=16; break;        // 极小
        }
        CalcSessionSetResolution(c, w, h);
        CalcSessionUpdateFrame(c);
        if (i % 100 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CalcSessionDestroy(c);
    printf("[SHM-3] done\n"); fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { fprintf(stderr, "usage: %s <xlsx> <pptx> [att=1|2|3|all]\n", argv[0]); return 2; }
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int att = (argc >= 4 && strcmp(argv[3], "all") == 0) ? 0 : atoi(argv[3]);
    int score = 0;
    printf("=== XSHM RACE ATTACK ===\n"); fflush(stdout);
    if (att == 0 || att == 1) { try { attack_shm_resize_capture_race(xlsx, pptx); } catch (...) { printf(">>> SHM-1 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 2) { try { attack_shm_cross_session_race(xlsx, pptx); } catch (...) { printf(">>> SHM-2 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 3) { try { attack_shm_extreme_resize_pattern(xlsx); } catch (...) { printf(">>> SHM-3 CRASH (1pt)\n"); score++; } }
    printf("=== SHM FINAL SCORE: %d/3 ===\n", score); fflush(stdout);
    return 0;
}
