// attack_semaphore_storm_probe.cpp — 信号量/共享内存破坏攻击
// 直接操作 /dev/shm 下的信号量和 slot 文件，制造跨进程混乱
#include <abi/abi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <semaphore.h>

static std::atomic<long long> s_frames{0};
static void OnFrame(const uint8_t* d, int32_t w, int32_t h, int32_t rp,
                    int32_t s, int32_t f, void* o) {
    s_frames.fetch_add(1, std::memory_order_relaxed);
    (void)d;(void)w;(void)h;(void)rp;(void)s;(void)f;(void)o;
}

// 攻击1: 多进程同时 Create，让 BootLock 信号量混乱
void attack_multi_proc_bootlock(const char* xlsx, const char* pptx) {
    printf("[SEM-1] Multi-proc BootLock storm\n"); fflush(stdout);
    std::vector<pid_t> pids;
    for (int i = 0; i < 16; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            for (int j = 0; j < 20; j++) {
                void* s = (j % 2 == 0)
                    ? CalcSessionCreate(xlsx, "", "attack_sem", OnFrame, nullptr, 1920, 1080)
                    : ImpressSessionCreate(pptx, "", "attack_sem", OnFrame, nullptr, 1920, 1080);
                if (!s) continue;
                if (j % 2 == 0) {
                    CalcSessionStart(s);
                    usleep(5000);
                    CalcSessionDestroy(s);
                } else {
                    ImpressSessionStart(s);
                    usleep(5000);
                    ImpressSessionDestroy(s);
                }
            }
            _exit(0);
        } else if (pid > 0) {
            pids.push_back(pid);
        }
    }
    int crashed = 0;
    for (pid_t pid : pids) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status)) crashed++;
    }
    printf("[SEM-1] done, %d children crashed\n", crashed); fflush(stdout);
}

// 攻击2: 先占满 8 slot，然后多进程并发 Create（slot 耗尽 + BootLock 双重压力）
void attack_slot_exhaust_fork(const char* xlsx) {
    printf("[SEM-2] Slot exhaust + fork storm\n"); fflush(stdout);
    // 先占满 slot
    std::vector<void*> holders;
    for (int i = 0; i < 8; i++) {
        void* s = CalcSessionCreate(xlsx, "", "attack_sem", OnFrame, nullptr, 1920, 1080);
        if (s) { CalcSessionStart(s); holders.push_back(s); }
    }
    printf("[SEM-2] holding %zu slots\n", holders.size()); fflush(stdout);

    // fork 子进程并发 Create（slot 耗尽）
    std::vector<pid_t> pids;
    for (int i = 0; i < 8; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            for (int j = 0; j < 30; j++) {
                void* s = CalcSessionCreate(xlsx, "", "attack_sem", OnFrame, nullptr, 1920, 1080);
                if (s) CalcSessionDestroy(s);
                usleep(2000);
            }
            _exit(0);
        } else if (pid > 0) {
            pids.push_back(pid);
        }
    }

    int crashed = 0;
    for (pid_t pid : pids) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status)) crashed++;
    }
    for (auto s : holders) CalcSessionDestroy(s);
    printf("[SEM-2] done, %d children crashed\n", crashed); fflush(stdout);
}

// 攻击3: 父进程创建后 fork，子进程 Destroy 父进程的 session
void attack_fork_child_destroy_parent_session(const char* xlsx) {
    printf("[SEM-3] Fork child destroys parent session\n"); fflush(stdout);
    void* s = CalcSessionCreate(xlsx, "", "attack_sem", OnFrame, nullptr, 1920, 1080);
    if (!s) { printf("create failed\n"); return; }
    CalcSessionStart(s);

    pid_t pid = fork();
    if (pid == 0) {
        // 子进程 Destroy 父进程的 session
        CalcSessionDestroy(s);
        _exit(0);
    }

    // 父进程继续操作（session 已被子进程 Destroy）
    for (int i = 0; i < 50; i++) {
        CalcSessionNextPage(s);
        CalcSessionUpdateFrame(s);
        usleep(10000);
    }

    int status;
    waitpid(pid, &status, 0);
    int crashed = WIFSIGNALED(status);
    // 父进程再 Destroy（可能 double free）
    CalcSessionDestroy(s);
    printf("[SEM-3] done, child crashed=%d\n", crashed); fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <xlsx> <pptx> [att=1|2|3|all]\n", argv[0]);
        return 2;
    }
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int att = (argc >= 4 && strcmp(argv[3], "all") == 0) ? 0 : atoi(argv[3]);
    int score = 0;

    printf("=== SEMAPHORE STORM ATTACK ===\n"); fflush(stdout);
    if (att == 0 || att == 1) { try { attack_multi_proc_bootlock(xlsx, pptx); } catch (...) { printf(">>> SEM-1 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 2) { try { attack_slot_exhaust_fork(xlsx); } catch (...) { printf(">>> SEM-2 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 3) { try { attack_fork_child_destroy_parent_session(xlsx); } catch (...) { printf(">>> SEM-3 CRASH (1pt)\n"); score++; } }
    printf("=== SEM FINAL SCORE: %d/3 ===\n", score); fflush(stdout);
    return 0;
}
