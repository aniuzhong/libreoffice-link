// attack_shared_resource.cpp — 共享资源耗尽攻击
// 攻击 office_runtime 的跨进程资源: Xvfb 显示号、slot shm、BootLock 信号量
#include <base/abi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/sem.h>
#include <unistd.h>

static void OnFrame(const uint8_t*, int32_t, int32_t, int32_t, int32_t, int32_t, void*) {}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) return 2;
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int score = 0;

    printf("=== SHARED RESOURCE ATTACK ===\n"); fflush(stdout);

    // 攻击1: 破坏 slot shm 文件 (写入垃圾数据)
    printf("--- A: Corrupt slot shm ---\n"); fflush(stdout);
    int fd = shm_open("/nova_office_slots_v1", O_RDWR, 0666);
    if (fd >= 0) {
        // 写入全 0xFF 破坏 slot 位图
        for (int i = 0; i < 128; i++) {
            uint8_t junk = 0xFF;
            write(fd, &junk, 1);
        }
        close(fd);
        printf("  shm corrupted\n"); fflush(stdout);
    } else {
        printf("  shm not found (will be created on first Create)\n"); fflush(stdout);
    }

    // 尝试 Create (可能因 shm 损坏而崩溃)
    void* s = CalcSessionCreate(xlsx, "", "res", OnFrame, nullptr, 1920, 1080);
    if (s) { printf("  Create OK (shm corruption survived)\n"); fflush(stdout); CalcSessionDestroy(s); }
    else { printf("  Create FAILED (shm corruption detected?)\n"); fflush(stdout); }

    // 攻击2: 破坏 BootLock 信号量
    printf("--- B: Corrupt BootLock semaphore ---\n"); fflush(stdout);
    sem_t* boot_sem = sem_open("/nova_office_boot", O_RDWR);
    if (boot_sem != SEM_FAILED) {
        // 消耗信号量 (让后续 Create 卡住或超时)
        for (int i = 0; i < 10; i++) {
            struct timespec ts = {1, 0};
            sem_timedwait(boot_sem, &ts);
        }
        sem_close(boot_sem);
        printf("  semaphore consumed\n"); fflush(stdout);
    } else {
        printf("  sem not found\n"); fflush(stdout);
    }

    s = CalcSessionCreate(xlsx, "", "res", OnFrame, nullptr, 1920, 1080);
    if (s) { printf("  Create OK (sem corruption survived)\n"); fflush(stdout); CalcSessionDestroy(s); }
    else { printf("  Create FAILED\n"); fflush(stdout); }

    // 攻击3: 占用所有 Xvfb 显示号 (90-99)
    printf("--- C: Exhaust Xvfb display numbers ---\n"); fflush(stdout);
    // 先正常 Create 一个 session 启动 Xvfb
    s = CalcSessionCreate(xlsx, "", "res", OnFrame, nullptr, 1920, 1080);
    if (s) {
        CalcSessionStart(s);
        // 伪造 lock 文件占用其他显示号
        for (int d = 91; d <= 99; d++) {
            char lock_path[64];
            snprintf(lock_path, sizeof(lock_path), "/tmp/.X%d-lock", d);
            FILE* f = fopen(lock_path, "w");
            if (f) { fprintf(f, "%10d\n", 999999); fclose(f); }
            char sock_dir[64];
            snprintf(sock_dir, sizeof(sock_dir), "/tmp/.X11-unix");
            mkdir(sock_dir, 0777);
            char sock_path[64];
            snprintf(sock_path, sizeof(sock_path), "/tmp/.X11-unix/X%d", d);
            f = fopen(sock_path, "w");
            if (f) fclose(f);
        }
        printf("  display 91-99 locked\n"); fflush(stdout);
        CalcSessionDestroy(s);
    }

    // 再 Create (应该只能用到 :90, 但 :90 已被上一个 session 释放)
    s = CalcSessionCreate(xlsx, "", "res", OnFrame, nullptr, 1920, 1080);
    if (s) { printf("  Create OK (display exhaustion survived)\n"); fflush(stdout); CalcSessionDestroy(s); }
    else { printf("  Create FAILED\n"); fflush(stdout); }

    printf("\n=== FINAL SCORE: %d/3 ===\n", score); fflush(stdout);
    return 0;
}
