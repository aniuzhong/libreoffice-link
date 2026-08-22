// uno_null_probe.cpp — 复现 UNO 对象未检查缺陷
// 测试 calc_session.cpp 中 ScrollPage() 的 pane_ 空指针解引用问题
#include <base/abi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

static int s_frames = 0;

static void OnFrame(const uint8_t* data, int32_t w, int32_t h, int32_t rp,
                    int32_t size, int32_t format, void* opaque) {
    (void)data; (void)w; (void)h; (void)rp; (void)size; (void)format; (void)opaque;
    s_frames++;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <xlsx>\n", argv[0]);
        return 2;
    }
    
    printf("=== UNO 对象未检查缺陷探针 ===\n");
    
    void* s = CalcSessionCreate(argv[1], "", "uno_null_probe", OnFrame, nullptr, 1920, 1080);
    if (!s) { 
        fprintf(stderr, "Create FAILED\n"); 
        return 1; 
    }
    
    printf("Create OK\n");
    CalcSessionStart(s);
    
    // 等待一些帧
    std::this_thread::sleep_for(std::chrono::seconds(2));
    printf("Initial frames: %d\n", s_frames);
    
    // 测试 ScrollPage 在正常情况下的行为
    printf("测试正常 ScrollPage 调用...\n");
    bool ok = CalcSessionNextPage(s);
    printf("NextPage result: %d\n", ok ? 1 : 0);
    
    // 测试 PreviousPage
    printf("测试 PreviousPage 调用...\n");
    ok = CalcSessionPreviousPage(s);
    printf("PreviousPage result: %d\n", ok ? 1 : 0);
    
    // 测试各种滚动操作
    printf("测试各种滚动操作...\n");
    for (int i = 0; i < 5; i++) {
        CalcSessionNextPage(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    printf("测试完成，最终帧数: %d\n", s_frames);
    
    CalcSessionStop(s);
    CalcSessionDestroy(s);
    printf("done\n");
    return 0;
}