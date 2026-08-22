// shm_leak_probe.cpp — 复现 X11 共享内存段泄漏缺陷
// 测试 xvfb_platform.cpp 中 ShmState::Release() 是否存在共享内存段泄漏
#include <base/abi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>

static int s_frames = 0;
static long long s_last_frame_ms = 0;
static long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static void OnFrame(const uint8_t* data, int32_t w, int32_t h, int32_t rp,
                    int32_t size, int32_t format, void* opaque) {
    (void)data; (void)w; (void)h; (void)rp; (void)size; (void)format; (void)opaque;
    s_frames++;
    s_last_frame_ms = NowMs();
}

// 检查当前进程的共享内存段使用情况
int check_shm_segments() {
    int count = 0;
    FILE* fp = fopen("/proc/sysvipc/shm", "r");
    if (!fp) return -1;
    
    char line[256];
    int my_pid = getpid();
    while (fgets(line, sizeof(line), fp)) {
        int key, shmid, perms, size, cpid, lpid;
        if (sscanf(line, "%d %d %o %d %d %d", &key, &shmid, &perms, &size, &cpid, &lpid) == 6) {
            if (cpid == my_pid || lpid == my_pid) {
                count++;
                printf("  Found shm segment: key=%d shmid=%d size=%d cpid=%d lpid=%d\n", 
                       key, shmid, size, cpid, lpid);
            }
        }
    }
    fclose(fp);
    return count;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <xlsx> [cycles=10]\n", argv[0]);
        return 2;
    }
    int cycles = (argc >= 3) ? atoi(argv[2]) : 10;
    
    printf("=== X11 共享内存段泄漏探针 ===\n");
    printf("开始前检查共享内存段:\n");
    int initial_shm = check_shm_segments();
    printf("初始共享内存段数量: %d\n", initial_shm);
    
    for (int cycle = 1; cycle <= cycles; cycle++) {
        printf("\n=== 第 %d 轮测试 ===\n", cycle);
        
        void* s = CalcSessionCreate(argv[1], "", "shm_leak_probe", OnFrame, nullptr, 1920, 1080);
        if (!s) { 
            fprintf(stderr, "Create FAILED\n"); 
            return 1; 
        }
        
        printf("Create OK\n");
        CalcSessionStart(s);
        
        // 运行一段时间产生一些帧
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        printf("运行2秒后帧数: %d\n", s_frames);
        
        // 检查共享内存段
        int current_shm = check_shm_segments();
        printf("当前共享内存段数量: %d\n", current_shm);
        
        CalcSessionStop(s);
        CalcSessionDestroy(s);
        
        printf("Destroy 完成\n");
        std::this_thread::sleep_for(std::chrono::seconds(1)); // 等待清理
        
        // 检查销毁后的共享内存段
        int after_shm = check_shm_segments();
        printf("销毁后共享内存段数量: %d\n", after_shm);
        
        if (after_shm > initial_shm) {
            printf(">>> 检测到共享内存段泄漏! 初始=%d, 当前=%d, 泄漏=%d\n", 
                   initial_shm, after_shm, after_shm - initial_shm);
        }
        
        s_frames = 0;
    }
    
    printf("\n=== 最终检查 ===\n");
    int final_shm = check_shm_segments();
    printf("最终共享内存段数量: %d\n", final_shm);
    
    if (final_shm > initial_shm) {
        printf(">>> 确认存在共享内存段泄漏! 初始=%d, 最终=%d, 总泄漏=%d\n", 
               initial_shm, final_shm, final_shm - initial_shm);
        return 1;
    } else {
        printf("未检测到共享内存段泄漏\n");
        return 0;
    }
}