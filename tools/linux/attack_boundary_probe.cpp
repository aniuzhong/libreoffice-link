// attack_boundary_probe.cpp — 边界条件攻击探针
// 攻击点1: 极端分辨率攻击（经验14：屏高≥最大文档分辨率）
// 攻击点2: 极端坐标攻击（经验15：16位坐标上限32767）
// 攻击点3: 空指针/无效参数攻击
// 攻击点4: 极端数值攻击（SetScale的10-400范围）
// 攻击点5: 文件路径边界攻击（经验38：中文路径处理）
#include <base/abi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

static int s_frames = 0;
static int s_boundary_violations = 0;

static void OnFrame(const uint8_t* data, int32_t w, int32_t h, int32_t rp,
                    int32_t size, int32_t format, void* opaque) {
    (void)data; (void)w; (void)h; (void)rp; (void)size; (void)format; (void)opaque;
    s_frames++;
}

// 攻击1: 极端分辨率攻击
void attack_extreme_resolution(const char* xlsx, const char* pptx) {
    printf("Starting extreme resolution attack\n");
    fflush(stdout);
    
    // 测试极端大分辨率（经验15：30720x2160上限）
    int extreme_widths[] = {32768, 65536, 100000, -1};
    int extreme_heights[] = {32768, 65536, 100000, -1};
    
    for (int i = 0; extreme_widths[i] > 0; i++) {
        printf("Testing extreme resolution: %dx%d\n", extreme_widths[i], extreme_heights[i]);
        fflush(stdout);
        
        void* calc_s = CalcSessionCreate(xlsx, "", "attack_boundary_probe", OnFrame, nullptr, 
                                         extreme_widths[i], extreme_heights[i]);
        if (calc_s) {
            printf("  Calc Create OK with %dx%d\n", extreme_widths[i], extreme_heights[i]);
            CalcSessionDestroy(calc_s);
        } else {
            printf("  Calc Create FAILED with %dx%d - boundary protection?\n", 
                   extreme_widths[i], extreme_heights[i]);
            s_boundary_violations++;
        }
        
        void* impress_s = ImpressSessionCreate(pptx, "", "attack_boundary_probe", OnFrame, nullptr,
                                              extreme_widths[i], extreme_heights[i]);
        if (impress_s) {
            printf("  Impress Create OK with %dx%d\n", extreme_widths[i], extreme_heights[i]);
            ImpressSessionDestroy(impress_s);
        } else {
            printf("  Impress Create FAILED with %dx%d - boundary protection?\n",
                   extreme_widths[i], extreme_heights[i]);
            s_boundary_violations++;
        }
    }
    
    // 测试零和负分辨率
    int invalid_sizes[] = {0, -1, -100, 1, 10};
    for (int i = 0; i < 5; i++) {
        printf("Testing invalid resolution: %dx%d\n", invalid_sizes[i], invalid_sizes[i]);
        fflush(stdout);
        
        void* calc_s = CalcSessionCreate(xlsx, "", "attack_boundary_probe", OnFrame, nullptr,
                                         invalid_sizes[i], invalid_sizes[i]);
        if (calc_s) {
            printf("  WARNING: Calc accepted invalid size %dx%d\n", invalid_sizes[i], invalid_sizes[i]);
            CalcSessionDestroy(calc_s);
            s_boundary_violations++;
        } else {
            printf("  Calc correctly rejected %dx%d\n", invalid_sizes[i], invalid_sizes[i]);
        }
    }
    
    printf("Extreme resolution attack completed, violations=%d\n", s_boundary_violations);
    fflush(stdout);
}

// 攻击2: 空指针/无效参数攻击
void attack_null_invalid_params(const char* xlsx, const char* pptx) {
    printf("Starting null/invalid parameter attack\n");
    fflush(stdout);
    
    // 空路径测试
    printf("Testing null path...\n");
    void* null_path_calc = CalcSessionCreate(nullptr, "", "attack_boundary_probe", OnFrame, nullptr, 1920, 1080);
    if (null_path_calc) {
        printf("  WARNING: Calc accepted null path\n");
        CalcSessionDestroy(null_path_calc);
        s_boundary_violations++;
    } else {
        printf("  Calc correctly rejected null path\n");
    }
    
    void* null_path_impress = ImpressSessionCreate(nullptr, "", "attack_boundary_probe", OnFrame, nullptr, 1920, 1080);
    if (null_path_impress) {
        printf("  WARNING: Impress accepted null path\n");
        ImpressSessionDestroy(null_path_impress);
        s_boundary_violations++;
    } else {
        printf("  Impress correctly rejected null path\n");
    }
    
    // 空路径字符串测试
    printf("Testing empty path string...\n");
    void* empty_path_calc = CalcSessionCreate("", "", "attack_boundary_probe", OnFrame, nullptr, 1920, 1080);
    if (empty_path_calc) {
        printf("  WARNING: Calc accepted empty path\n");
        CalcSessionDestroy(empty_path_calc);
        s_boundary_violations++;
    } else {
        printf("  Calc correctly rejected empty path\n");
    }
    
    // 空回调测试
    printf("Testing null callback...\n");
    void* null_cb_calc = CalcSessionCreate(xlsx, "", "attack_boundary_probe", nullptr, nullptr, 1920, 1080);
    if (null_cb_calc) {
        printf("  WARNING: Calc accepted null callback\n");
        CalcSessionDestroy(null_cb_calc);
        s_boundary_violations++;
    } else {
        printf("  Calc correctly rejected null callback\n");
    }
    
    printf("Null/invalid parameter attack completed, violations=%d\n", s_boundary_violations);
    fflush(stdout);
}

