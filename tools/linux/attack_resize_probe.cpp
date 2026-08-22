// attack_resize_probe.cpp — 运行期分辨率变更攻击探针
// 已覆盖：边界分辨率（attack_boundary）。未覆盖：运行期 SetResolution 与帧泵
// CaptureFrame 的 XShm 段重建竞态、超大/0/负尺寸、连续高频 resize。
// 目标：XShm 段撕裂 / 崩溃 / 卡死。每类 = 1 分。
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

// 攻击1: resize 与帧泵并发 —— SetResolution 持 mu_ 调 platform_->SetWindowSize
// （XShm 段可能重建），而 PollThread 持 frame_mutex_ 调 CaptureFrame 读同一段。
// 两把锁不同（mu_ vs frame_mutex_），存在窗口期。
void attack_resize_race(const char* xlsx, const char* pptx) {
    printf("[RES-1] resize vs capture race\n"); fflush(stdout);
    void* c = CalcSessionCreate(xlsx, "", "attack_resize", OnFrame, nullptr, 1920, 1080);
    void* p = ImpressSessionCreate(pptx, "", "attack_resize", OnFrame, nullptr, 1920, 1080);
    if (c) CalcSessionStart(c);
    if (p) ImpressSessionStart(p);
    std::atomic<bool> stop{false};
    std::thread resizer([&]() {
        int w = 640, h = 480;
        while (!stop.load()) {
            w = (w == 640) ? 3840 : 640;
            h = (h == 480) ? 2160 : 480;
            if (c) CalcSessionSetResolution(c, w, h);
            if (p) ImpressSessionSetResolution(p, w, h);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    // 主线程持续驱动帧（Impress UpdateFrame 与 ticker 并发）
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(8)) {
        if (c) CalcSessionUpdateFrame(c);
        if (p) ImpressSessionUpdateFrame(p);
    }
    stop.store(true); resizer.join();
    if (c) CalcSessionDestroy(c);
    if (p) ImpressSessionDestroy(p);
    printf("[RES-1] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// 攻击2: 非法尺寸 SetResolution —— 0/负/超大(超 16 位坐标上限 32767)/奇葩
void attack_bad_resolution(const char* xlsx) {
    printf("[RES-2] illegal resolutions\n"); fflush(stdout);
    void* c = CalcSessionCreate(xlsx, "", "attack_resize", OnFrame, nullptr, 1920, 1080);
    if (!c) { printf("[RES-2] create failed\n"); return; }
    CalcSessionStart(c);
    static const int dims[][2] = {
        {0, 0}, {-1, -1}, {0, 1080}, {1920, 0}, {32768, 2160}, {40000, 40000},
        {1, 1}, {2147483647, 2147483647}, {1920, -1080}, {-1920, 1080}
    };
    for (auto& d : dims) {
        CalcSessionSetResolution(c, d[0], d[1]);
        CalcSessionUpdateFrame(c); // 立刻取帧，观察段是否撕裂
        CalcSessionSetResolution(c, 1920, 1080); // 立即恢复，放大竞态
    }
    CalcSessionDestroy(c);
    printf("[RES-2] done\n"); fflush(stdout);
}

// 攻击3: 高频连续 resize（无停顿），制造 XShm 反复申请/释放
void attack_rapid_resize(const char* pptx) {
    printf("[RES-3] rapid resize storm\n"); fflush(stdout);
    void* p = ImpressSessionCreate(pptx, "", "attack_resize", OnFrame, nullptr, 1920, 1080);
    if (!p) { printf("[RES-3] create failed\n"); return; }
    ImpressSessionStart(p);
    for (int i = 0; i < 5000; i++) {
        int w = (i % 2) ? 1280 : 2560;
        int h = (i % 2) ? 720 : 1440;
        ImpressSessionSetResolution(p, w, h);
        if (i % 50 == 0) ImpressSessionUpdateFrame(p);
    }
    ImpressSessionDestroy(p);
    printf("[RES-3] done\n"); fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { fprintf(stderr, "usage: %s <xlsx> <pptx> [att=1|2|3|all]\n", argv[0]); return 2; }
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int att = (argc >= 4 && strcmp(argv[3], "all") == 0) ? 0 : atoi(argv[3]);
    int score = 0;
    printf("=== RESIZE ATTACK ===\n"); fflush(stdout);
    if (att == 0 || att == 1) { try { attack_resize_race(xlsx, pptx); } catch (...) { printf(">>> RES-1 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 2) { try { attack_bad_resolution(xlsx); } catch (...) { printf(">>> RES-2 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 3) { try { attack_rapid_resize(pptx); } catch (...) { printf(">>> RES-3 CRASH (1pt)\n"); score++; } }
    printf("=== RESIZE FINAL SCORE: %d/3 ===\n", score); fflush(stdout);
    return 0;
}
