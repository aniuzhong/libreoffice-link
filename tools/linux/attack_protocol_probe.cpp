// attack_protocol_probe.cpp — 协议状态攻击探针（简化版）
// 攻击点1: 暂停/恢复状态攻击（经验41：暂停→恢复翻页失效）
// 攻击点2: Start幂等性攻击（经验42：Start未重置paused_）
#include <abi/abi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

static int s_frames = 0;
static bool s_state_corruption = false;
static int s_pause_resume_cycles = 0;

static void OnFrame(const uint8_t* data, int32_t w, int32_t h, int32_t rp,
                    int32_t size, int32_t format, void* opaque) {
    (void)data; (void)w; (void)h; (void)rp; (void)size; (void)format; (void)opaque;
    s_frames++;
}

// 攻击1: 暂停/恢复状态攻击（经验41）
void attack_pause_resume_state(void* session, bool is_impress, int cycles) {
    printf("Starting pause/resume state attack (%d cycles)\n", cycles);
    fflush(stdout);
    
    int frames_before = s_frames;
    
    for (int i = 0; i < cycles; i++) {
        if (is_impress) {
            ImpressSessionPause(session);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ImpressSessionResume(session);
        } else {
            CalcSessionPause(session);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            CalcSessionResume(session);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // 尝试翻页，检测是否卡死
        bool ok;
        if (is_impress) {
            ok = ImpressSessionNextPage(session);
        } else {
            ok = CalcSessionNextPage(session);
        }
        
        if (!ok) {
            printf("Cycle %d: Page operation FAILED after pause/resume - state corruption!\n", i);
            s_state_corruption = true;
            break;
        }
        
        // 检查是否还有帧输出（经验41：暂停恢复后画面冻结）
        int frames_during = s_frames - frames_before;
        if (i > 5 && frames_during == 0) {
            printf("Cycle %d: No frames after pause/resume - likely frame freeze (经验41)\n", i);
            s_state_corruption = true;
            break;
        }
        
        s_pause_resume_cycles++;
        
        if (i % 10 == 0) {
            printf("Progress: %d/%d cycles, frames=%d\n", i, cycles, s_frames);
            fflush(stdout);
        }
    }
    
    printf("Pause/resume attack completed, state_corruption=%d\n", s_state_corruption);
    fflush(stdout);
}

// 攻击2: Start幂等性攻击（经验42：双重Start竞态）
void attack_start_idempotence(void* session, bool is_impress) {
    printf("Starting Start idempotence attack\n");
    fflush(stdout);
    
    // 双重Start测试
    bool first_start = is_impress ? ImpressSessionStart(session) : CalcSessionStart(session);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    bool second_start = is_impress ? ImpressSessionStart(session) : CalcSessionStart(session);
    
    printf("First Start: %s, Second Start: %s\n", 
           first_start ? "OK" : "FAIL", second_start ? "OK" : "FAIL");
    
    // 快速连续Start测试（经验42 P5竞态）
    for (int i = 0; i < 20; i++) {
        bool ok = is_impress ? ImpressSessionStart(session) : CalcSessionStart(session);
        if (!ok) {
            printf("Start iteration %d FAILED - possible race condition (经验42 P5)\n", i);
            s_state_corruption = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    
    printf("Start idempotence attack completed, state_corruption=%d\n", s_state_corruption);
    fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <xlsx> <pptx> [attack_type=1|2|all]\n", argv[0]);
        fprintf(stderr, "  attack_type: 1=pause/resume, 2=start idempotence, all=all\n");
        return 2;
    }
    
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int attack_type = (argc >= 4) ? 
        (strcmp(argv[3], "all") == 0 ? 2 : atoi(argv[3])) : 1;
    
    printf("=== ATTACK PROTOCOL PROBE ===\n");
    printf("xlsx=%s pptx=%s attack_type=%d\n", xlsx, pptx, attack_type);
    fflush(stdout);
    
    int score = 0;
    
    // 创建测试会话
    void* calc_s = CalcSessionCreate(xlsx, "", "attack_protocol_probe", OnFrame, nullptr, 1920, 1080);
    void* impress_s = ImpressSessionCreate(pptx, "", "attack_protocol_probe", OnFrame, nullptr, 1920, 1080);
    
    if (!calc_s || !impress_s) {
        printf("Setup failed\n");
        return 1;
    }
    
    CalcSessionStart(calc_s);
    ImpressSessionStart(impress_s);
    
    // 攻击1: 暂停/恢复状态
    if (attack_type == 1 || attack_type == 2) {
        printf("\n--- Attack 1: Pause/Resume State (经验41) ---\n");
        fflush(stdout);
        
        bool prev_corruption = s_state_corruption;
        attack_pause_resume_state(impress_s, true, 50);
        
        if (s_state_corruption != prev_corruption) {
            printf(">>> ATTACK 1 SUCCESS: Pause/resume state corruption detected (1 point)\n");
            score++;
        } else {
            printf(">>> Attack 1 FAILED: No pause/resume state corruption\n");
        }
        fflush(stdout);
    }
    
    // 攻击2: Start幂等性
    if (attack_type == 2) {
        printf("\n--- Attack 2: Start Idempotence (经验42 P5) ---\n");
        fflush(stdout);
        
        bool prev_corruption = s_state_corruption;
        attack_start_idempotence(calc_s, false);
        attack_start_idempotence(impress_s, true);
        
        if (s_state_corruption != prev_corruption) {
            printf(">>> ATTACK 2 SUCCESS: Start idempotence issue detected (1 point)\n");
            score++;
        } else {
            printf(">>> Attack 2 FAILED: No Start idempotence issue\n");
        }
        fflush(stdout);
    }
    
    // 清理
    CalcSessionStop(calc_s);
    ImpressSessionStop(impress_s);
    CalcSessionDestroy(calc_s);
    ImpressSessionDestroy(impress_s);
    
    printf("\n=== FINAL SCORE: %d/2 ===\n", score);
    printf("Note: Each protocol state corruption detected = 1 point\n");
    fflush(stdout);
    
    return 0;
}
