// ffplay_window_size_probe.cpp — ffplay 嵌入引擎窗口尺寸链验证 (严格例证)
// 直接调 ffplay_engine API (不经过 LO/soffice), 验证 is->width/is->height
// 是否等于外部 X11 窗口实际尺寸 (而非硬编码 640x480)。
//
// 三方对比:
//   期望 = 探针创建的 X11 窗口尺寸 (命令行指定, 故意非 640x480 以暴露 bug)
//   X11  = XGetWindowAttributes 实测 (X11 真相)
//   ffplay = ffplay_engine_get_window_size 读取的 is->width/is->height
//
// 判定:
//   bug  (修复前): ffplay == 640x480 ≠ 期望/X11
//   pass (修复后): ffplay == 期望 == X11
//
// 用法: ffplay_window_size_probe <media> [w=500] [h=300]
// 链接: compat/ffplay_embed.c (FFPLAY_EMBED) + cmdutils.c + Nova 库
// 注: 原 impress 组版本 (通过 ImpressSession 间接) 无法读取 ffplay 内部
//     is->width/height (handle 在 LO avmedia 内部), 故改为 engine 组直接验证。
//     ffplay 侧 bug 与 LO/pptx 无关 (LO 侧尺寸链已验证正确), 直接验证引擎即可。
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>

// 引擎 C API (compat/ffplay_embed.c, FFPLAY_EMBED 下导出)
#include "ffplay_engine.h"

static Display *g_dpy = nullptr;

// 建一个 X 窗口作为播放目标 (模拟 LO 媒体子窗口的父窗口, 与 LO aArgs[0] 语义一致)
static Window MakeWindow(int w, int h) {
    Window root = DefaultRootWindow(g_dpy);
    Window win = XCreateSimpleWindow(g_dpy, root, 0, 0, w, h, 0,
                                     BlackPixel(g_dpy, DefaultScreen(g_dpy)),
                                     WhitePixel(g_dpy, DefaultScreen(g_dpy)));
    XMapWindow(g_dpy, win);
    XSync(g_dpy, False);
    return win;
}

// 实测 X11 窗口尺寸 (XGetWindowAttributes 真相)
static bool QueryX11Size(Window win, int *w, int *h) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(g_dpy, win, &a)) return false;
    *w = a.width;
    *h = a.height;
    return true;
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <media> [w=500] [h=300]\n", argv[0]);
        fprintf(stderr, "  w/h 故意默认非 640x480, 以暴露硬编码 bug\n");
        return 2;
    }
    const char *url = argv[1];
    int want_w = (argc >= 3) ? atoi(argv[2]) : 500;
    int want_h = (argc >= 4) ? atoi(argv[3]) : 300;

    g_dpy = XOpenDisplay(nullptr);
    if (!g_dpy) {
        fprintf(stderr, "XOpenDisplay failed (DISPLAY=%s)\n", getenv("DISPLAY"));
        return 1;
    }

    printf("=== ffplay 嵌入引擎窗口尺寸链验证 ===\n");
    printf("期望尺寸 (探针创建): %dx%d\n", want_w, want_h);

    // 1. 创建 X11 窗口
    Window win = MakeWindow(want_w, want_h);
    int x11_w = 0, x11_h = 0;
    QueryX11Size(win, &x11_w, &x11_h);
    printf("X11 窗口实测:        %dx%d\n", x11_w, x11_h);

    // 2. 创建 ffplay 引擎并播放 (触发 video_open 设置 is->width/height)
    void *e = ffplay_engine_create(url, (void*)win);
    if (!e) {
        fprintf(stderr, "ffplay_engine_create FAILED\n");
        return 1;
    }
    ffplay_engine_play(e);
    printf("engine created + play, duration=%.2fs\n", ffplay_engine_get_duration(e));

    // 3. 等待 video_open 执行 (pump 线程渲染循环首次调用, is->width==0 触发)
    //    video_open 在 video_display 内 (is->width==0 时), play 后渲染循环启动,
    //    等 3 秒确保至少一次 video_display → video_open 执行
    int fw0 = 0, fh0 = 0;
    ffplay_engine_get_window_size(e, &fw0, &fh0);
    printf("play 即刻 ffplay 内部: %dx%d (video_open 可能未执行)\n", fw0, fh0);

    for (int i = 1; i <= 6; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        int fw = 0, fh = 0;
        ffplay_engine_get_window_size(e, &fw, &fh);
        printf("  +%dms ffplay 内部: %dx%d\n", i * 500, fw, fh);
        if (fw != 0) break;  // video_open 已执行
    }

    // 4. 读取最终 ffplay 内部尺寸
    int fp_w = 0, fp_h = 0;
    ffplay_engine_get_window_size(e, &fp_w, &fp_h);

    // 5. 三方对比 + 判定
    printf("\n=== 三方对比 ===\n");
    printf("  期望 (探针创建): %dx%d\n", want_w, want_h);
    printf("  X11  (实测):     %dx%d\n", x11_w, x11_h);
    printf("  ffplay (内部):   %dx%d\n", fp_w, fp_h);

    bool x11_match = (x11_w == want_w && x11_h == want_h);
    bool ffplay_match = (fp_w == x11_w && fp_h == x11_h);
    bool is_hardcoded = (fp_w == 640 && fp_h == 480);

    printf("\n=== 判定 ===\n");
    printf("  X11 == 期望:     %s\n", x11_match ? "YES" : "NO (X11 创建异常)");
    printf("  ffplay == X11:   %s\n", ffplay_match ? "YES (尺寸链一致)" : "NO");
    if (is_hardcoded) {
        printf("  ffplay == 640x480 硬编码: YES (BUG 确认)\n");
        printf("结论: BUG — ffplay video_open 用 default_width/height (640x480)\n");
        printf("      而非 SDL_GetWindowSize 读取实际窗口尺寸\n");
    } else if (ffplay_match) {
        printf("  ffplay == 640x480 硬编码: NO\n");
        printf("结论: PASS — ffplay 内部尺寸与 X11 窗口一致, 硬编码已修复\n");
    } else {
        printf("  ffplay == 640x480 硬编码: NO (但也不匹配 X11)\n");
        printf("结论: 异常 — ffplay 内部尺寸非 640x480 也非 X11 尺寸, 需排查\n");
    }

    ffplay_engine_destroy(e);
    XDestroyWindow(g_dpy, win);
    XCloseDisplay(g_dpy);
    return 0;
}
