// impress_multi_probe.cpp — 并发回归: 2 xlsx + 1 pptx 共享 runtime
// 三线程并发 Create (测 office_runtime 共享引导锁 BootMutex):
//   线程A CalcSessionCreate(xlsx1)  线程B CalcSessionCreate(xlsx2)
//   线程C ImpressSessionCreate(pptx)
// 验证: 并发创建不死锁 / 三路帧都流动且含内容 / slot 窗口互不重叠 /
//       impress 翻页生效 / 干净销毁。
#include <base/abi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

// ---- 会话状态 ----
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

static int NonWhitePct(const Session& s) {
    if (s.last.empty()) return -1;
    int nw = 0, total = (int)s.last.size() / 4;
    for (size_t i = 0; i < s.last.size(); i += 4) {
        uint8_t r = s.last[i + 2], g = s.last[i + 1], b = s.last[i];
        if (!(r > 245 && g > 245 && b > 245)) nw++;
    }
    return total ? nw * 100 / total : -1;
}

// ---- X11: 打印所有 viewable 大窗口位置 (验证 slot 隔离) ----
static void DumpWindows(const char* dpy_name) {
    Display* d = XOpenDisplay(dpy_name);
    if (!d) { printf("[x11] open %s failed\n", dpy_name); return; }
    printf("[x11] windows on %s:\n", dpy_name);
    Window root = DefaultRootWindow(d);
    Window rr, pp; Window* kids = nullptr; unsigned n = 0;
    if (XQueryTree(d, root, &rr, &pp, &kids, &n)) {
        for (unsigned i = 0; i < n; i++) {
            XWindowAttributes a;
            if (XGetWindowAttributes(d, kids[i], &a) && a.map_state == IsViewable && a.width > 100) {
                XClassHint ch = {};
                std::string cls;
                if (XGetClassHint(d, kids[i], &ch)) {
                    cls = ch.res_class ? ch.res_class : "";
                    if (ch.res_name) XFree(ch.res_name);
                    if (ch.res_class) XFree(ch.res_class);
                }
                printf("[x11]   0x%lx %dx%d+%d+%d class='%s'\n",
                       kids[i], a.width, a.height, a.x, a.y, cls.c_str());
            }
        }
        if (kids) XFree(kids);
    }
    XCloseDisplay(d);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 4) {
        fprintf(stderr, "usage: %s <xlsx1> <xlsx2> <pptx> [sessions(1-3,默认3)]\n", argv[0]);
        return 2;
    }
    int n_sessions = (argc >= 5) ? atoi(argv[4]) : 3;
    if (n_sessions < 1 || n_sessions > 3) n_sessions = 3;

    Session s1, s2, s3;
    s1.tag = "calc1"; s2.tag = "calc2"; s3.tag = "impress";

    // 多线程并发 Create (共享引导锁串行化, 不死锁即通过)
    std::thread t1([&] {
        printf("[probe] Create calc1...\n");
        s1.handle = CalcSessionCreate(argv[1], "", "multi_impress_c1", CalcCb, &s1, 1920, 1080);
        printf("[probe] calc1 Create %s\n", s1.handle ? "OK" : "FAILED");
        s1.ok = s1.handle != nullptr;
    });
    std::thread t2([&] {
        if (n_sessions < 2) { s2.ok = true; return; }
        printf("[probe] Create calc2...\n");
        s2.handle = CalcSessionCreate(argv[2], "", "multi_impress_c2", CalcCb, &s2, 1920, 1080);
        printf("[probe] calc2 Create %s\n", s2.handle ? "OK" : "FAILED");
        s2.ok = s2.handle != nullptr;
    });
    std::thread t3([&] {
        if (n_sessions < 3) { s3.ok = true; return; }
        printf("[probe] Create impress...\n");
        s3.handle = ImpressSessionCreate(argv[3], "", "multi_impress_p", ImpressCb, &s3, 1920, 1080);
        printf("[probe] impress Create %s\n", s3.handle ? "OK" : "FAILED");
        s3.ok = s3.handle != nullptr;
    });
    t1.join(); t2.join(); t3.join();

    if (!s1.ok || !s2.ok || !s3.ok) {
        fprintf(stderr, "[probe] some Create FAILED\n");
        return 1;
    }
    printf("[probe] all created OK (共享引导锁无死锁)\n");

    if (s1.handle) CalcSessionStart(s1.handle);
    if (s2.handle) CalcSessionStart(s2.handle);
    if (s3.handle) ImpressSessionStart(s3.handle);
    printf("[probe] all started\n");
    sleep(5); // 收集帧

    printf("[probe] calc1: frames=%d %dx%d non-white=%d%%\n",
           s1.frames.load(), s1.w, s1.h, NonWhitePct(s1));
    if (s2.handle) printf("[probe] calc2: frames=%d %dx%d non-white=%d%%\n",
           s2.frames.load(), s2.w, s2.h, NonWhitePct(s2));
    if (s3.handle) printf("[probe] impress: frames=%d %dx%d non-white=%d%%\n",
           s3.frames.load(), s3.w, s3.h, NonWhitePct(s3));

    // impress 页数与翻页
    if (s3.handle) {
        int slides = ImpressSessionGetPageCount(s3.handle);
        int cur = ImpressSessionGetCurrentPage(s3.handle);
        printf("[probe] impress slides=%d current=%d\n", slides, cur);
        std::vector<uint8_t> before = s3.last;
        ImpressSessionNextPage(s3.handle);
        sleep(2);
        size_t diff = 0;
        for (size_t i = 0; i + 4 <= before.size() && i + 4 <= s3.last.size(); i += 4) {
            if (memcmp(&before[i], &s3.last[i], 3) != 0) diff++;
        }
        size_t total = s3.last.size() / 4;
        printf("[probe] impress next-page diff: %.1f%%\n", total ? diff * 100.0 / total : 0.0);
    }

    // xlsx 页数 (sheet)
    printf("[probe] calc1 sheets=%d%s\n",
           CalcSessionGetSheetCount(s1.handle),
           s2.handle ? (std::string(" calc2 sheets=") + std::to_string(CalcSessionGetSheetCount(s2.handle))).c_str() : "");

    DumpWindows(getenv("DISPLAY") ? getenv("DISPLAY") : ":90");

    if (s1.handle) CalcSessionStop(s1.handle);
    if (s2.handle) CalcSessionStop(s2.handle);
    if (s3.handle) ImpressSessionStop(s3.handle);
    if (s1.handle) CalcSessionDestroy(s1.handle);
    if (s2.handle) CalcSessionDestroy(s2.handle);
    if (s3.handle) ImpressSessionDestroy(s3.handle);
    printf("[probe] destroyed all, done\n");
    return 0;
}
