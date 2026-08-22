// media_green_probe.cpp — 媒体全绿里程碑探针 (方案 A 验证)
// ORT_MEDIA_BACKEND=ffplay 时, LO 真实播放路径的媒体后端选择应命中
// Manager_FFPlay (mediawindow_impl.cxx 补丁), 媒体区域渲染纯绿 (GreenWindow
// 服务端背景子窗口)。不设变量时走 GStreamer 正常播放 (回归对照)。
// 用法: media_green_probe <pptx> [每页等待ms=2500]
// 判定: [FFPLAY] 日志 (内核 stderr 透传) + 每页绿色像素占比。
#include <base/abi.h>

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <unistd.h>

static std::vector<uint8_t> s_last;
static std::vector<uint8_t> s_prev; // 上一帧 (帧间差异判据)
static int s_frames = 0;

static void OnFrame(const uint8_t* data, int32_t w, int32_t h, int32_t rp,
                    int32_t size, int32_t format, void* opaque) {
    (void)rp; (void)format; (void)opaque;
    s_frames++;
    s_prev = s_last; // 保留上一帧
    s_last.assign(data, data + size);
}

// 帧间差异: 与上一帧不同像素占比 (视频在动 = 真实播放判据, 经验 34)
static int DiffPct() {
    if (s_last.empty() || s_prev.empty() || s_last.size() != s_prev.size())
        return -1;
    int d = 0, total = (int)s_last.size() / 4;
    for (size_t i = 0; i < s_last.size(); i += 4) {
        if (s_last[i] != s_prev[i] || s_last[i+1] != s_prev[i+1] || s_last[i+2] != s_prev[i+2])
            d++;
    }
    return total ? d * 100 / total : -1;
}

// 绿色像素占比: BGRA 帧中 B<40 && G>230 && R<40 (GreenWindow 纯绿 0x00FF00)
static int GreenPct() {
    if (s_last.empty()) return -1;
    int g = 0, total = (int)s_last.size() / 4;
    for (size_t i = 0; i < s_last.size(); i += 4) {
        uint8_t b = s_last[i], gr = s_last[i + 1], r = s_last[i + 2];
        if (gr > 230 && r < 40 && b < 40) g++;
    }
    return total ? g * 100 / total : -1;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <pptx> [wait_ms=2500]\n", argv[0]);
        return 2;
    }
    int wait_ms = (argc >= 3) ? atoi(argv[2]) : 2500;

    void* s = ImpressSessionCreate(argv[1], "", "media_green_probe", OnFrame, nullptr, 1920, 1080);
    if (!s) { fprintf(stderr, "Create FAILED\n"); return 1; }
    int n = ImpressSessionGetPageCount(s);
    printf("Create OK slides=%d\n", n);
    ImpressSessionStart(s);
    usleep(1500 * 1000); // 首帧

    for (int p = 0; p < n; p++) {
        ImpressSessionGoToPage(s, p);
        // 页面稳定等待 (媒体页含媒体子系统初始化, 双时点采样)
        usleep(wait_ms * 1000);
        int g1 = GreenPct();
        int d1 = DiffPct();
        usleep(1500 * 1000);
        int g2 = GreenPct();
        int d2 = DiffPct();
        int cur = ImpressSessionGetCurrentPage(s);
        printf("[page %d] current=%d green=%d%%->%d%% diff=%d%%->%d%% frames=%d\n",
               p, cur, g1, g2, d1, d2, s_frames);
        fflush(stdout);
    }
    // ===== 静音专项 ABI 端到端验证 =====
    // 链路: ImpressSessionSetMute ABI → ImpressSession::SetMute →
    //       link_utils::MuteAllFfplayEngines → dlopen/dlsym(ffplay.so) →
    //       ffplay_set_mute_all → FfplayPlayer::SetMuteAll → 遍历 g_engines set_volume。
    // 期望: 1) 返回 1 (dlopen+dlsym 成功);
    //      2) stderr [FFPLAY] SetMuteAll(true) engines=N (N=本次注册的引擎数, 媒体页≥1);
    //      3) unmute 同样返回 1 + stderr engines=N。
    // 后端为 gstreamer (无 ORT_MEDIA_BACKEND=ffplay) 时: dlopen ffplay.so 仍成功
    //      (office/program 内总有 ffplay.so), 但 engines=0 (无 FfplayPlayer 实例注册)。
    int mute_r = ImpressSessionSetMute(s, 1);
    printf("[SetMute] mute=1 result=%d (1=OK, 0=fail/absent)\n", mute_r);
    fflush(stdout);
    usleep(800 * 1000);
    int unmute_r = ImpressSessionSetMute(s, 0);
    printf("[SetMute] mute=0 result=%d\n", unmute_r);
    fflush(stdout);
    usleep(500 * 1000);

    ImpressSessionStop(s);
    ImpressSessionDestroy(s);
    printf("done\n");
    return 0;
}
