// attack_cb_reenter.cpp — 回调重入攻击
// 在 cb_ 回调中调用各种 ABI 函数, 测试递归锁/死锁/崩溃
#include <abi/abi.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

static std::atomic<int> g_cb_count{0};
static std::atomic<bool> g_cb_done{false};
static void* g_cb_session = nullptr;
static bool g_is_calc = false;

// 回调: 在帧回调中调用各种 ABI 函数
static void OnFrameReenter(const uint8_t* d, int32_t w, int32_t h, int32_t rp,
                           int32_t s, int32_t f, void* o) {
    (void)d; (void)w; (void)h; (void)rp; (void)s; (void)f; (void)o;
    int n = g_cb_count.fetch_add(1);
    if (n > 50 || g_cb_done.load()) return;  // 防止无限递归

    void* sess = g_cb_session;
    if (!sess) return;

    // 攻击: 在回调中调 UpdateFrame → 递归进 frame_mutex_
    if (n == 0) {
        printf("  cb[%d]: calling UpdateFrame (reenter frame_mutex_)\n", n); fflush(stdout);
        if (g_is_calc) CalcSessionUpdateFrame(sess);
        else ImpressSessionUpdateFrame(sess);
    }
    // 攻击: 在回调中调 Start/Stop
    if (n == 1) {
        printf("  cb[%d]: calling Stop (reenter mu_)\n", n); fflush(stdout);
        if (g_is_calc) CalcSessionStop(sess);
        else ImpressSessionStop(sess);
    }
    // 攻击: 在回调中调 Destroy
    if (n == 2) {
        printf("  cb[%d]: calling Destroy (delete this in callback)\n", n); fflush(stdout);
        g_cb_done.store(true);
        if (g_is_calc) CalcSessionDestroy(sess);
        else ImpressSessionDestroy(sess);
        g_cb_session = nullptr;
    }
    // 攻击: 在回调中调 NextPage
    if (n == 3) {
        printf("  cb[%d]: calling NextPage\n", n); fflush(stdout);
        if (g_is_calc) CalcSessionNextPage(sess);
        else ImpressSessionNextPage(sess);
    }
    // 攻击: 在回调中调 SetResolution
    if (n == 4) {
        printf("  cb[%d]: calling SetResolution\n", n); fflush(stdout);
        if (g_is_calc) CalcSessionSetResolution(sess, 800, 600);
        else ImpressSessionSetResolution(sess, 800, 600);
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) return 2;
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int score = 0;

    printf("=== CB REENTER ATTACK ===\n"); fflush(stdout);

    // 攻击1: calc 回调中 UpdateFrame (递归 frame_mutex_)
    printf("--- A1: Calc cb->UpdateFrame (frame_mutex_ reenter) ---\n"); fflush(stdout);
    g_cb_count.store(0);
    g_cb_done.store(false);
    g_is_calc = true;
    g_cb_session = CalcSessionCreate(xlsx, "", "cbre", OnFrameReenter, nullptr, 1920, 1080);
    if (g_cb_session) {
        printf("  created, starting...\n"); fflush(stdout);
        CalcSessionStart(g_cb_session);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (g_cb_session) {
            printf("  survived, destroying...\n"); fflush(stdout);
            CalcSessionDestroy(g_cb_session);
            g_cb_session = nullptr;
        }
        printf("  A1 done\n"); fflush(stdout);
    }

    // 攻击2: impress 回调中 Destroy (delete this in callback)
    printf("--- A2: Impress cb->Destroy (delete this in callback) ---\n"); fflush(stdout);
    g_cb_count.store(0);
    g_cb_done.store(false);
    g_is_calc = false;
    g_cb_session = ImpressSessionCreate(pptx, "", "cbre", OnFrameReenter, nullptr, 1920, 1080);
    if (g_cb_session) {
        ImpressSessionStart(g_cb_session);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (g_cb_session) {
            ImpressSessionDestroy(g_cb_session);
            g_cb_session = nullptr;
        }
        printf("  A2 done\n"); fflush(stdout);
    }

    // 攻击3: 回调中调 Start/Stop 风暴
    printf("--- A3: cb->Start/Stop storm ---\n"); fflush(stdout);
    g_cb_count.store(0);
    g_cb_done.store(false);
    g_is_calc = true;
    g_cb_session = CalcSessionCreate(xlsx, "", "cbre", OnFrameReenter, nullptr, 1920, 1080);
    if (g_cb_session) {
        CalcSessionStart(g_cb_session);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (g_cb_session) CalcSessionDestroy(g_cb_session);
        g_cb_session = nullptr;
        printf("  A3 done\n"); fflush(stdout);
    }

    printf("\n=== FINAL SCORE: %d/3 ===\n", score); fflush(stdout);
    return 0;
}
