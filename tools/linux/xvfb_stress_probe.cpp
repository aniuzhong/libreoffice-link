// xvfb_stress_probe.cpp — Xvfb 共享大屏机制探索 (office_runtime 鲁棒性前置研究)
// 阶段1: 屏幕尺寸上限 (7680/15360/30720 x 1080, 记录成败/内存)
// 阶段2: 抓帧性能 (XGetImage vs XShmGetImage; 全屏 vs 窗口; BGRA 转换开销)
// 阶段3: 多窗口并发抓帧 (4 线程 40ms 轮询 vs 单线程顺序, 耗时/CPU)
// 阶段4: 帧率上限 (单窗口裸抓 1000 次)
// 阶段5: Xvfb 自身资源 (RSS)
// 用法: xvfb_stress_probe [display]
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

static std::string g_dpy = ":96";

static bool WaitForX(const std::string& dpy) {
    for (int i = 0; i < 100; i++) {
        Display* d = XOpenDisplay(dpy.c_str());
        if (d) {
            XCloseDisplay(d);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// 启动 Xvfb (返回 pid; -1 失败)
static pid_t StartXvfb(const char* geometry) {
    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/bin/Xvfb", "Xvfb", g_dpy.c_str(), "-screen", "0", geometry,
              "-nolisten", "tcp", (char*)nullptr);
        _exit(127);
    }
    if (!WaitForX(g_dpy)) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        return -1;
    }
    return pid;
}

static void StopXvfb(pid_t pid) {
    if (pid > 0) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
    }
    // SIGKILL 的 X server 不清理自己的 lock/socket; 残留会让下一次同号启动
    // "server already running" 失败 ( 实测段错误根因)
    unlink(("/tmp/.X" + g_dpy.substr(1) + "-lock").c_str());
    unlink(("/tmp/.X11-unix/X" + g_dpy.substr(1)).c_str());
}

static long XvfbRssKB(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0)
            return std::atol(line.c_str() + 7);
    }
    return -1;
}

static double NowMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 阶段1: 屏幕尺寸上限
static void Phase1() {
    printf("========== 阶段1: 屏幕尺寸上限 ==========\n");
    struct { const char* geom; } tests[] = {
        {"7680x1080x24"}, {"15360x1080x24"}, {"30720x1080x24"},
        {"7680x2160x24"}, {"15360x2160x24"}, {"30720x2160x24"},
    };
    for (auto& t : tests) {
        pid_t pid = StartXvfb(t.geom);
        if (pid < 0) {
            printf("  %-16s -> Xvfb 启动失败\n", t.geom);
            continue;
        }
        Display* d = XOpenDisplay(g_dpy.c_str());
        int w = d ? DisplayWidth(d, DefaultScreen(d)) : -1;
        int h = d ? DisplayHeight(d, DefaultScreen(d)) : -1;
        long rss = XvfbRssKB(pid);
        // 实际抓一帧验证可用
        bool grab_ok = false;
        if (d && w > 0) {
            XImage* img = XGetImage(d, DefaultRootWindow(d), 0, 0, w > 1920 ? 1920 : w,
                                    h > 1080 ? 1080 : h, AllPlanes, ZPixmap);
            grab_ok = img != nullptr;
            if (img)
                XDestroyImage(img);
        }
        if (d)
            XCloseDisplay(d);
        printf("  %-16s -> %dx%d 实际, RSS=%ldMB, 抓帧%s\n", t.geom, w, h,
               rss / 1024, grab_ok ? "OK" : "FAIL");
        StopXvfb(pid);
        unlink(("/tmp/.X" + g_dpy.substr(1) + "-lock").c_str());
    }
}

