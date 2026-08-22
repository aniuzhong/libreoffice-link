// attack_cb_join_self.cpp — 回调中 Destroy 导致泵线程 join 自己
// PushFrame 在 frame_mutex_ 内调 cb_, cb_ 里调 Destroy → pump_->Stop() → poll_thread_.join()
// 当前线程就是 poll_thread_ → join 自己 → 崩溃或永久死锁
#include <abi/abi.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

static std::atomic<int> g_cb_count{0};
static void* g_session = nullptr;
static bool g_is_calc = false;

static void OnFrameJoinSelf(const uint8_t* d, int32_t w, int32_t h, int32_t rp,
                            int32_t s, int32_t f, void* o) {
    (void)d; (void)w; (void)h; (void)rp; (void)s; (void)f; (void)o;
    if (g_cb_count.fetch_add(1) > 0) return;

    void* sess = g_session;
    if (!sess) return;
    printf("  cb: calling Destroy (pump thread will join self!)\n"); fflush(stdout);
    if (g_is_calc) CalcSessionDestroy(sess);
    else ImpressSessionDestroy(sess);
    g_session = nullptr;
    printf("  cb: Destroy returned (should not reach here if join self crashed)\n"); fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) return 2;
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int score = 0;

    printf("=== JOIN SELF ATTACK ===\n"); fflush(stdout);

    printf("--- A1: cb->Destroy (calc, pump thread joins self) ---\n"); fflush(stdout);
    g_cb_count.store(0);
    g_is_calc = true;
    g_session = CalcSessionCreate(xlsx, "", "joinself", OnFrameJoinSelf, nullptr, 1920, 1080);
    if (g_session) {
        CalcSessionStart(g_session);
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        printf("  survived 2s (join self didn't crash?)\n"); fflush(stdout);
        g_session = nullptr;
    }

    printf("--- A2: cb->Destroy (impress) ---\n"); fflush(stdout);
    g_cb_count.store(0);
    g_is_calc = false;
    g_session = ImpressSessionCreate(pptx, "", "joinself", OnFrameJoinSelf, nullptr, 1920, 1080);
    if (g_session) {
        ImpressSessionStart(g_session);
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        printf("  survived 2s\n"); fflush(stdout);
        g_session = nullptr;
    }

    printf("\n=== FINAL SCORE: %d/2 ===\n", score); fflush(stdout);
    return 0;
}
