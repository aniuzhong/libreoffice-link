// attack_cross_process_probe.cpp — 跨进程协调攻击探针
// 目标：破坏跨进程协调机制导致卡死/崩溃
#include <abi/abi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

static void OnFrame(const uint8_t* data, int32_t w, int32_t h, int32_t rp,
                    int32_t size, int32_t format, void* opaque) {
    (void)data; (void)w; (void)h; (void)rp; (void)size; (void)format; (void)opaque;
}

// 攻击1: 多进程并发Create攻击（跨进程BootLock竞态）
void attack_multi_process_create_race(const char* xlsx, const char* pptx, int process_count) {
    printf("Starting multi-process create race attack (%d processes)\n", process_count);
    fflush(stdout);
    
    std::vector<pid_t> pids;
    
    for (int i = 0; i < process_count; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            // 子进程
            printf("Child process %d starting\n", i);
            fflush(stdout);
            
            void* calc_s = CalcSessionCreate(xlsx, "", "attack_cross_process_probe", OnFrame, nullptr, 1920, 1080);
            if (calc_s) {
                CalcSessionStart(calc_s);
                sleep(1);
                CalcSessionStop(calc_s);
                CalcSessionDestroy(calc_s);
            }
            
            void* impress_s = ImpressSessionCreate(pptx, "", "attack_cross_process_probe", OnFrame, nullptr, 1920, 1080);
            if (impress_s) {
                ImpressSessionStart(impress_s);
                sleep(1);
                ImpressSessionStop(impress_s);
                ImpressSessionDestroy(impress_s);
            }
            
            printf("Child process %d completed\n", i);
            fflush(stdout);
            exit(0);
        } else if (pid > 0) {
            pids.push_back(pid);
        } else {
            printf("Fork failed\n");
        }
    }
    
    // 等待所有子进程
    int status;
    for (pid_t pid : pids) {
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status)) {
            printf("Process %d terminated by signal %d\n", pid, WTERMSIG(status));
        }
    }
    
    printf("Multi-process race attack completed\n");
    fflush(stdout);
}

// 攻击2: 进程间资源争用攻击
void attack_inter_process_resource_contention(const char* xlsx, const char* pptx, int process_count) {
    printf("Starting inter-process resource contention attack (%d processes)\n", process_count);
    fflush(stdout);
    
    std::vector<pid_t> pids;
    
    for (int i = 0; i < process_count; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            // 子进程：连续创建销毁会话
            for (int j = 0; j < 50; j++) {
                void* calc_s = CalcSessionCreate(xlsx, "", "attack_cross_process_probe", OnFrame, nullptr, 1920, 1080);
                if (calc_s) {
                    CalcSessionStart(calc_s);
                    usleep(10000); // 10ms
                    CalcSessionStop(calc_s);
                    CalcSessionDestroy(calc_s);
                }
                
                void* impress_s = ImpressSessionCreate(pptx, "", "attack_cross_process_probe", OnFrame, nullptr, 1920, 1080);
                if (impress_s) {
                    ImpressSessionStart(impress_s);
                    usleep(10000);
                    ImpressSessionStop(impress_s);
                    ImpressSessionDestroy(impress_s);
                }
            }
            exit(0);
        } else if (pid > 0) {
            pids.push_back(pid);
        }
    }
    
    // 等待所有子进程
    int status;
    for (pid_t pid : pids) {
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status)) {
            printf("Process %d terminated by signal %d (CRASH!)\n", pid, WTERMSIG(status));
        }
    }
    
    printf("Inter-process resource contention attack completed\n");
    fflush(stdout);
}

// 攻击3: 共享资源破坏攻击（slot/信号量争用）
void attack_shared_resource_corruption(const char* xlsx, const char* pptx) {
    printf("Starting shared resource corruption attack\n");
    fflush(stdout);
    
    // 快速连续创建会话，争用slot资源
    std::vector<void*> sessions;
    
    for (int i = 0; i < 100; i++) {
        void* calc_s = CalcSessionCreate(xlsx, "", "attack_cross_process_probe", OnFrame, nullptr, 1920, 1080);
        if (calc_s) {
            CalcSessionStart(calc_s);
            sessions.push_back(calc_s);
        }
        
        void* impress_s = ImpressSessionCreate(pptx, "", "attack_cross_process_probe", OnFrame, nullptr, 1920, 1080);
        if (impress_s) {
            ImpressSessionStart(impress_s);
            sessions.push_back(impress_s);
        }
        
        if (i % 10 == 0) {
            printf("Created %d sessions\n", (int)sessions.size());
            fflush(stdout);
        }
    }
    
    printf("Holding %zu sessions to test shared resource limits\n", sessions.size());
    sleep(10);
    
    // 快速销毁，测试资源回收
    for (auto s : sessions) {
        CalcSessionStop(s);
        CalcSessionDestroy(s);
    }
    
    printf("Shared resource corruption attack completed\n");
    fflush(stdout);
}