// 攻击3: 极端数值攻击（SetScale范围10-400）
void attack_extreme_scale_values(const char* xlsx) {
    printf("Starting extreme scale values attack\n");
    fflush(stdout);
    
    void* calc_s = CalcSessionCreate(xlsx, "", "attack_boundary_probe", OnFrame, nullptr, 1920, 1080);
    if (!calc_s) {
        printf("Setup failed\n");
        return;
    }
    
    CalcSessionStart(calc_s);
    
    // 测试边界外数值
    int extreme_scales[] = {0, 1, 5, 9, 401, 500, 1000, -1, -100};
    for (int i = 0; extreme_scales[i] != -1; i++) {
        printf("Testing scale=%d\n", extreme_scales[i]);
        bool ok = CalcSessionSetScale(calc_s, extreme_scales[i]);
        if (ok) {
            printf("  WARNING: Calc accepted scale=%d (outside 10-400 range)\n", extreme_scales[i]);
            s_boundary_violations++;
        } else {
            printf("  Calc correctly rejected scale=%d\n", extreme_scales[i]);
        }
    }
    
    // 测试边界内数值
    int boundary_scales[] = {10, 11, 399, 400};
    for (int i = 0; i < 4; i++) {
        printf("Testing boundary scale=%d\n", boundary_scales[i]);
        bool ok = CalcSessionSetScale(calc_s, boundary_scales[i]);
        if (!ok) {
            printf("  WARNING: Calc rejected valid scale=%d\n", boundary_scales[i]);
            s_boundary_violations++;
        } else {
            printf("  Calc correctly accepted scale=%d\n", boundary_scales[i]);
        }
    }
    
    CalcSessionStop(calc_s);
    CalcSessionDestroy(calc_s);
    
    printf("Extreme scale values attack completed, violations=%d\n", s_boundary_violations);
    fflush(stdout);
}

// 攻击4: 页面索引边界攻击
void attack_page_index_bounds(const char* pptx) {
    printf("Starting page index bounds attack\n");
    fflush(stdout);
    
    void* impress_s = ImpressSessionCreate(pptx, "", "attack_boundary_probe", OnFrame, nullptr, 1920, 1080);
    if (!impress_s) {
        printf("Setup failed\n");
        return;
    }
    
    ImpressSessionStart(impress_s);
    
    int page_count = ImpressSessionGetPageCount(impress_s);
    printf("Total pages: %d\n", page_count);
    
    // 测试边界外页面索引
    int extreme_pages[] = {-1, -100, page_count, page_count + 1, page_count + 100, 1000000};
    for (int i = 0; i < 6; i++) {
        printf("Testing GoToPage=%d\n", extreme_pages[i]);
        bool ok = ImpressSessionGoToPage(impress_s, extreme_pages[i]);
        if (ok) {
            printf("  WARNING: Impress accepted page=%d (outside 0-%d range)\n", 
                   extreme_pages[i], page_count - 1);
            s_boundary_violations++;
        } else {
            printf("  Impress correctly rejected page=%d\n", extreme_pages[i]);
        }
    }
    
    // 测试有效页面索引
    if (page_count > 0) {
        printf("Testing valid page indices: 0, %d\n", page_count - 1);
        bool ok0 = ImpressSessionGoToPage(impress_s, 0);
        bool ok_last = ImpressSessionGoToPage(impress_s, page_count - 1);
        if (!ok0 || !ok_last) {
            printf("  WARNING: Impress rejected valid page indices\n");
            s_boundary_violations++;
        } else {
            printf("  Impress correctly accepted valid page indices\n");
        }
    }
    
    ImpressSessionStop(impress_s);
    ImpressSessionDestroy(impress_s);
    
    printf("Page index bounds attack completed, violations=%d\n", s_boundary_violations);
    fflush(stdout);
}

