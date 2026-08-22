// static_race_probe.cpp — 复现静态变量竞态条件缺陷
// 测试 calc_session.cpp 中 PushFrame() 的静态变量 sum_us/count 是否存在竞态
#include <abi/abi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>
#include <unistd.h>

static int s_frames = 0;
static std::atomic<bool> s_done{false};

static void OnFrame(const uint8_t* data, int32_t w, int32_t h, int32_t rp,
                    int32_t size, int32_t format, void* opaque) {
    (void)data; (void)w; (void)h; (void)rp; (void)size; (void)format; (void)opaque;
    s_frames++;
}

// 多线程同时调用 UpdateFrame，测试静态变量竞态
void thread_func(void* session, int thread_id) {
    for (int i = 0; i < 100; i++) {
        CalcSessionUpdateFrame(session);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    printf("Thread %d completed\n", thread_id);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <xlsx> [threads=4]\n", argv[0]);
        return 2;
    }
    int num_threads = (argc >= 3) ? atoi(argv[2]) : 4;
    
    void* s = CalcSessionCreate(argv[1], "", "static_race_probe", OnFrame, nullptr, 1920, 1080);
    if (!s) { 
        fprintf(stderr, "Create FAILED\n"); 
        return 1; 
    }
    
    printf("Create OK, starting with %d threads\n", num_threads);
    CalcSessionStart(s);
    
    // 启动多个线程同时调用 UpdateFrame
    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(thread_func, s, i);
    }
    
    // 等待所有线程完成
    for (auto& t : threads) {
        t.join();
    }
    
    printf("All threads completed, total frames=%d\n", s_frames);
    
    CalcSessionStop(s);
    CalcSessionDestroy(s);
    printf("done\n");
    return 0;
}