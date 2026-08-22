// writer_probe.cpp — writerlink 运行态验证 (自治 PDF 位图管线, 经验 38)
// 验证: Create (docx→PDF→Draw 导入→首页渲染) / 翻页 (同步渲染+推帧) /
//   GoToPage / 帧内容 (非白%) / 缓存命中 (NPOfficeCache 复用 vs 自转)。
// 用法: writer_probe <doc> [翻页数=5]
#include <abi/abi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>

static std::vector<uint8_t> s_last;
static int s_w = 0, s_h = 0;
static int s_frames = 0;

static void OnFrame(const uint8_t* data, int32_t w, int32_t h, int32_t rp,
                    int32_t size, int32_t format, void* opaque) {
    (void)rp; (void)format; (void)opaque;
    s_frames++;
    s_w = w; s_h = h;
    s_last.assign(data, data + size);
}

// 非白像素占比 (内容验证)
static int NonWhitePct() {
    if (s_last.empty()) return -1;
    int nw = 0, total = (int)s_last.size() / 4;
    for (size_t i = 0; i < s_last.size(); i += 4) {
        uint8_t b = s_last[i], g = s_last[i + 1], r = s_last[i + 2];
        if (!(r > 245 && g > 245 && b > 245)) nw++;
    }
    return total ? nw * 100 / total : -1;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <doc> [翻页数=5]\n", argv[0]);
        return 2;
    }
    int n_next = (argc >= 3) ? atoi(argv[2]) : 5;

    long long t0 = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
    void* s = WriterSessionCreate(argv[1], "", "writer_probe", OnFrame, nullptr, 1920, 1080);
    long long t_create = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count() - t0;
    if (!s) { fprintf(stderr, "Create FAILED\n"); return 1; }
    int pages = WriterSessionGetPageCount(s);
    printf("Create OK: %lld ms, pages=%d, first frame %dx%d nonwhite=%d%% frames=%d\n",
           t_create, pages, s_w, s_h, NonWhitePct(), s_frames);

    WriterSessionStart(s);
    usleep(500 * 1000); // 心跳帧流
    printf("started: frames=%d (%dx%d nonwhite=%d%%)\n", s_frames, s_w, s_h, NonWhitePct());

    // 翻页 (同步渲染 + 推帧, 计时)
    long long total_next = 0;
    int n_turned = 0; // 实际翻页数 (单页文档为 0, 平均值按实翻计)
    for (int i = 0; i < n_next && i < pages - 1; i++) {
        int before = s_frames;
        long long t1 = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
        bool ok = WriterSessionNextPage(s);
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count() - t1;
        total_next += ms;
        n_turned++;
        usleep(150 * 1000);
        printf("next %d: %s (%lld ms), page=%d/%d, nonwhite=%d%% frames=%d\n",
               i + 1, ok ? "OK" : "FAIL", ms,
               WriterSessionGetCurrentPage(s), pages, NonWhitePct(), s_frames);
        if (!ok) break;
    }
    printf("翻页平均: %lld ms (实翻 %d/%d 页)\n",
           n_turned ? total_next / n_turned : 0, n_turned, n_next);

    // PreviousPage 验证 (上层反馈"上一页没生效", 先隔离底层)
    if (pages > 3) {
        WriterSessionGoToPage(s, 5);
        usleep(200 * 1000);
        printf("goto 5: page=%d\n", WriterSessionGetCurrentPage(s));
        bool okp = WriterSessionPreviousPage(s);
        usleep(200 * 1000);
        printf("prev: %s page=%d nonwhite=%d%%\n", okp ? "OK" : "FAIL",
               WriterSessionGetCurrentPage(s), NonWhitePct());
    }

    // GoToPage 跳转
    if (pages > 3) {
        long long t2 = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
        bool ok = WriterSessionGoToPage(s, pages - 1);
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count() - t2;
        usleep(200 * 1000);
        printf("goto last: %s (%lld ms), page=%d, nonwhite=%d%%\n",
               ok ? "OK" : "FAIL", ms, WriterSessionGetCurrentPage(s), NonWhitePct());
    }

    // ---- 缺陷复现 2: Stop 后 UpdateFrame 应仍能取到帧 (接口契约: 会话保留可抓)
    WriterSessionStop(s);
    s_frames = 0;
    s_last.clear();
    WriterSessionUpdateFrame(s);
    usleep(400 * 1000);
    printf("stop+updateframe: frames=%d %s\n", s_frames,
           s_frames > 0 ? "OK" : "FAIL (帧数为 0: Stop 后 UpdateFrame 无效)");
    WriterSessionStart(s); // 恢复, 继续后续验证
    usleep(200 * 1000);

    // ---- LO 行为认知锚点: 混合页面尺寸文档 (WRITER_MIXED=1)。
    // 实测 (2026-08-17): draw_pdf_Import 后所有页统一为第一页尺寸
    // (横竖混排 PDF 页 612x792/842x595/595x842 → createPreview 全 834x1080),
    // 故"缓存命中不刷新 width_/height_"的错配前提不存在 (LO 层恒同尺寸)。
    // 本段保留作回归锚点: 若未来 LO 行为变化 (尺寸开始随页变), 这里会 FAIL
    // 提醒补充 per-page 尺寸处理。
    if (getenv("WRITER_MIXED") && pages > 1) {
        WriterSessionGoToPage(s, 0); // 竖页 (冷/缓存)
        usleep(200 * 1000);
        int w0 = s_w, h0 = s_h;
        WriterSessionGoToPage(s, 1); // 横页
        usleep(200 * 1000);
        int w1 = s_w, h1 = s_h;
        WriterSessionGoToPage(s, 0); // 回竖页 (应命中缓存)
        usleep(200 * 1000);
        printf("mixed: p0=%dx%d p1=%dx%d back-p0=%dx%d %s\n", w0, h0, w1, h1, s_w, s_h,
               (s_w == w0 && s_h == h0) ? "OK" : "FAIL (回跳后尺寸未随页刷新)");
    }

    WriterSessionStop(s);
    WriterSessionDestroy(s);
    printf("done (total frames=%d)\n", s_frames);
    return 0;
}