// 攻击4: Xvfb进程攻击（尝试干扰Xvfb）
void attack_xvfb_process_interference(const char* xlsx, const char* pptx) {
    printf("Starting Xvfb process interference attack\n");
    fflush(stdout);
    
    // 创建会话后持续操作，可能干扰Xvfb
    void* calc_s = CalcSessionCreate(xlsx, "", "attack_cross_process_probe", OnFrame, nullptr, 1920, 1080);
    void* impress_s = ImpressSessionCreate(pptx, "", "attack_cross_process_probe", OnFrame, nullptr, 1920, 1080);
    
    if (!calc_s || !impress_s) {
        printf("Setup failed\n");
        return;
    }
    
    CalcSessionStart(calc_s);
    ImpressSessionStart(impress_s);
    
    // 持续页面操作，可能触发Xvfb相关bug
    for (int i = 0; i < 10000; i++) {
        CalcSessionNextPage(calc_s);
        ImpressSessionNextPage(impress_s);
        CalcSessionUpdateFrame(calc_s);
        ImpressSessionUpdateFrame(impress_s);
        
        if (i % 1000 == 0) {
            printf("Interference iteration %d\n", i);
            fflush(stdout);
        }
        
        usleep(1000); // 1ms
    }
    
    CalcSessionStop(calc_s);
    ImpressSessionStop(impress_s);
    CalcSessionDestroy(calc_s);
    ImpressSessionDestroy(impress_s);
    
    printf("Xvfb process interference attack completed\n");
    fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <xlsx> <pptx> [attack_type=1|2|3|4|all] [param=8]\n", argv[0]);
        fprintf(stderr, "  attack_type: 1=multi-process race, 2=inter-process contention, 3=shared resource, 4=Xvfb interference, all=all\n");
        fprintf(stderr, "  param: process count for type1/2\n");
        return 2;
    }
    
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int attack_type = (argc >= 4) ? 
        (strcmp(argv[3], "all") == 0 ? 4 : atoi(argv[3])) : 1;
    int param = (argc >= 5) ? atoi(argv[4]) : 8;
    
    printf("=== CROSS-PROCESS ATTACK PROBE ===\n");
    printf("xlsx=%s pptx=%s attack_type=%d param=%d\n", xlsx, pptx, attack_type, param);
    fflush(stdout);
    
    int score = 0;
    
    // 攻击1: 多进程并发
    if (attack_type == 1 || attack_type == 4) {
        printf("\n--- Attack 1: Multi-Process Create Race ---\n");
        fflush(stdout);
        
        try {
            attack_multi_process_create_race(xlsx, pptx, param);
            printf(">>> Attack 1: No crash (0 point)\n");
        } catch (...) {
            printf(">>> ATTACK 1 SUCCESS: Exception/crash detected (1 point)\n");
            score++;
        }
        fflush(stdout);
    }
    
    // 攻击2: 进程间资源争用
    if (attack_type == 2 || attack_type == 4) {
        printf("\n--- Attack 2: Inter-Process Resource Contention ---\n");
        fflush(stdout);
        
        try {
            attack_inter_process_resource_contention(xlsx, pptx, param);
            printf(">>> Attack 2: No crash (0 point)\n");
        } catch (...) {
            printf(">>> ATTACK 2 SUCCESS: Exception/crash detected (1 point)\n");
            score++;
        }
        fflush(stdout);
    }
    
    // 攻击3: 共享资源破坏
    if (attack_type == 3 || attack_type == 4) {
        printf("\n--- Attack 3: Shared Resource Corruption ---\n");
        fflush(stdout);
        
        try {
            attack_shared_resource_corruption(xlsx, pptx);
            printf(">>> Attack 3: No crash (0 point)\n");
        } catch (...) {
            printf(">>> ATTACK 3 SUCCESS: Exception/crash detected (1 point)\n");
            score++;
        }
        fflush(stdout);
    }
    
    // 攻击4: Xvfb进程干扰
    if (attack_type == 4) {
        printf("\n--- Attack 4: Xvfb Process Interference ---\n");
        fflush(stdout);
        
        try {
            attack_xvfb_process_interference(xlsx, pptx);
            printf(">>> Attack 4: No crash (0 point)\n");
        } catch (...) {
            printf(">>> ATTACK 4 SUCCESS: Exception/crash detected (1 point)\n");
            score++;
        }
        fflush(stdout);
    }
    
    printf("\n=== FINAL SCORE: %d/4 ===\n", score);
    printf("Note: Each crash/deadlock = 1 point\n");
    fflush(stdout);
    
    return 0;
}
