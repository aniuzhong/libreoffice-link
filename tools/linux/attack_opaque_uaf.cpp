// attack_opaque_uaf.cpp — opaque 悬垂指针攻击
// Destroy 后上层释放 opaque, 但帧泵可能还在推帧 → cb_ 收到悬垂指针
#include <base/abi.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

static std::atomic<int> g_cb_count{0};
static std::atomic<bool> g_opaque_freed{false};

struct OpaqueData {
    int value;
    char data[64];
};

static void OnFrameOpaque(const uint8_t* d, int32_t w, int32_t h, int32_t rp,
                          int32_t s, int32_t f, void* o) {
    (void)d; (void)w; (void)h; (void)rp; (void)s; (void)f;
    g_cb_count.fetch_add(1);
    // 访问 opaque — 如果已被释放就是 UAF
    if (o) {
        volatile OpaqueData* od = static_cast<OpaqueData*>(o);
        volatile int v = od->value;  // 读已释放内存
        (void)v;
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) return 2;
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int score = 0;

    printf("=== OPAQUE UAF ATTACK ===\n"); fflush(stdout);

    // 攻击: opaque 指向栈变量, Destroy 后栈变量失效
    printf("--- A1: opaque points to stack, Destroy then stack unwinds ---\n"); fflush(stdout);
    {
        OpaqueData od;
        od.value = 42;
        void* s = CalcSessionCreate(xlsx, "", "opaque", OnFrameOpaque, &od, 1920, 1080);
        if (s) {
            CalcSessionStart(s);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            CalcSessionDestroy(s);
            // od 离开作用域 → 栈内存释放, 但帧泵可能还在推帧
            printf("  destroyed, stack about to unwind\n"); fflush(stdout);
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    printf("  survived\n"); fflush(stdout);

    // 攻击2: opaque 指向堆内存, Destroy 后 free, 帧泵继续推帧
    printf("--- A2: opaque points to heap, free after Destroy ---\n"); fflush(stdout);
    OpaqueData* odp = new OpaqueData();
    odp->value = 99;
    void* s2 = CalcSessionCreate(xlsx, "", "opaque", OnFrameOpaque, odp, 1920, 1080);
    if (s2) {
        CalcSessionStart(s2);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CalcSessionDestroy(s2);
        delete odp;  // 释放 opaque, 但帧泵可能还在推帧
        odp = nullptr;
        printf("  freed opaque\n"); fflush(stdout);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    printf("  survived\n"); fflush(stdout);

    printf("\n=== FINAL SCORE: %d/2 ===\n", score); fflush(stdout);
    return 0;
}
