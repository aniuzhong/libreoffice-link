// attack_lock_inversion.cpp — 锁序反转攻击
// frame_mutex_ → mu_ 是允许的锁序, 但 mu_ → frame_mutex_ 是禁止的。
// 如果某个路径先持 mu_ 再调 UpdateFrame (持 frame_mutex_), 就反转了。
// 检查: NextPage 持 mu_, 里面不调 UpdateFrame。SetResolution 持 mu_, 里面不调 UpdateFrame。
// 但 PushFrame 持 frame_mutex_, 里面不持 mu_。所以锁序是安全的。
// 
// 攻击点: 多线程并发, 一个线程持 mu_ 做操作, 另一个线程持 frame_mutex_ 做操作,
// 如果两者都需要对方锁 → 死锁。但当前代码没有双向等待。
//
// 唯一可能: Destroy 在 mu_ 外 Stop(pump), 但 Stop 要 join 泵线程。
// 如果泵线程正在 PushFrame (持 frame_mutex_), PushFrame 里 cb_ 调了某个持 mu_ 的函数...
// 而 Destroy 的线程持了 mu_ (在 Stop 之后) → 泵线程等 mu_, Destroy 线程等泵线程 join → 死锁
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

// 回调: 持 frame_mutex_, 尝试获取 mu_ (通过调 NextPage/Stop 等)
static void OnFrameLockInvert(const uint8_t* d, int32_t w, int32_t h, int32_t rp,
                              int32_t s, int32_t f, void* o) {
    (void)d; (void)w; (void)h; (void)rp; (void)s; (void)f; (void)o;
    int n = g_cb_count.fetch_add(1);
    if (n > 10) return;

    void* sess = g_session;
    if (!sess) return;

    // 在 frame_mutex_ 内尝试获取 mu_ (通过调 Stop, 它持 mu_)
    // 这本身是允许的锁序 (frame_mutex_ → mu_)
    // 但如果另一个线程持 mu_ 等 frame_mutex_, 就反转了
    if (n % 2 == 0) {
        if (g_is_calc) CalcSessionStop(sess);
        else ImpressSessionStop(sess);
    } else {
        if (g_is_calc) CalcSessionNextPage(sess);
        else ImpressSessionNextPage(sess);
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) return 2;
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int score = 0;

    printf("=== LOCK INVERSION ATTACK ===\n"); fflush(stdout);

    // 攻击: 线程A 持 mu_ 做操作, 线程B 持 frame_mutex_ 做操作
    // 如果线程A 需要 frame_mutex_ (调 UpdateFrame), 线程B 需要 mu_ (回调中调 Stop)
    // → 死锁
    printf("--- A1: mu_ vs frame_mutex_ deadlock (calc) ---\n"); fflush(stdout);
    g_cb_count.store(0);
    g_is_calc = true;
    g_session = CalcSessionCreate(xlsx, "", "lockinv", OnFrameLockInvert, nullptr, 1920, 1080);
    if (g_session) {
        CalcSessionStart(g_session);

        std::atomic<bool> stop{false};
        // 线程A: 持 mu_ 调 NextPage, 然后尝试调 UpdateFrame (持 frame_mutex_)
        std::thread t1([&]() {
            while (!stop.load()) {
                CalcSessionNextPage(g_session);  // 持 mu_
                CalcSessionUpdateFrame(g_session); // 持 frame_mutex_ (锁序: mu_ → frame_mutex_)
            }
        });
        // 线程B: 持 frame_mutex_ (通过 PushFrame 回调), 回调中调 Stop (持 mu_)
        // 锁序: frame_mutex_ → mu_ (允许)
        // 但如果线程A 持 mu_ 等 frame_mutex_, 线程B 持 frame_mutex_ 等 mu_ → 死锁
        std::this_thread::sleep_for(std::chrono::seconds(3));
        stop.store(true);
        t1.join();

        printf("  survived\n"); fflush(stdout);
        CalcSessionDestroy(g_session);
        g_session = nullptr;
    }

    printf("\n=== FINAL SCORE: %d/1 ===\n", score); fflush(stdout);
    return 0;
}
