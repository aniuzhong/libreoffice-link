// attack_fork_bomb_probe.cpp — fork 炸弹 + 共享内核破坏
// fork 后子进程继承 UNO 引用，操作破坏父进程状态
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
#include <signal.h>

static std::atomic<long long> s_frames{0};
static void OnFrame(const uint8_t* d, int32_t w, int32_t h, int32_t rp,
                    int32_t s, int32_t f, void* o) {
    s_frames.fetch_add(1, std::memory_order_relaxed);
    (void)d;(void)w;(void)h;(void)rp;(void)s;(void)f;(void)o;
}

// 攻击1: fork 后子进程创建会话，父进程同时操作
void attack_fork_child_create(const char* xlsx) {
    printf("[FORK-1] Fork child create\n"); fflush(stdout);
    // 父进程先创建一个会话
    void* parent = CalcSessionCreate(xlsx, "", "attack_fork", OnFrame, nullptr, 1920, 1080);
    if (!parent) { printf("parent create failed\n"); return; }
    CalcSessionStart(parent);

    int crashed = 0;
    for (int i = 0; i < 10; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            // 子进程：创建自己的会话
            void* s = CalcSessionCreate(xlsx, "", "attack_fork", OnFrame, nullptr, 1920, 1080);
            if (s) {
                CalcSessionStart(s);
                for (int j = 0; j < 10; j++) {
                    CalcSessionNextPage(s);
                    CalcSessionUpdateFrame(s);
                }
                CalcSessionDestroy(s);
            }
            _exit(0);
        }
    }

    // 父进程持续操作
    for (int i = 0; i < 100; i++) {
        CalcSessionNextPage(parent);
        CalcSessionUpdateFrame(parent);
        usleep(10000);
    }

    for (int i = 0; i < 10; i++) {
        int status;
        wait(&status);
        if (WIFSIGNALED(status)) { crashed++; }
    }
    CalcSessionDestroy(parent);
    printf("[FORK-1] done, %d children crashed\n", crashed); fflush(stdout);
}

// 攻击2: fork 后子进程直接操作父进程的 session 指针
void attack_fork_share_session(const char* xlsx) {
    printf("[FORK-2] Fork share session pointer\n"); fflush(stdout);
    void* s = CalcSessionCreate(xlsx, "", "attack_fork", OnFrame, nullptr, 1920, 1080);
    if (!s) { printf("create failed\n"); return; }
    CalcSessionStart(s);

    pid_t pid = fork();
    if (pid == 0) {
        // 子进程操作父进程的 session（共享内存空间）
        for (int i = 0; i < 50; i++) {
            CalcSessionNextPage(s);
            CalcSessionUpdateFrame(s);
            usleep(5000);
        }
        _exit(0);
    }

    // 父进程同时操作同一 session
    for (int i = 0; i < 50; i++) {
        CalcSessionNextPage(s);
        CalcSessionUpdateFrame(s);
        usleep(5000);
    }

    int status;
    waitpid(pid, &status, 0);
    int crashed = WIFSIGNALED(status);
    CalcSessionDestroy(s);
    printf("[FORK-2] done, child crashed=%d\n", crashed); fflush(stdout);
}

// 攻击3: fork 后父进程 Destroy，子进程继续操作
void attack_fork_destroy_in_parent(const char* xlsx) {
    printf("[FORK-3] Fork then parent destroys while child operates\n"); fflush(stdout);
    void* s = CalcSessionCreate(xlsx, "", "attack_fork", OnFrame, nullptr, 1920, 1080);
    if (!s) { printf("create failed\n"); return; }
    CalcSessionStart(s);

    pid_t pid = fork();
    if (pid == 0) {
        // 子进程持续操作
        for (int i = 0; i < 100; i++) {
            CalcSessionNextPage(s);
            CalcSessionUpdateFrame(s);
            usleep(2000);
        }
        _exit(0);
    }

    // 父进程立即销毁
    usleep(50000);
    CalcSessionDestroy(s);

    int status;
    waitpid(pid, &status, 0);
    int crashed = WIFSIGNALED(status);
    printf("[FORK-3] done, child crashed=%d\n", crashed); fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <xlsx> [att=1|2|3|all]\n", argv[0]);
        return 2;
    }
    const char* xlsx = argv[1];
    int att = (argc >= 3 && strcmp(argv[2], "all") == 0) ? 0 : atoi(argv[2]);
    int score = 0;

    printf("=== FORK BOMB ATTACK ===\n"); fflush(stdout);
    if (att == 0 || att == 1) { try { attack_fork_child_create(xlsx); } catch (...) { printf(">>> FORK-1 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 2) { try { attack_fork_share_session(xlsx); } catch (...) { printf(">>> FORK-2 CRASH (1pt)\n"); score++; } }
    if (att == 0 || att == 3) { try { attack_fork_destroy_in_parent(xlsx); } catch (...) { printf(">>> FORK-3 CRASH (1pt)\n"); score++; } }
    printf("=== FORK FINAL SCORE: %d/3 ===\n", score); fflush(stdout);
    return 0;
}