// 阶段2: 抓帧性能
static void Phase2() {
    printf("\n========== 阶段2: 抓帧性能 (7680x1080 屏) ==========\n");
    pid_t pid = StartXvfb("7680x1080x24");
    if (pid < 0) { printf("Xvfb 失败\n"); return; }
    Display* d = XOpenDisplay(g_dpy.c_str());
    if (!d) { printf("XOpenDisplay(%s) failed\n", g_dpy.c_str()); StopXvfb(pid); return; }
    Window root = DefaultRootWindow(d);

    // 建 4 个窗口 (各自 slot 区域, 填不同颜色)
    std::vector<Window> wins;
    for (int i = 0; i < 4; i++) {
        Window w = XCreateWindow(d, root, i * 1920, 0, 1920, 1080, 0,
                                 CopyFromParent, InputOutput, CopyFromParent, 0, nullptr);
        XMapWindow(d, w);
        XSetForeground(d, DefaultGC(d, DefaultScreen(d)), 0xFF0000 + i * 0x10101);
        XFillRectangle(d, w, DefaultGC(d, DefaultScreen(d)), 0, 0, 1920, 1080);
        wins.push_back(w);
    }
    XSync(d, False);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    auto bench = [&](const char* tag, int reps, auto fn) {
        double t0 = NowMs();
        for (int i = 0; i < reps; i++)
            fn();
        double total = NowMs() - t0;
        printf("  %-38s %4d 次: 总 %7.1fms  均值 %6.2fms\n", tag, reps, total, total / reps);
    };

    // XGetImage 全屏 (7680x1080, 33MB/帧)
    bench("XGetImage 全屏 7680x1080", 30, [&] {
        XImage* img = XGetImage(d, root, 0, 0, 7680, 1080, AllPlanes, ZPixmap);
        XDestroyImage(img);
    });
    // XGetImage 窗口 (1920x1080, 8.3MB/帧)
    bench("XGetImage 窗口 1920x1080", 100, [&] {
        XImage* img = XGetImage(d, wins[0], 0, 0, 1920, 1080, AllPlanes, ZPixmap);
        XDestroyImage(img);
    });

    // XShm 窗口
    if (XShmQueryExtension(d)) {
        XShmSegmentInfo shm;
        XImage* shmimg = XShmCreateImage(d, DefaultVisual(d, DefaultScreen(d)), 24, ZPixmap,
                                         nullptr, &shm, 1920, 1080);
        if (shmimg) {
            shm.shmid = shmget(IPC_PRIVATE, 1920 * 1080 * 4, IPC_CREAT | 0600);
            shm.shmaddr = (char*)shmat(shm.shmid, nullptr, 0);
            shmimg->data = shm.shmaddr;
            XShmAttach(d, &shm);
            bench("XShmGetImage 窗口 1920x1080", 100, [&] {
                XShmGetImage(d, wins[0], shmimg, 0, 0, AllPlanes);
            });
            // XShm 全屏
            XImage* shmfull = XShmCreateImage(d, DefaultVisual(d, DefaultScreen(d)), 24, ZPixmap,
                                              nullptr, &shm, 7680, 1080);
            if (shmfull) {
                shmfull->data = shm.shmaddr;
                bench("XShmGetImage 全屏 7680x1080", 30, [&] {
                    XShmGetImage(d, root, shmfull, 0, 0, AllPlanes);
                });
                XDestroyImage(shmfull);
            }
            XShmDetach(d, &shm);
            shmdt(shm.shmaddr);
            shmctl(shm.shmid, IPC_RMID, nullptr);
            XDestroyImage(shmimg);
        }
    } else {
        printf("  XShm 扩展不可用\n");
    }

    // BGRA 转换开销 (模拟 GrabBgra 的 mask 移位转换)
    XImage* img = XGetImage(d, wins[0], 0, 0, 1920, 1080, AllPlanes, ZPixmap);
    if (img) {
        int bpp = img->bits_per_pixel / 8;
        std::vector<uint8_t> out(1920 * 1080 * 4);
        unsigned long rm = img->red_mask, gm = img->green_mask, bm = img->blue_mask;
        auto shiftOf = [](unsigned long mask) {
            int s = 0;
            while (mask && !(mask & 1)) { mask >>= 1; s++; }
            return s;
        };
        int rs = shiftOf(rm), gs = shiftOf(gm), bs = shiftOf(bm);
        bench("BGRA 转换 1920x1080 (GrabBgra 逻辑)", 100, [&] {
            for (int y = 0; y < 1080; y++) {
                const char* src = img->data + (size_t)y * img->bytes_per_line;
                uint8_t* dst = out.data() + (size_t)y * 1920 * 4;
                for (int x = 0; x < 1920; x++) {
                    uint32_t p = 0;
                    memcpy(&p, src + (size_t)x * bpp, bpp);
                    dst[x * 4 + 0] = (p & bm) >> bs;
                    dst[x * 4 + 1] = (p & gm) >> gs;
                    dst[x * 4 + 2] = (p & rm) >> rs;
                    dst[x * 4 + 3] = 0xFF;
                }
            }
        });
        XDestroyImage(img);
    }
    XCloseDisplay(d);
    StopXvfb(pid);
    unlink(("/tmp/.X" + g_dpy.substr(1) + "-lock").c_str());
}

