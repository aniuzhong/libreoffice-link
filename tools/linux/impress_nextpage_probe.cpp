// impress_nextpage_probe.cpp — 卡死问题复现探针:
// Create -> Start -> 循环 NextPage x20, 每步计时 gotoNextEffect 调用耗时 /
// 抓帧耗时, 定位卡点 (UNO 侧 vs 显示侧 vs 锁)。
#include <abi/abi.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

static int s_frames = 0;
static long long s_last_frame_ms = 0;
static long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static void OnFrame(const uint8_t* data, int32_t w, int32_t h, int32_t rp,
                    int32_t size, int32_t format, void* opaque) {
    (void)data; (void)w; (void)h; (void)rp; (void)size; (void)format; (void)opaque;
    s_frames++;
    s_last_frame_ms = NowMs();
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <pptx> [nexts=20] [interval_ms=2000]\n", argv[0]);
        return 2;
    }
    int n_next = (argc >= 3) ? atoi(argv[2]) : 20;
    int interval = (argc >= 4) ? atoi(argv[3]) : 2000; // 翻页间隔 (动画完成窗口)
    int start_page = (argc >= 5) ? atoi(argv[4]) : 0;  // 起始页 (对照实验)
    int next_mode = (argc >= 6) ? atoi(argv[5]) : 0;   // 0=gotoNextEffect, 1=gotoSlideIndex(页级)

    void* s = ImpressSessionCreate(argv[1], "", "nextpage_probe", OnFrame, nullptr, 1920, 1080);
    if (!s) { fprintf(stderr, "Create FAILED\n"); return 1; }
    printf("Create OK slides=%d\n", ImpressSessionGetPageCount(s));
    int start_mode = (argc >= 7) ? atoi(argv[6]) : 1; // 0=不Start(无轮询线程), 1=Start
    if (start_mode == 1)
        ImpressSessionStart(s);
    if (start_page > 0) {
        ImpressSessionGoToPage(s, start_page);
        usleep(500 * 1000);
    }

    int cur = ImpressSessionGetCurrentPage(s);
    printf("[0] current=%d frames=%d (start_page=%d)\n", cur, s_frames, start_page);

    for (int i = 1; i <= n_next; i++) {
        sleep(interval / 1000);
        int before = s_frames;
        long long t0 = NowMs();
        bool ok;
        int c = ImpressSessionGetCurrentPage(s);
        if (next_mode == 1) {
            ok = c >= 0 && ImpressSessionGoToPage(s, c + 1);
        } else if (next_mode == 2) {
            ok = c >= 0 && ImpressSessionGoToPage(s, c + 3); // 跳 3 页 (跨媒体页)
        } else {
            ok = ImpressSessionNextPage(s);
        }
        long long uno_ms = NowMs() - t0;
        long long t1 = NowMs();
        // 等一帧 (最多 3s)
        while (NowMs() - t1 < 3000 && s_frames == before)
            usleep(10000);
        long long frame_wait = NowMs() - t1;
        int after = ImpressSessionGetCurrentPage(s);
        printf("[%d] gotoNextEffect=%s %lldms 帧等待=%lldms (收到%d帧) current=%d frames=%d\n",
               i, ok ? "OK" : "FAIL", uno_ms, frame_wait, s_frames - before, after, s_frames);
        fflush(stdout);
        if (!ok) {
            printf(">>> 第 %d 次翻页异常, 停止\n", i);
            break;
        }
        if (after < 0) {
            // current 暂不可用: 等待后重查
            sleep(2);
            after = ImpressSessionGetCurrentPage(s);
            printf("    (current 恢复重查: %d)\n", after);
        }
        if (uno_ms > 5000) {
            printf(">>> 第 %d 次翻页 UNO 调用超过 5s, 疑似卡死\n", i);
            break;
        }
    }
    ImpressSessionStop(s);
    ImpressSessionDestroy(s);
    printf("done\n");
    return 0;
}
