// mute_isolation_probe.cpp — 方案 A (per-window 静音隔离) 验证
//
// 场景: 创建 2 个 ImpressSession 加载同一带视频 pptx, soffice.bin 共享内核模式下
// 两个 session 的 ffplay 引擎同处一个 g_engines 静态表。验证 SetMute 按
// parent_window_id 物理句柄精确过滤, 不互相干扰。
//
// 判据 (自动从 ffplay_<pid>.log 抓 SetMuteAll 行的 matched/engines 数):
//   engines_total = 首次 SetMuteAll 行的 engines=N (子进程内全部引擎)
//   对 A 静音 matched_A1, 对 B 静音 matched_B1
//   PASS: matched_A1 > 0 && matched_B1 > 0 && matched_A1 + matched_B1 == engines_total
//   FAIL-A (全局 bug): matched_A1 == engines_total (A 静音误命中 B)
//   FAIL-B (per-session 错配): matched_A1 == 0 (A 的引擎 session_id 未归属)
//
// 沙箱跑: ORT_HOME=/tmp/ort_mute LD_LIBRARY_PATH=<deploy>/office/program \
//         <probe> <abs_pptx_path>
// 跑前清场: 见 HANDOFF 1.4 (kill Xvfb + rm lock + rm /tmp/.X11-unix/X9*)
// 素材: 必须绝对路径 (经验 46), 用带视频的 pptx (如 tools/data/AI时代.pptx)

#include <base/abi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

static void FrameCb(const uint8_t*, int32_t, int32_t, int32_t,
                    int32_t, int32_t, void*) {}

// 从 ORT_HOME/logs 或 ~/.office-link/logs 取最新 ffplay_<pid>.log 路径
static std::string GetLatestFfplayLog() {
    const char* ort_home = getenv("ORT_HOME");
    std::string dir = ort_home
        ? std::string(ort_home) + "/logs"
        : std::string(getenv("HOME")) + "/.office-link/logs";
    std::string cmd = "ls -t " + dir + "/ffplay_*.log 2>/dev/null | head -1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    char buf[1024];
    std::string line;
    if (fgets(buf, sizeof(buf), p)) line = buf;
    pclose(p);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    return line;
}

// 抓最新一行 SetMuteAll, 输出参数填 engines/matched; 返回行内容 (调试用)
static std::string GetLastMuteLine(int* engines, int* matched) {
    if (engines) *engines = -1;
    if (matched) *matched = -1;
    std::string log = GetLatestFfplayLog();
    if (log.empty()) return "";
    std::string cmd = "grep 'SetMuteAll' " + log + " 2>/dev/null | tail -1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "";
    char buf[2048];
    std::string line;
    if (fgets(buf, sizeof(buf), p)) line = buf;
    pclose(p);
    if (const char* pe = strstr(line.c_str(), "engines="))
        if (engines) *engines = atoi(pe + 8);
    if (const char* pm = strstr(line.c_str(), "matched="))
        if (matched) *matched = atoi(pm + 8);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    return line;
}

// 统计 ffplay log 中 createPlayerWindow 出现次数 (引擎注册的判据)
static int CountPlayerWindows() {
    std::string log = GetLatestFfplayLog();
    if (log.empty()) return 0;
    std::string cmd = "grep -c 'createPlayerWindow' " + log + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return 0;
    char buf[64];
    int n = 0;
    if (fgets(buf, sizeof(buf), p)) n = atoi(buf);
    pclose(p);
    return n;
}