// 阶段3: 多窗口并发抓帧
static void Phase3() {
    printf("\n========== 阶段3: 多窗口并发抓帧 (40ms 轮询, 持续 3s) ==========\n");
    pid_t pid = StartXvfb("7680x1080x24");
    if (pid < 0) { printf("Xvfb 失败\n"); return; }
    Display* d = XOpenDisplay(g_dpy.c_str());
    if (!d) { printf("XOpenDisplay(%s) failed\n", g_dpy.c_str()); StopXvfb(pid); return; }
    Window root = DefaultRootWindow(d);
    std::vector<Window> wins;
    for (int i = 0; i < 4; i++) {
        Window w = XCreateWindow(d, root, i * 1920, 0, 1920, 1080, 0,
                                 CopyFromParent, InputOutput, CopyFromParent, 0, nullptr);
        XMapWindow(d, w);
        XSetForeground(d, DefaultGC(d, DefaultScreen(d)), 0xFF0000 + i * 0x10101);
        XFillRectangle(d, w, DefaultGC(d, DefaultScreen(d)), 0, 0, 1920, 1080);
        wins.push_back(w);
    }
    XSync(d, False);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 单线程顺序抓 4 窗口 (40ms x 4)
    {
        double t0 = NowMs();
        int total = 0;
        while (NowMs() - t0 < 3000) {
            for (auto w : wins) {
                XImage* img = XGetImage(d, w, 0, 0, 1920, 1080, AllPlanes, ZPixmap);
                XDestroyImage(img);
            }
            total += 4;
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        double el = NowMs() - t0;
        printf("  单线程顺序: %d 帧 (%.0f fps 整体), 每轮 4 帧均值 %.2fms\n",
               total, total / (el / 1000.0), el / (total / 4.0));
    }

    // 4 线程并发, 各自独立 Display (XInitThreads 不必要, 各线程独享 Display)
    {
        std::atomic<int> counters[4];
        for (auto& c : counters)
            c.store(0);
        auto t0 = NowMs();
        std::vector<std::thread> ths;
        for (int i = 0; i < 4; i++) {
            ths.emplace_back([&, i]() {
                Display* td = XOpenDisplay(g_dpy.c_str());
                if (!td)
                    return; // 本线程连接失败, 仅丢该线程计数
                auto wt0 = NowMs();
                while (NowMs() - wt0 < 3000) {
                    XImage* img = XGetImage(td, wins[i], 0, 0, 1920, 1080, AllPlanes, ZPixmap);
                    XDestroyImage(img);
                    counters[i]++;
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
                XCloseDisplay(td);
            });
        }
        for (auto& t : ths)
            t.join();
        double el = NowMs() - t0;
        int sum = 0;
        for (auto& c : counters)
            sum += c.load();
        printf("  4 线程并发: 共 %d 帧 (%.0f fps 整体), 每线程 %d~%d 帧\n",
               sum, sum / (el / 1000.0),
               counters[0].load(), counters[3].load());
    }
    XCloseDisplay(d);
    StopXvfb(pid);
    unlink(("/tmp/.X" + g_dpy.substr(1) + "-lock").c_str());
}

// 阶段4: 帧率上限
static void Phase4() {
    printf("\n========== 阶段4: 帧率上限 (单窗口裸抓, 无 sleep) ==========\n");
    pid_t pid = StartXvfb("7680x1080x24");
    if (pid < 0) { printf("Xvfb 失败\n"); return; }
    Display* d = XOpenDisplay(g_dpy.c_str());
    if (!d) { printf("XOpenDisplay(%s) failed\n", g_dpy.c_str()); StopXvfb(pid); return; }
    Window root = DefaultRootWindow(d);
    Window w = XCreateWindow(d, root, 0, 0, 1920, 1080, 0,
                             CopyFromParent, InputOutput, CopyFromParent, 0, nullptr);
    XMapWindow(d, w);
    XSetForeground(d, DefaultGC(d, DefaultScreen(d)), 0x00FF00);
    XFillRectangle(d, w, DefaultGC(d, DefaultScreen(d)), 0, 0, 1920, 1080);
    XSync(d, False);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    int N = 1000;
    double t0 = NowMs();
    for (int i = 0; i < N; i++) {
        XImage* img = XGetImage(d, w, 0, 0, 1920, 1080, AllPlanes, ZPixmap);
        XDestroyImage(img);
    }
    double el = NowMs() - t0;
    printf("  XGetImage 窗口: %d 帧 / %.0fms = %.0f fps\n", N, el, N / (el / 1000.0));

    // XShm 版本
    if (XShmQueryExtension(d)) {
        XShmSegmentInfo shm;
        XImage* shmimg = XShmCreateImage(d, DefaultVisual(d, DefaultScreen(d)), 24, ZPixmap,
                                         nullptr, &shm, 1920, 1080);
        if (shmimg) {
            shm.shmid = shmget(IPC_PRIVATE, 1920 * 1080 * 4, IPC_CREAT | 0600);
            shm.shmaddr = (char*)shmat(shm.shmid, nullptr, 0);
            shmimg->data = shm.shmaddr;
            XShmAttach(d, &shm);
            t0 = NowMs();
            for (int i = 0; i < N; i++)
                XShmGetImage(d, w, shmimg, 0, 0, AllPlanes);
            el = NowMs() - t0;
            printf("  XShmGetImage 窗口: %d 帧 / %.0fms = %.0f fps\n", N, el, N / (el / 1000.0));
            XShmDetach(d, &shm);
            shmdt(shm.shmaddr);
            shmctl(shm.shmid, IPC_RMID, nullptr);
            XDestroyImage(shmimg);
        }
    }
    XCloseDisplay(d);
    StopXvfb(pid);
    unlink(("/tmp/.X" + g_dpy.substr(1) + "-lock").c_str());
}

// 阶段5: 资源占用 (随窗口数增长)
static void Phase5() {
    printf("\n========== 阶段5: Xvfb 资源 (RSS, 1/2/4 窗口) ==========\n");
    pid_t pid = StartXvfb("7680x1080x24");
    if (pid < 0) { printf("Xvfb 失败\n"); return; }
    Display* d = XOpenDisplay(g_dpy.c_str());
    if (!d) { printf("XOpenDisplay(%s) failed\n", g_dpy.c_str()); StopXvfb(pid); return; }
    Window root = DefaultRootWindow(d);
    for (int n = 1; n <= 4; n++) {
        // 新窗口
        for (int i = 0; i < n; i++) {
            Window w = XCreateWindow(d, root, i * 1920, 0, 1920, 1080, 0,
                                     CopyFromParent, InputOutput, CopyFromParent, 0, nullptr);
            XMapWindow(d, w);
            XSetForeground(d, DefaultGC(d, DefaultScreen(d)), 0xFF0000 + i * 0x10101);
            XFillRectangle(d, w, DefaultGC(d, DefaultScreen(d)), 0, 0, 1920, 1080);
        }
        XSync(d, False);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        long rss = XvfbRssKB(pid);
        printf("  %d 窗口: Xvfb RSS = %ldMB (屏缓冲理论 7680x1080x4 = 33MB)\n", n, rss / 1024);
    }
    XCloseDisplay(d);
    StopXvfb(pid);
    unlink(("/tmp/.X" + g_dpy.substr(1) + "-lock").c_str());
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 2)
        g_dpy = argv[1];
    printf("=== Xvfb 共享大屏机制探索 (display %s, %d 核) ===\n", g_dpy.c_str(), (int)std::thread::hardware_concurrency());
    Phase1();
    Phase2();
    Phase3();
    Phase4();
    Phase5();
    printf("\n=== 探索完成 ===\n");
    return 0;
}
