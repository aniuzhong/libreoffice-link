// ui_1px_probe.cpp — 1px 现象量化验证: 2 xlsx + 1 pptx 并发, 抓帧存盘,
// 分析 impress 帧底部 N 行像素, 判定是否与 xlsx 内容串扰。
// 用法: ./ui_1px_probe <xlsx1> <xlsx2> <pptx>
// 输出: /tmp/ui_1px_{calc1,calc2,impress}.bmp + 底部行像素摘要
// 后续: 用 ffmpeg 精确对比 impress 底部行与 calc 帧的相似度
#include <abi/abi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>
#include <unistd.h>

struct Session {
    void* handle = nullptr;
    const char* tag = "?";
    std::atomic<int> frames{0};
    std::vector<uint8_t> last;
    int w = 0, h = 0;
    std::atomic<bool> ok{false};
};

static void CalcCb(const uint8_t* data, int32_t w, int32_t h, int32_t rp,
                   int32_t size, int32_t format, void* opaque) {
    Session* s = static_cast<Session*>(opaque);
    s->frames++;
    s->w = w; s->h = h;
    s->last.assign(data, data + size);
}

static void ImpressCb(const uint8_t* data, int32_t w, int32_t h, int32_t rp,
                      int32_t size, int32_t format, void* opaque) {
    Session* s = static_cast<Session*>(opaque);
    s->frames++;
    s->w = w; s->h = h;
    s->last.assign(data, data + size);
}

// BGRA buffer -> BMP 文件 (与 iframe_probe 同构)
static void SaveBmp(const char* path, const std::vector<uint8_t>& bgra, int w, int h) {
    if (bgra.empty() || w <= 0 || h <= 0) { printf("[save] %s skip (empty)\n", path); return; }
    int row = ((w * 3 + 3) / 4) * 4;
    std::vector<uint8_t> bmp(54 + row * h);
    memcpy(&bmp[0], "BM", 2);
    *(int*)&bmp[2] = 54 + row * h; *(int*)&bmp[10] = 54; *(int*)&bmp[14] = 40;
    *(int*)&bmp[18] = w; *(int*)&bmp[22] = h; *(short*)&bmp[26] = 1; *(short*)&bmp[28] = 24;
    for (int y = 0; y < h; y++) {
        const uint8_t* src = &bgra[(h - 1 - y) * w * 4];
        uint8_t* dst = &bmp[54 + y * row];
        for (int x = 0; x < w; x++) {
            dst[x * 3] = src[x * 4];       // B
            dst[x * 3 + 1] = src[x * 4 + 1]; // G
            dst[x * 3 + 2] = src[x * 4 + 2]; // R
        }
    }
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(bmp.data(), 1, bmp.size(), f); fclose(f); printf("[save] %s (%dx%d)\n", path, w, h); }
    else printf("[save] %s FAILED\n", path);
}

// 分析帧底部 N 行像素: 输出每行的平均颜色 + 非白占比
static void AnalyzeBottomRows(const char* tag, const std::vector<uint8_t>& bgra, int w, int h, int n_rows) {
    if (bgra.empty() || w <= 0 || h <= 0) { printf("[%s] analyze skip (empty)\n", tag); return; }
    printf("[%s] bottom %d rows (h=%d):\n", tag, n_rows, h);
    for (int r = 0; r < n_rows && r < h; r++) {
        int y = h - 1 - r; // 从底部往上
        long sr = 0, sg = 0, sb = 0;
        int nonwhite = 0;
        const uint8_t* row = &bgra[y * w * 4];
        for (int x = 0; x < w; x++) {
            uint8_t b = row[x * 4], g = row[x * 4 + 1], rd = row[x * 4 + 2];
            sb += b; sg += g; sr += rd;
            if (!(rd > 245 && g > 245 && b > 245)) nonwhite++;
        }
        printf("[%s]   row[-%d] (y=%d): avg=(%ld,%ld,%ld) nonwhite=%d/%d (%.1f%%)\n",
               tag, r + 1, y, sr / w, sg / w, sb / w, nonwhite, w, nonwhite * 100.0 / w);
    }
}