// 翻页等引擎就绪: A 和 B 同步翻页直到 ffplay log 有 ≥ target 个 createPlayerWindow
// (LO 进入含视频页才创建 player; 首页无视频须翻页触发)
static bool WaitForEngines(void* sa, void* sb, int target, int max_pages) {
    for (int p = 0; p < max_pages; p++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        int n = CountPlayerWindows();
        printf("  翻页 %d: createPlayerWindow=%d (目标 %d)\n", p, n, target);
        if (n >= target) return true;
        ImpressSessionNextPage(sa);
        ImpressSessionNextPage(sb);
    }
    return false;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <abs_pptx_path>\n", argv[0]);
        return 2;
    }
    const char* path = argv[1];
    // 经验 46: 相对路径会失败; 显式校验绝对路径
    if (path[0] != '/') {
        fprintf(stderr, "FAIL: path must be absolute (经验 46): %s\n", path);
        return 2;
    }

    printf("=== 方案 A: per-window 静音隔离验证 ===\n");
    printf("素材: %s\n", path);
    printf("沙箱建议: ORT_HOME=/tmp/ort_mute LD_LIBRARY_PATH=<deploy>/office/program\n\n");

    printf("步骤 1: 创建 session A + B (加载带视频 pptx, 共享内核)\n");
    void* sa = ImpressSessionCreate(path, "", "mute_iso_a", FrameCb, nullptr, 1920, 1080);
    if (!sa) { fprintf(stderr, "FAIL: session A create\n"); return 1; }
    ImpressSessionStart(sa);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    void* sb = ImpressSessionCreate(path, "", "mute_iso_b", FrameCb, nullptr, 1920, 1080);
    if (!sb) { fprintf(stderr, "FAIL: session B create\n"); ImpressSessionDestroy(sa); return 1; }
    ImpressSessionStart(sb);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));  // 等两引擎都创建

    // AI时代.pptx 等素材视频可能在非首页; 翻页遍历直到 LO 创建 ≥2 个 ffplay 引擎 (A/B 各一)
    printf("\n步骤 1.5: 翻页等引擎就绪 (目标 ≥2 个 createPlayerWindow)\n");
    bool engines_ready = WaitForEngines(sa, sb, 2, 29);
    if (!engines_ready) {
        int n = CountPlayerWindows();
        printf("WARN: 翻 29 页后引擎数=%d (未达 2), 继续 SetMute 验证 (可能 pptx 无视频或非首页)\n", n);
    }

    printf("\n步骤 2: SetMute(A, true) — 应只命中 A 的引擎\n");
    ImpressSessionSetMute(sa, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int eng_total = -1, matched_a1 = -1;
    std::string line_a1 = GetLastMuteLine(&eng_total, &matched_a1);
    printf("  ffplay log: %s\n", line_a1.c_str());
    printf("  engines_total=%d matched_A=%d\n", eng_total, matched_a1);

    printf("\n步骤 3: SetMute(B, true) — 应只命中 B 的引擎, 不影响 A\n");
    ImpressSessionSetMute(sb, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int matched_b1 = -1;
    std::string line_b1 = GetLastMuteLine(nullptr, &matched_b1);
    printf("  ffplay log: %s\n", line_b1.c_str());
    printf("  matched_B=%d\n", matched_b1);

    printf("\n步骤 4: SetMute(A, false) — 恢复 A, 不影响 B\n");
    ImpressSessionSetMute(sa, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int matched_a2 = -1;
    std::string line_a2 = GetLastMuteLine(nullptr, &matched_a2);
    printf("  ffplay log: %s\n", line_a2.c_str());
    printf("  matched_A_restore=%d\n", matched_a2);

    printf("\n步骤 5: SetMute(B, false) — 恢复 B\n");
    ImpressSessionSetMute(sb, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int matched_b2 = -1;
    std::string line_b2 = GetLastMuteLine(nullptr, &matched_b2);
    printf("  ffplay log: %s\n", line_b2.c_str());
    printf("  matched_B_restore=%d\n", matched_b2);

    ImpressSessionStop(sa);
    ImpressSessionStop(sb);
    ImpressSessionDestroy(sa);
    ImpressSessionDestroy(sb);

    printf("\n=== 判定 ===\n");
    bool ok_engines = (eng_total >= 2);
    bool ok_a = (matched_a1 > 0 && matched_a1 < eng_total);  // A 命中且 < 总数
    bool ok_b = (matched_b1 > 0 && matched_b1 < eng_total);  // B 命中且 < 总数
    bool ok_partition = (matched_a1 > 0 && matched_b1 > 0 &&
                         matched_a1 + matched_b1 == eng_total);  // 无重叠无遗漏
    bool ok_a_restore = (matched_a2 == matched_a1);  // 恢复 A 命中同数
    bool ok_b_restore = (matched_b2 == matched_b1);

    printf("  engines_total >= 2:    %s (engines=%d)\n",
           ok_engines ? "PASS" : "FAIL", eng_total);
    printf("  A 静音 matched_A > 0 && < total: %s (matched_A=%d)\n",
           ok_a ? "PASS" : "FAIL", matched_a1);
    printf("  B 静音 matched_B > 0 && < total: %s (matched_B=%d)\n",
           ok_b ? "PASS" : "FAIL", matched_b1);
    printf("  无重叠无遗漏 (A+B == total): %s (A=%d + B=%d == %d)\n",
           ok_partition ? "PASS" : "FAIL",
           matched_a1, matched_b1, eng_total);
    printf("  A 恢复 matched == 静音 matched: %s (%d == %d)\n",
           ok_a_restore ? "PASS" : "FAIL", matched_a2, matched_a1);
    printf("  B 恢复 matched == 静音 matched: %s (%d == %d)\n",
           ok_b_restore ? "PASS" : "FAIL", matched_b2, matched_b1);

    if (matched_a1 == eng_total)
        printf("\nFAIL-A (全局 bug): A 静音命中所有 %d 个引擎 (方案 A 未生效)\n", eng_total);
    if (matched_a1 == 0)
        printf("\nFAIL-B (per-session 错配): A 静音 matched=0 (window_id 归属未建立)\n");
    if (matched_a1 > 0 && matched_b1 > 0 && matched_a1 == matched_b1 && eng_total > 0 &&
        matched_a1 + matched_b1 == eng_total)
        printf("\nPASS: 方案 A per-window 隔离生效, A/B 引擎精确分流\n");
    else
        printf("\nFAIL: 方案 A 隔离未达成, 请检查 ffplay log\n");

    printf("done\n");
    return 0;
}
