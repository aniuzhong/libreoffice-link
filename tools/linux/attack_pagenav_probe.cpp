// attack_pagenav_probe.cpp — 翻页/导航越界与状态破坏攻击探针
// 已覆盖：暂停/恢复状态（attack_protocol）、乱序调用（attack_state_corruption）。
// 未覆盖：impress gotoSlideIndex 越界（负/超界）、stopped 态 goto、NextPage
// 越过末尾、并发翻页；calc ScrollPage 在极端表/降序表下的越界。目标：崩溃/卡死 = 1 分。
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

// 攻击1: impress 越界 goto + 越界 NextPage + stopped 态 goto
void attack_impress_nav_oob(const char* pptx) {
    printf("[NAV-1] impress out-of-bound goto/next\n"); fflush(stdout);
    void* p = ImpressSessionCreate(pptx, "", "attack_pagenav", OnFrame, nullptr, 1920, 1080);
    if (!p) { printf("[NAV-1] create failed\n"); return; }
    ImpressSessionStart(p);
    int n = ImpressSessionGetPageCount(p);
    printf("[NAV-1] page_count=%d\n", n); fflush(stdout);
    // 越界索引：负、超界、INT_MAX
    for (int g : {-1, -100, n, n + 1, 100000, 2147483647}) {
        ImpressSessionGoToPage(p, g);
        ImpressSessionUpdateFrame(p);
    }
    // 末尾之后继续 NextPage
    for (int i = 0; i < n + 20; i++) ImpressSessionNextPage(p);
    // stopped 态翻页
    ImpressSessionStop(p);
    ImpressSessionGoToPage(p, 0);
    ImpressSessionNextPage(p);
    ImpressSessionPreviousPage(p);
    ImpressSessionStart(p);
    // 并发翻页
    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; t++)
        ts.emplace_back([&]() {
            while (!stop.load()) {
                ImpressSessionNextPage(p);
                ImpressSessionPreviousPage(p);
                ImpressSessionGoToPage(p, rand() % (n > 0 ? n : 1));
            }
        });
    std::this_thread::sleep_for(std::chrono::seconds(4));
    stop.store(true);
    for (auto& x : ts) x.join();
    ImpressSessionDestroy(p);
    printf("[NAV-1] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

// 攻击2: calc 切表越界 + 降序表滚动 + 极端缩放
void attack_calc_nav_oob(const char* xlsx) {
    printf("[NAV-2] calc sheet/scroll/scale oob\n"); fflush(stdout);
    void* c = CalcSessionCreate(xlsx, "", "attack_pagenav", OnFrame, nullptr, 1920, 1080);
    if (!c) { printf("[NAV-2] create failed\n"); return; }
    CalcSessionStart(c);
    unsigned sheets = CalcSessionGetSheetCount(c);
    printf("[NAV-2] sheets=%u\n", sheets); fflush(stdout);
    for (unsigned i = 0; i < sheets + 20; i++) {
        CalcSessionSetSheet(c, i);                 // 越界切表
        CalcSessionNextPage(c);
        CalcSessionPreviousPage(c);
    }
    // 极端缩放 0 / 超大 / 400
    for (unsigned sc : {0u, 1u, 400u, 1000u, 0xFFFFFFFFu}) {
        CalcSessionSetScale(c, sc);
        CalcSessionUpdateFrame(c);
    }
    // 大量滚动越界
    for (int i = 0; i < 500; i++) {
        CalcSessionMoveScroll(c, 0, 1000000);   // 远超文档
        CalcSessionMoveScroll(c, 0, -1000000);
    }
    CalcSessionDestroy(c);
    printf("[NAV-2] done\n"); fflush(stdout);
}

// 攻击3: impress goto 与 slideshow pause/resume 并发（gotoSlideIndex 在 pause 态语义）
void attack_nav_pause_race(const char* pptx) {
    printf("[NAV-3] goto vs pause/resume race\n"); fflush(stdout);
    void* p = ImpressSessionCreate(pptx, "", "attack_pagenav", OnFrame, nullptr, 1920, 1080);
    if (!p) { printf("[NAV-3] create failed\n"); return; }
    ImpressSessionStart(p);
    std::atomic<bool> stop{false};
    std::thread nav([&]() {
        int n = ImpressSessionGetPageCount(p);
        while (!stop.load()) {
            ImpressSessionGoToPage(p, rand() % (n > 0 ? n : 1));
            ImpressSessionNextPage(p);
        }
    });
    std::thread ctrl([&]() {
        while (!stop.load()) {
            ImpressSessionPause(p);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ImpressSessionResume(p);
        }
    });
    std::thread upd([&]() {
        while (!stop.load()) ImpressSessionUpdateFrame(p);
    });
    std::this_thread::sleep_for(std::chrono::seconds(5));
    stop.store(true);
    nav.join(); ctrl.join(); upd.join();
    ImpressSessionDestroy(p);
    printf("[NAV-3] done frames=%lld\n", s_frames.load()); fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) { fprintf(stderr, "usage: %s <xlsx> <pptx> [att=1|2|3|all]\n", argv[0]); return 2; }
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int att = (argc >= 4 && strcmp(argv[3], "all") == 0) ? 0 : atoi(argv[3]);
    int score = 0;
    printf("=== PAGE-NAV ATTACK ===\n"); fflush(stdout);
    if (att == 0 || att == 1) { try { attack_impress_nav_oob(pptx); } catch (...) { printf(">>> NAV-1 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 2) { try { attack_calc_nav_oob(xlsx); } catch (...) { printf(">>> NAV-2 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 3) { try { attack_nav_pause_race(pptx); } catch (...) { printf(">>> NAV-3 CRASH (1pt)\n"); score++; } }
    printf("=== NAV FINAL SCORE: %d/3 ===\n", score); fflush(stdout);
    return 0;
}
