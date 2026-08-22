// attack_uaf_probe.cpp — 悬垂指针/重复释放攻击探针
// 已存在的 7 个攻击探针未覆盖：销毁与会话内部帧泵/平台资源的竞态窗口、
// NULL/重复 Destroy、销毁后继续调用 API。
// 目标：制造 use-after-free / double-free / 崩溃。每类崩溃或卡死 = 1 分。
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

// 攻击1: Destroy 与帧泵运行并发 —— Destroy 在 mu_ 外 reset platform_/pump_，
// 而 FramePump::PollThread 可能正持 frame_mutex_ 跑 frame_fn_（CaptureFrame 触碰 platform_）。
// 双线程：一个不停 Start/Stop 触发泵启停，另一个穿插 Destroy。
void attack_destroy_race(const char* xlsx, const char* pptx) {
    printf("[UAF-1] Destroy vs pump race\n"); fflush(stdout);
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int r = 0; r < 8; r++) {
        ts.emplace_back([&, r]() {
            while (!stop.load()) {
                void* s = CalcSessionCreate(xlsx, "", "attack_uaf", OnFrame, nullptr, 1920, 1080);
                if (!s) continue;
                CalcSessionStart(s);
                // 极短存活：在泵 tick(20ms) 内即销毁，最大化 Destroy 命中 PollThread 的概率
                std::this_thread::sleep_for(std::chrono::milliseconds(r % 2 ? 3 : 8));
                CalcSessionDestroy(s); // 触发 platform_/pump_ reset
            }
        });
    }
    for (int i = 0; i < 3000; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true);
    for (auto& t : ts) t.join();
    printf("[UAF-1] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// 攻击2: NULL 与重复 Destroy —— ABI 层对空指针/已销毁对象无防护
void attack_null_double_destroy(const char* xlsx) {
    printf("[UAF-2] NULL + double Destroy\n"); fflush(stdout);
    // NULL session 各 API（C ABI 仅判空返回 0，应安全，但若某 API 漏判即崩）
    CalcSessionDestroy(nullptr);
    ImpressSessionDestroy(nullptr);
    CalcSessionStart(nullptr);
    CalcSessionStop(nullptr);
    CalcSessionNextPage(nullptr);
    CalcSessionUpdateFrame(nullptr);
    CalcSessionSetResolution(nullptr, 1, 1);
    ImpressSessionNextPage(nullptr);
    ImpressSessionSetMute(nullptr, 1);
    ImpressSessionGetWidth(nullptr);
    CalcSessionGetWidth(nullptr);

    void* s = CalcSessionCreate(xlsx, "", "attack_uaf", OnFrame, nullptr, 1920, 1080);
    if (s) {
        CalcSessionStart(s);
        CalcSessionDestroy(s);
        // 重复 Destroy（销毁后对象已被 delete，再次 delete = UB/崩溃）
        CalcSessionDestroy(s);
        // 销毁后再调用 API（悬垂指针）
        CalcSessionStart(s);
        CalcSessionNextPage(s);
        CalcSessionUpdateFrame(s);
        CalcSessionSetResolution(s, 800, 600);
    }
    printf("[UAF-2] done\n"); fflush(stdout);
}

// 攻击3: 销毁后立即用同指针地址重新 Create（堆重用），旧悬垂引用可能命中
void attack_double_create_same_slot(const char* pptx) {
    printf("[UAF-3] recrecreate same heap slot\n"); fflush(stdout);
    for (int i = 0; i < 200; i++) {
        void* s = ImpressSessionCreate(pptx, "", "attack_uaf", OnFrame, nullptr, 1920, 1080);
        if (s) {
            ImpressSessionStart(s);
            ImpressSessionDestroy(s); // delete this
            // 立即再次分配，堆很可能复用同一地址；若内部有残留异步引用即崩
        }
    }
    printf("[UAF-3] done\n"); fflush(stdout);
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
    printf("=== UAF / DOUBLE-FREE ATTACK ===\n"); fflush(stdout);
    if (att == 0 || att == 1) { try { attack_destroy_race(xlsx, pptx); } catch (...) { printf(">>> UAF-1 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 2) { try { attack_null_double_destroy(xlsx); } catch (...) { printf(">>> UAF-2 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 3) { try { attack_double_create_same_slot(pptx); } catch (...) { printf(">>> UAF-3 CRASH (1pt)\n"); score++; } }
    printf("=== UAF FINAL SCORE: %d/3 ===\n", score); fflush(stdout);
    return 0;
}