// 分析帧顶部 N 行像素 (用于对比 impress 底部 vs calc 顶部)
static void AnalyzeTopRows(const char* tag, const std::vector<uint8_t>& bgra, int w, int h, int n_rows) {
    if (bgra.empty() || w <= 0 || h <= 0) { printf("[%s] analyze skip (empty)\n", tag); return; }
    printf("[%s] top %d rows (h=%d):\n", tag, n_rows, h);
    for (int r = 0; r < n_rows && r < h; r++) {
        int y = r;
        long sr = 0, sg = 0, sb = 0;
        int nonwhite = 0;
        const uint8_t* row = &bgra[y * w * 4];
        for (int x = 0; x < w; x++) {
            uint8_t b = row[x * 4], g = row[x * 4 + 1], rd = row[x * 4 + 2];
            sb += b; sg += g; sr += rd;
            if (!(rd > 245 && g > 245 && b > 245)) nonwhite++;
        }
        printf("[%s]   row[+%d] (y=%d): avg=(%ld,%ld,%ld) nonwhite=%d/%d (%.1f%%)\n",
               tag, r + 1, y, sr / w, sg / w, sb / w, nonwhite, w, nonwhite * 100.0 / w);
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 4) {
        fprintf(stderr, "usage: %s <xlsx1> <xlsx2> <pptx>\n", argv[0]);
        return 2;
    }

    Session s1, s2, s3;
    s1.tag = "calc1"; s2.tag = "calc2"; s3.tag = "impress";

    // 3 文档并发 Create (与 demo 同构)
    std::thread t1([&] {
        printf("[probe] Create calc1...\n");
        s1.handle = CalcSessionCreate(argv[1], "", "ui1px_c1", CalcCb, &s1, 1920, 1080);
        printf("[probe] calc1 Create %s\n", s1.handle ? "OK" : "FAILED");
        s1.ok = s1.handle != nullptr;
    });
    std::thread t2([&] {
        printf("[probe] Create calc2...\n");
        s2.handle = CalcSessionCreate(argv[2], "", "ui1px_c2", CalcCb, &s2, 1920, 1080);
        printf("[probe] calc2 Create %s\n", s2.handle ? "OK" : "FAILED");
        s2.ok = s2.handle != nullptr;
    });
    std::thread t3([&] {
        printf("[probe] Create impress...\n");
        s3.handle = ImpressSessionCreate(argv[3], "", "ui1px_p", ImpressCb, &s3, 1920, 1080);
        printf("[probe] impress Create %s\n", s3.handle ? "OK" : "FAILED");
        s3.ok = s3.handle != nullptr;
    });
    t1.join(); t2.join(); t3.join();

    if (!s1.ok || !s2.ok || !s3.ok) {
        fprintf(stderr, "[probe] some Create FAILED\n");
        return 1;
    }
    printf("[probe] all created OK\n");

    if (s1.handle) CalcSessionStart(s1.handle);
    if (s2.handle) CalcSessionStart(s2.handle);
    if (s3.handle) ImpressSessionStart(s3.handle);
    printf("[probe] all started, waiting 6s for frames...\n");
    sleep(6);

    printf("\n=== 帧统计 ===\n");
    printf("[calc1]   frames=%d %dx%d\n", s1.frames.load(), s1.w, s1.h);
    printf("[calc2]   frames=%d %dx%d\n", s2.frames.load(), s2.w, s2.h);
    printf("[impress] frames=%d %dx%d\n", s3.frames.load(), s3.w, s3.h);

    // 存盘 BMP
    printf("\n=== 存盘 BMP ===\n");
    SaveBmp("/tmp/ui_1px_calc1.bmp", s1.last, s1.w, s1.h);
    SaveBmp("/tmp/ui_1px_calc2.bmp", s2.last, s2.w, s2.h);
    SaveBmp("/tmp/ui_1px_impress.bmp", s3.last, s3.w, s3.h);

    // 像素分析: impress 底部 + calc 顶部 (对比是否串扰)
    printf("\n=== 像素分析 (底部 5 行) ===\n");
    AnalyzeBottomRows("impress", s3.last, s3.w, s3.h, 5);
    AnalyzeBottomRows("calc1", s1.last, s1.w, s1.h, 5);
    AnalyzeBottomRows("calc2", s2.last, s2.w, s2.h, 5);

    printf("\n=== 像素分析 (顶部 5 行, 对比) ===\n");
    AnalyzeTopRows("calc1", s1.last, s1.w, s1.h, 5);
    AnalyzeTopRows("calc2", s2.last, s2.w, s2.h, 5);
    AnalyzeTopRows("impress", s3.last, s3.w, s3.h, 5);

    // 清理
    if (s1.handle) CalcSessionStop(s1.handle);
    if (s2.handle) CalcSessionStop(s2.handle);
    if (s3.handle) ImpressSessionStop(s3.handle);
    if (s1.handle) CalcSessionDestroy(s1.handle);
    if (s2.handle) CalcSessionDestroy(s2.handle);
    if (s3.handle) ImpressSessionDestroy(s3.handle);
    printf("\n[probe] destroyed all, done\n");
    printf("\n后续 ffmpeg 分析:\n");
    printf("  # 提取 impress 底部 1px 行\n");
    printf("  ffmpeg -f image2 -i /tmp/ui_1px_impress.bmp -vf \"crop=iw:1:0:ih-1\" /tmp/ui_1px_impress_bottom1.bmp\n");
    printf("  # 提取 calc1 任意 1px 行对比\n");
    printf("  ffmpeg -f image2 -i /tmp/ui_1px_calc1.bmp -vf \"crop=iw:1:0:0\" /tmp/ui_1px_calc1_top1.bmp\n");
    printf("  # PSNR 对比 (越高越相似)\n");
    printf("  ffmpeg -i /tmp/ui_1px_impress_bottom1.bmp -i /tmp/ui_1px_calc1_top1.bmp -filter_complex psnr -f null -\n");
    return 0;
}
