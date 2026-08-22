// attack_cb_destroy.cpp — 回调中 Destroy (delete this) 攻击
#include <base/abi.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

static std::atomic<int> g_cb_count{0};
static void* g_cb_session = nullptr;
static bool g_is_calc = false;

// 回调: 第一次调用就 Destroy
static void OnFrameDestroy(const uint8_t* d, int32_t w, int32_t h, int32_t rp,
                           int32_t s, int32_t f, void* o) {
    (void)d; (void)w; (void)h; (void)rp; (void)s; (void)f; (void)o;
    int n = g_cb_count.fetch_add(1);
    if (n > 0) return;  // 只执行一次

    void* sess = g_cb_session;
    if (!sess) return;

    printf("  cb: calling Destroy (delete this in callback!)\n"); fflush(stdout);
    if (g_is_calc) CalcSessionDestroy(sess);
    else ImpressSessionDestroy(sess);
    g_cb_session = nullptr;
    printf("  cb: Destroy returned (this was deleted!)\n"); fflush(stdout);
}

// 回调: 第一次调 UpdateFrame (递归 frame_mutex_), 第二次调 Destroy
static void OnFrameUpdateThenDestroy(const uint8_t* d, int32_t w, int32_t h, int32_t rp,
                                     int32_t s, int32_t f, void* o) {
    (void)d; (void)w; (void)h; (void)rp; (void)s; (void)f; (void)o;
    int n = g_cb_count.fetch_add(1);
    void* sess = g_cb_session;
    if (!sess) return;

    if (n == 0) {
        printf("  cb[0]: calling UpdateFrame (reenter frame_mutex_)\n"); fflush(stdout);
        if (g_is_calc) CalcSessionUpdateFrame(sess);
        else ImpressSessionUpdateFrame(sess);
    } else if (n == 1) {
        printf("  cb[1]: calling Destroy\n"); fflush(stdout);
        if (g_is_calc) CalcSessionDestroy(sess);
        else ImpressSessionDestroy(sess);
        g_cb_session = nullptr;
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) return 2;
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int score = 0;

    printf("=== CB DESTROY ATTACK ===\n"); fflush(stdout);

    // 攻击1: 回调中直接 Destroy (delete this)
    printf("--- A1: cb->Destroy (calc) ---\n"); fflush(stdout);
    g_cb_count.store(0);
    g_is_calc = true;
    g_cb_session = CalcSessionCreate(xlsx, "", "cbkill", OnFrameDestroy, nullptr, 1920, 1080);
    if (g_cb_session) {
        CalcSessionStart(g_cb_session);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        printf("  survived 1s after cb->Destroy\n"); fflush(stdout);
        // session 可能已被回调销毁, 不二次 Destroy
        g_cb_session = nullptr;
    }

    // 攻击2: 回调中 UpdateFrame (递归锁) + Destroy
    printf("--- A2: cb->UpdateFrame then Destroy (calc) ---\n"); fflush(stdout);
    g_cb_count.store(0);
    g_is_calc = true;
    g_cb_session = CalcSessionCreate(xlsx, "", "cbkill", OnFrameUpdateThenDestroy, nullptr, 1920, 1080);
    if (g_cb_session) {
        CalcSessionStart(g_cb_session);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        printf("  survived 1s\n"); fflush(stdout);
        g_cb_session = nullptr;
    }

    // 攻击3: impress 回调中 Destroy
    printf("--- A3: cb->Destroy (impress) ---\n"); fflush(stdout);
    g_cb_count.store(0);
    g_is_calc = false;
    g_cb_session = ImpressSessionCreate(pptx, "", "cbkill", OnFrameDestroy, nullptr, 1920, 1080);
    if (g_cb_session) {
        ImpressSessionStart(g_cb_session);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        printf("  survived 1s\n"); fflush(stdout);
        g_cb_session = nullptr;
    }

    printf("\n=== FINAL SCORE: %d/3 ===\n", score); fflush(stdout);
    return 0;
}