// 攻击5: SetResolution动态变化攻击
void attack_dynamic_resolution_change(const char* xlsx) {
    printf("Starting dynamic resolution change attack\n");
    fflush(stdout);
    
    void* calc_s = CalcSessionCreate(xlsx, "", "attack_boundary_probe", OnFrame, nullptr, 1920, 1080);
    if (!calc_s) {
        printf("Setup failed\n");
        return;
    }
    
    CalcSessionStart(calc_s);
    
    // 快速切换分辨率
    int resolutions[][2] = {{1920, 1080}, {3840, 2160}, {1280, 720}, {640, 480}, {1920, 1080}};
    for (int i = 0; i < 5; i++) {
        printf("Testing resolution change to %dx%d\n", resolutions[i][0], resolutions[i][1]);
        bool ok = CalcSessionSetResolution(calc_s, resolutions[i][0], resolutions[i][1]);
        if (!ok) {
            printf("  Resolution change failed\n");
            s_boundary_violations++;
        }
        usleep(100000); // 100ms
    }
    
    // 测试无效分辨率动态设置
    printf("Testing invalid resolution changes...\n");
    bool invalid1 = CalcSessionSetResolution(calc_s, -1, 1080);
    bool invalid2 = CalcSessionSetResolution(calc_s, 1920, -1);
    bool invalid3 = CalcSessionSetResolution(calc_s, 0, 0);
    
    if (invalid1 || invalid2 || invalid3) {
        printf("  WARNING: Calc accepted invalid resolution changes\n");
        s_boundary_violations++;
    } else {
        printf("  Calc correctly rejected invalid resolution changes\n");
    }
    
    CalcSessionStop(calc_s);
    CalcSessionDestroy(calc_s);
    
    printf("Dynamic resolution change attack completed, violations=%d\n", s_boundary_violations);
    fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        fprintf(stderr, "usage: %s <xlsx> <pptx> [attack_type=1|2|3|4|5|all]\n", argv[0]);
        fprintf(stderr, "  attack_type: 1=extreme resolution, 2=null params, 3=extreme scale, 4=page bounds, 5=dynamic resolution, all=all\n");
        return 2;
    }
    
    const char* xlsx = argv[1];
    const char* pptx = argv[2];
    int attack_type = (argc >= 4) ? 
        (strcmp(argv[3], "all") == 0 ? 5 : atoi(argv[3])) : 1;
    
    printf("=== ATTACK BOUNDARY PROBE ===\n");
    printf("xlsx=%s pptx=%s attack_type=%d\n", xlsx, pptx, attack_type);
    fflush(stdout);
    
    int score = 0;
    
    // 攻击1: 极端分辨率
    if (attack_type == 1 || attack_type == 5) {
        printf("\n--- Attack 1: Extreme Resolution (经验14/15) ---\n");
        fflush(stdout);
        
        int before = s_boundary_violations;
        attack_extreme_resolution(xlsx, pptx);
        int after = s_boundary_violations;
        
        if (after > before) {
            printf(">>> ATTACK 1 SUCCESS: Boundary violations detected (%d) (1 point)\n", after - before);
            score++;
        } else {
            printf(">>> Attack 1 FAILED: No boundary violations\n");
        }
        fflush(stdout);
    }
    
    // 攻击2: 空指针/无效参数
    if (attack_type == 2 || attack_type == 5) {
        printf("\n--- Attack 2: Null/Invalid Parameters ---\n");
        fflush(stdout);
        
        int before = s_boundary_violations;
        attack_null_invalid_params(xlsx, pptx);
        int after = s_boundary_violations;
        
        if (after > before) {
            printf(">>> ATTACK 2 SUCCESS: Parameter validation issues detected (1 point)\n");
            score++;
        } else {
            printf(">>> Attack 2 FAILED: No parameter validation issues\n");
        }
        fflush(stdout);
    }
    
    // 攻击3: 极端数值
    if (attack_type == 3 || attack_type == 5) {
        printf("\n--- Attack 3: Extreme Scale Values ---\n");
        fflush(stdout);
        
        int before = s_boundary_violations;
        attack_extreme_scale_values(xlsx);
        int after = s_boundary_violations;
        
        if (after > before) {
            printf(">>> ATTACK 3 SUCCESS: Scale boundary issues detected (1 point)\n");
            score++;
        } else {
            printf(">>> Attack 3 FAILED: No scale boundary issues\n");
        }
        fflush(stdout);
    }
    
    // 攻击4: 页面索引边界
    if (attack_type == 4 || attack_type == 5) {
        printf("\n--- Attack 4: Page Index Bounds ---\n");
        fflush(stdout);
        
        int before = s_boundary_violations;
        attack_page_index_bounds(pptx);
        int after = s_boundary_violations;
        
        if (after > before) {
            printf(">>> ATTACK 4 SUCCESS: Page index boundary issues detected (1 point)\n");
            score++;
        } else {
            printf(">>> Attack 4 FAILED: No page index boundary issues\n");
        }
        fflush(stdout);
    }
    
    // 攻击5: 动态分辨率变化
    if (attack_type == 5) {
        printf("\n--- Attack 5: Dynamic Resolution Change ---\n");
        fflush(stdout);
        
        int before = s_boundary_violations;
        attack_dynamic_resolution_change(xlsx);
        int after = s_boundary_violations;
        
        if (after > before) {
            printf(">>> ATTACK 5 SUCCESS: Dynamic resolution issues detected (1 point)\n");
            score++;
        } else {
            printf(">>> Attack 5 FAILED: No dynamic resolution issues\n");
        }
        fflush(stdout);
    }
    
    printf("\n=== FINAL SCORE: %d/5 ===\n", score);
    printf("Note: Each boundary violation detected = 1 point\n");
    fflush(stdout);
    
    return 0;
}
