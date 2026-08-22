// attack_abi_sequence.cpp — C ABI 顺序破坏攻击
// 单线程极端顺序调用, 测试每个 ABI 函数在异常状态下的行为
#include <base/abi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

static void OnFrame(const uint8_t*, int32_t, int32_t, int32_t, int32_t, int32_t, void*) {}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) return 2;
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int score = 0;

    printf("=== SEQUENCE ATTACK ===\n"); fflush(stdout);

    // === 攻击 A: Create 后不 Start, 直接调所有 API ===
    printf("--- A: Create only, call all APIs ---\n"); fflush(stdout);
    void* s = CalcSessionCreate(xlsx, "", "seq", OnFrame, nullptr, 1920, 1080);
    if (s) {
        CalcSessionNextPage(s);
        CalcSessionPreviousPage(s);
        CalcSessionUpdateFrame(s);
        CalcSessionSetResolution(s, 800, 600);
        CalcSessionMoveScroll(s, 100, 100);
        CalcSessionSetSheet(s, 0);
        CalcSessionGetSheetCount(s);
        CalcSessionGetCurrentSheet(s);
        CalcSessionGetWidth(s);
        CalcSessionGetHeight(s);
        CalcSessionSetScale(s, 100);
        CalcSessionGetScale(s);
        CalcSessionDestroy(s);
        printf("  Calc OK\n"); fflush(stdout);
    }
    void* p = ImpressSessionCreate(pptx, "", "seq", OnFrame, nullptr, 1920, 1080);
    if (p) {
        ImpressSessionNextPage(p);
        ImpressSessionPreviousPage(p);
        ImpressSessionUpdateFrame(p);
        ImpressSessionSetResolution(p, 800, 600);
        ImpressSessionGetCurrentPage(p);
        ImpressSessionGetPageCount(p);
        ImpressSessionGetWidth(p);
        ImpressSessionGetHeight(p);
        ImpressSessionSetMute(p, 1);
        ImpressSessionDestroy(p);
        printf("  Impress OK\n"); fflush(stdout);
    }

    // === 攻击 B: Start 后不 Stop 直接 Destroy ===
    printf("--- B: Start then Destroy (no Stop) ---\n"); fflush(stdout);
    s = CalcSessionCreate(xlsx, "", "seq", OnFrame, nullptr, 1920, 1080);
    if (s) { CalcSessionStart(s); CalcSessionDestroy(s); printf("  Calc OK\n"); fflush(stdout); }
    p = ImpressSessionCreate(pptx, "", "seq", OnFrame, nullptr, 1920, 1080);
    if (p) { ImpressSessionStart(p); ImpressSessionDestroy(p); printf("  Impress OK\n"); fflush(stdout); }

    // === 攻击 C: Destroy 后立即 Create 同一指针 (堆重用) ===
    printf("--- C: Destroy then immediate Create (heap reuse) ---\n"); fflush(stdout);
    for (int i = 0; i < 500; i++) {
        s = CalcSessionCreate(xlsx, "", "seq", OnFrame, nullptr, 1920, 1080);
        if (s) { CalcSessionStart(s); CalcSessionDestroy(s); }
        p = ImpressSessionCreate(pptx, "", "seq", OnFrame, nullptr, 1920, 1080);
        if (p) { ImpressSessionStart(p); ImpressSessionDestroy(p); }
    }
    printf("  500 cycles OK\n"); fflush(stdout);

    // === 攻击 D: 连续 Start/Stop 5000 次 ===
    printf("--- D: Start/Stop 5000 times ---\n"); fflush(stdout);
    s = CalcSessionCreate(xlsx, "", "seq", OnFrame, nullptr, 1920, 1080);
    if (s) {
        for (int i = 0; i < 5000; i++) { CalcSessionStart(s); CalcSessionStop(s); }
        CalcSessionDestroy(s);
        printf("  Calc OK\n"); fflush(stdout);
    }
    p = ImpressSessionCreate(pptx, "", "seq", OnFrame, nullptr, 1920, 1080);
    if (p) {
        for (int i = 0; i < 5000; i++) { ImpressSessionStart(p); ImpressSessionStop(p); }
        ImpressSessionDestroy(p);
        printf("  Impress OK\n"); fflush(stdout);
    }

    // === 攻击 E: Pause/Resume 5000 次 ===
    printf("--- E: Pause/Resume 5000 times ---\n"); fflush(stdout);
    s = CalcSessionCreate(xlsx, "", "seq", OnFrame, nullptr, 1920, 1080);
    if (s) {
        CalcSessionStart(s);
        for (int i = 0; i < 5000; i++) { CalcSessionPause(s); CalcSessionResume(s); }
        CalcSessionDestroy(s);
        printf("  Calc OK\n"); fflush(stdout);
    }
    p = ImpressSessionCreate(pptx, "", "seq", OnFrame, nullptr, 1920, 1080);
    if (p) {
        ImpressSessionStart(p);
        for (int i = 0; i < 5000; i++) { ImpressSessionPause(p); ImpressSessionResume(p); }
        ImpressSessionDestroy(p);
        printf("  Impress OK\n"); fflush(stdout);
    }

    // === 攻击 F: SetResolution 5000 次 ===
    printf("--- F: SetResolution 5000 times ---\n"); fflush(stdout);
    s = CalcSessionCreate(xlsx, "", "seq", OnFrame, nullptr, 1920, 1080);
    if (s) {
        CalcSessionStart(s);
        for (int i = 0; i < 5000; i++) { CalcSessionSetResolution(s, 640 + (i%5)*320, 480 + (i%5)*240); }
        CalcSessionDestroy(s);
        printf("  Calc OK\n"); fflush(stdout);
    }
    p = ImpressSessionCreate(pptx, "", "seq", OnFrame, nullptr, 1920, 1080);
    if (p) {
        ImpressSessionStart(p);
        for (int i = 0; i < 5000; i++) { ImpressSessionSetResolution(p, 640 + (i%5)*320, 480 + (i%5)*240); }
        ImpressSessionDestroy(p);
        printf("  Impress OK\n"); fflush(stdout);
    }

    // === 攻击 G: 混合调用 ===
    printf("--- G: Mixed calls ---\n"); fflush(stdout);
    s = CalcSessionCreate(xlsx, "", "seq", OnFrame, nullptr, 1920, 1080);
    if (s) {
        CalcSessionStart(s);
        for (int i = 0; i < 2000; i++) {
            CalcSessionNextPage(s);
            CalcSessionPreviousPage(s);
            CalcSessionUpdateFrame(s);
            CalcSessionMoveScroll(s, i % 100, i % 100);
            CalcSessionSetSheet(s, i % 3);
            CalcSessionSetScale(s, 50 + (i % 300));
            CalcSessionSetResolution(s, 640 + (i%5)*320, 480 + (i%5)*240);
        }
        CalcSessionDestroy(s);
        printf("  Calc OK\n"); fflush(stdout);
    }
    p = ImpressSessionCreate(pptx, "", "seq", OnFrame, nullptr, 1920, 1080);
    if (p) {
        ImpressSessionStart(p);
        for (int i = 0; i < 2000; i++) {
            ImpressSessionNextPage(p);
            ImpressSessionPreviousPage(p);
            ImpressSessionUpdateFrame(p);
            ImpressSessionSetResolution(p, 640 + (i%5)*320, 480 + (i%5)*240);
            ImpressSessionSetMute(p, i % 2);
            ImpressSessionGetCurrentPage(p);
            ImpressSessionGetPageCount(p);
        }
        ImpressSessionDestroy(p);
        printf("  Impress OK\n"); fflush(stdout);
    }

    printf("\n=== FINAL SCORE: %d/8 ===\n", score); fflush(stdout);
    return 0;
}
