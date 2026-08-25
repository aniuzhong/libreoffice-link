// xvfb_platform.cpp — Linux 平台实现 (Xvfb 共享内核模式)。
// 迁移自 calc/linux/calc_platform.cpp 与 impress/linux/impress_platform.cpp 的
// 公共侧 (两版 77% 重复), 文档类型差异由规则参数化表达。日志前缀统一走
// OfficeLog("[<tag>] ..."), tag 由构造传入。
#include "xvfb_platform.h"

#include <dlfcn.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace {

// 本 link 所在目录 (== soffice program dir when deployed)
std::string GetLinkDirImpl() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&GetLinkDirImpl), &info) && info.dli_fname) {
        std::string p = info.dli_fname;
        auto pos = p.rfind('/');
        return pos == std::string::npos ? std::string(".") : p.substr(0, pos);
    }
    return ".";
}

// 等待 Xvfb 显示可达
bool WaitForX(const std::string& dpy) {
    for (int i = 0; i < 50; i++) {
        Display* d = XOpenDisplay(dpy.c_str());
        if (d) {
            XCloseDisplay(d);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// 窗口树扫描 (仅收集有名称/class 的 viewable 窗口)
void ScanTree(Display* d, Window win, std::vector<WinInfo>& out) {
    Window root, parent;
    Window* kids = nullptr;
    unsigned n = 0;
    if (!XQueryTree(d, win, &root, &parent, &kids, &n))
        return;
    for (unsigned i = 0; i < n; i++) {
        XWindowAttributes a;
        if (XGetWindowAttributes(d, kids[i], &a) &&
            a.map_state == IsViewable && a.width > 0 && a.height > 0) {
            WinInfo info;
            info.wid = kids[i];
            info.w = a.width;
            info.h = a.height;
            char* nm = nullptr;
            XFetchName(d, kids[i], &nm);
            if (nm) {
                info.name = nm;
                XFree(nm);
            }
            XClassHint ch = {};
            if (XGetClassHint(d, kids[i], &ch)) {
                if (ch.res_name) {
                    info.cls = ch.res_name;
                    XFree(ch.res_name);
                }
                if (ch.res_class) {
                    XFree(ch.res_class);
                }
            }
            if (!info.name.empty() || !info.cls.empty())
                out.push_back(std::move(info));
        }
        ScanTree(d, kids[i], out);
    }
    if (kids)
        XFree(kids);
}

// XGetImage 失败会使默认 Xlib 处理器退出进程; 捕获。
static std::atomic<int> s_xerr_code{0};
static int XErrCapture(Display*, XErrorEvent* e) {
    s_xerr_code = e->error_code;
    OfficeLogWarn("[Common.X11] X error captured: opcode=%d code=%d resource=0x%lx",
              e->request_code, e->error_code, e->resourceid);
    return 0;
}

// BGRX 直拷判定: Xvfb TrueColor 24bpp 视觉, 32bpp LSBFirst 容器,
// 内存布局天然 = B,G,R,X —— memcpy + alpha 填充即可, 免逐像素转换
// (实测: 2160p 转换 ~6ms vs 直拷 ~1ms 级)。
bool IsBgrxDirect(const XImage* img) {
    return img->bits_per_pixel == 32 && img->byte_order == LSBFirst &&
           img->red_mask == 0xFF0000 && img->green_mask == 0xFF00 &&
           img->blue_mask == 0xFF && img->bytes_per_line == static_cast<int>(img->width) * 4;
}

// mask 位偏移计算 (BGRX 像素布局转换; GrabBgra 快路径/回退路径共用)
auto shiftOf = [](unsigned long mask) {
    int s = 0;
    while (mask && !(mask & 1)) { mask >>= 1; s++; }
    return s;
};

// 抓取窗口为 BGRA。首选 XShm (零拷贝抓取) + 字节序直拷;
// XShm 不可用/失败时回退 XGetImage + mask 转换。
bool GrabBgra(Display* d, Window w, std::vector<uint8_t>& out, int& ow, int& oh,
              ShmState& shm) {
    XWindowAttributes a;
    if (!XGetWindowAttributes(d, w, &a) || a.width <= 0 || a.height <= 0) {
        OfficeLogErr("[Common.X11] GrabBgra: XGetWindowAttributes failed win=0x%lx", w);
        return false;
    }
    int gw = a.width;
    int gh = a.height;

    // ---- 快路径: XShm + 直拷 ----
    if (shm.Ensure(d, w, gw, gh) && shm.img) {
        s_xerr_code = 0;
        XErrorHandler old = XSetErrorHandler(XErrCapture);
        bool ok = XShmGetImage(d, w, shm.img, 0, 0, AllPlanes);
        XSync(d, False);
        XSetErrorHandler(old);
        if (ok && !s_xerr_code) {
            out.resize(static_cast<size_t>(gw) * gh * 4);
            if (IsBgrxDirect(shm.img)) {
                // BGRX 直拷 + alpha 置 FF
                memcpy(out.data(), shm.img->data, out.size());
                uint32_t* dst = reinterpret_cast<uint32_t*>(out.data());
                size_t n = out.size() / 4;
                for (size_t i = 0; i < n; i++)
                    dst[i] |= 0xFF000000;
            } else {
                // 非预期布局: 逐像素 mask 转换
                unsigned long rm = shm.img->red_mask, gm = shm.img->green_mask, bm = shm.img->blue_mask;
                int rs = shiftOf(rm), gs = shiftOf(gm), bs = shiftOf(bm);
                for (int y = 0; y < gh; y++) {
                    const char* src = shm.img->data + (size_t)y * shm.img->bytes_per_line;
                    uint8_t* dst = out.data() + (size_t)y * gw * 4;
                    for (int x = 0; x < gw; x++) {
                        uint32_t p = 0;
                        memcpy(&p, src + (size_t)x * 4, 4);
                        dst[x * 4 + 0] = (p & bm) >> bs;
                        dst[x * 4 + 1] = (p & gm) >> gs;
                        dst[x * 4 + 2] = (p & rm) >> rs;
                        dst[x * 4 + 3] = 0xFF;
                    }
                }
            }
            ow = gw;
            oh = gh;
            return true;
        }
        OfficeLogWarn("[Common.X11] GrabBgra: XShmGetImage failed win=0x%lx %dx%d, fallback XGetImage",
                  w, gw, gh);
        shm.available = false; // 段异常, 本会话回退 XGetImage
        shm.Release();
    }

    // ---- 回退路径: XGetImage + mask 转换 ----
    s_xerr_code = 0;
    XErrorHandler old = XSetErrorHandler(XErrCapture);
    XImage* img = XGetImage(d, w, 0, 0, gw, gh, AllPlanes, ZPixmap);
    XSync(d, False); // let async errors arrive
    XSetErrorHandler(old);
    if (s_xerr_code || !img) {
        OfficeLogErr("[Common.X11] GrabBgra: XGetImage failed win=0x%lx %dx%d at(%d,%d) map=%d depth=%d",
                  w, gw, gh, a.x, a.y, static_cast<int>(a.map_state), a.depth);
        if (img)
            XDestroyImage(img);
        return false;
    }

    unsigned long rm = a.visual->red_mask;
    unsigned long gm = a.visual->green_mask;
    unsigned long bm = a.visual->blue_mask;
    int rs = shiftOf(rm), gs = shiftOf(gm), bs = shiftOf(bm);
    int bpp = img->bits_per_pixel, nbytes = bpp / 8;
    if (nbytes < 3)
        nbytes = 3;

    out.resize(static_cast<size_t>(gw) * gh * 4);
    for (int y = 0; y < gh; y++) {
        const char* src = img->data + static_cast<size_t>(y) * img->bytes_per_line;
        uint8_t* dst = out.data() + static_cast<size_t>(y) * gw * 4;
        for (int x = 0; x < gw; x++) {
            uint32_t p = 0;
            memcpy(&p, src + static_cast<size_t>(x) * nbytes, nbytes);
            dst[x * 4 + 0] = static_cast<uint8_t>((p & bm) >> bs); // B
            dst[x * 4 + 1] = static_cast<uint8_t>((p & gm) >> gs); // G
            dst[x * 4 + 2] = static_cast<uint8_t>((p & rm) >> rs); // R
            dst[x * 4 + 3] = 0xFF;                                 // A
        }
    }
    XDestroyImage(img);
    ow = gw;
    oh = gh;
    return true;
}

}  // namespace

// ---- LinuxBootSection (平台隔离设计: [platform-isolation] Part 2 D) ----
// BootLock 构造函数本身即 Lock() (office_runtime.cpp BootLock::BootLock),
// 成员构造时已持锁 —— 此处再调 Lock() 会同线程二次 lock 非递归 s_proc_mutex,
// 立即自死锁 ( 探针卡死根因, 经验 43)。
LinuxBootSection::LinuxBootSection() = default;

void LinuxBootSection::Release() {
    if (!released_) {
        boot_lock_.Unlock();
        released_ = true;
    }
}

// ---- ShmState (头文件声明, 实现在此) ----
void ShmState::Release() {
    if (img && seg.shmaddr && dpy) {
        XShmDetach(dpy, &seg);
        shmdt(seg.shmaddr);
        shmctl(seg.shmid, IPC_RMID, nullptr);
    }
    if (img)
        XDestroyImage(img);
    img = nullptr;
    w = h = 0;
    dpy = nullptr;
}

bool ShmState::Ensure(Display* d, Window win, int width, int height) {
    if (!probed) {
        probed = true;
        dpy = d;
        available = XShmQueryExtension(d);
    }
    if (!available || width <= 0 || height <= 0)
        return false;
    if (img && w == width && h == height)
        return true;
    Release();
    XWindowAttributes a;
    if (!XGetWindowAttributes(d, win, &a))
        return false;
    img = XShmCreateImage(d, a.visual, a.depth, ZPixmap, nullptr, &seg, width, height);
    if (!img)
        return false;
    seg.shmid = shmget(IPC_PRIVATE, img->bytes_per_line * height, IPC_CREAT | 0600);
    if (seg.shmid < 0) {
        XDestroyImage(img);
        img = nullptr;
        return false;
    }
    seg.shmaddr = static_cast<char*>(shmat(seg.shmid, nullptr, 0));
    if (seg.shmaddr == reinterpret_cast<char*>(-1)) {
        shmctl(seg.shmid, IPC_RMID, nullptr);
        XDestroyImage(img);
        img = nullptr;
        return false;
    }
    seg.readOnly = False;
    img->data = seg.shmaddr;
    XShmAttach(d, &seg);
    w = width;
    h = height;
    return true;
}

// ---- XvfbSessionPlatform ----
XvfbSessionPlatform::XvfbSessionPlatform(const WindowMatchRule& rule, const char* tag)
    : rule_(rule), tag_(tag) {}

// ---- 平台隔离设计新增接口 ([platform-isolation] Part 2 D) ----
SessionPlan XvfbSessionPlatform::Plan() {
    // 文档类型差异数据化: Tag() == "calc"/"impress" (构造参数)
    // Linux 共通 (经验 1/26/27): 共享内核 → terminate_on_destroy=false;
    //   窗口化+slot (fullscreen=false); discover=AfterReveal (setVisible 后窗口才创建)
    // 差异:
    //   impress: form=AfterStart (P7 放映 start 后 slot 落位, LO 已铺满屏);
    //            settle_ms=2500 (放映启动形态稳定); ui_hide=true
    //   calc: form=AfterReveal (无放映段, setVisible 后直接落位);
    //         settle_ms=0 (无放映启动); ui_hide=true
    SessionPlan plan;
    plan.discover = WindowPoint::AfterReveal;
    plan.fullscreen = false;
    plan.ui_hide_needed = true;
    plan.terminate_on_destroy = false;
    const bool is_impress = (std::string(Tag()) == "impress");
    if (is_impress) {
        plan.form = WindowPoint::AfterStart;
        plan.settle_ms = 2500;
    } else {
        plan.form = WindowPoint::AfterReveal;
        plan.settle_ms = 0;
    }
    OfficeLog("[%s] Plan: discover=%d form=%d fullscreen=%d settle_ms=%d ui_hide=%d terminate=%d",
              Tag(), static_cast<int>(plan.discover), static_cast<int>(plan.form),
              plan.fullscreen, plan.settle_ms, plan.ui_hide_needed, plan.terminate_on_destroy);
    return plan;
}

std::unique_ptr<BootSection> XvfbSessionPlatform::BeginBoot() {
    return std::make_unique<LinuxBootSection>();
}

bool XvfbSessionPlatform::DiscoverWindow() {
    return FindWindow(); // 复用现有实现
}

bool XvfbSessionPlatform::FormWindow(int w, int h) {
    return SizeWindowToSlot(w, h); // 复用现有实现
}

std::string XvfbSessionPlatform::GetLinkDir() {
    return GetLinkDirImpl();
}

// Linux: per-session profile 无消费方 (共享内核用 office_paths::xvfb_profile,
// 独立 profile 仅 Windows bootstrap 使用; 原实现为死代码,  清理)。
std::string XvfbSessionPlatform::GetProfileDir(const std::string&) {
    return std::string();
}

std::string XvfbSessionPlatform::PrepareEnvironment(const std::string& link_dir,
                                                    const std::string& guid) {
    (void)guid;
    // Xlib 默认非线程安全: 轮询线程与创建线程共用 Display, 须启用线程锁
    static std::once_flag xinit_flag;
    std::call_once(xinit_flag, []() { XInitThreads(); });
    // 共享运行时 (Xvfb + LO 内核 + slot): 首个 session Acquire 创建, 末个释放
    OfficeRuntimeConfig cfg; // 默认值见 runtime.h (max_docs=8, 3840×2160)
    if (!runtime_.Acquire(cfg)) {
        OfficeLogErr("[%s] Acquire failed", Tag());
        return {};
    }
    display_ = runtime_.display();
    // XOpenDisplay 可能因 Xvfb 短暂未就绪失败, 重试
    constexpr int kXOpenMaxRetries = 5;
    constexpr int kXOpenRetryMs = 300;
    for (int i = 0; i < kXOpenMaxRetries && !dpy_; i++) {
        dpy_ = XOpenDisplay(display_.c_str());
        if (!dpy_) {
            OfficeLogWarn("[%s] XOpenDisplay(%s) failed, retry %d", Tag(), display_.c_str(), i + 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(kXOpenRetryMs));
        }
    }
    if (!dpy_) {
        OfficeLogErr("[%s] XOpenDisplay(%s) failed after retries", Tag(), display_.c_str());
        runtime_.Release();
        return {};
    }
    slot_ = runtime_.AllocSlot(); // 本 session 的屏幕分区 (窗口放到该区域, 互不重叠)
    if (slot_ < 0) {
        OfficeLogWarn("[%s] slot exhausted (max %d docs)", Tag(), cfg.max_docs);
        XCloseDisplay(static_cast<Display*>(dpy_));
        dpy_ = nullptr;
        runtime_.Release();
        return {};
    }
    return {}; // bootstrap desktop name (none on Linux)
}

css::uno::Reference<css::uno::XComponentContext> XvfbSessionPlatform::EnsureKernel() {
    if (!runtime_.EnsureKernel())
        return nullptr;
    return runtime_.kernel();
}

void XvfbSessionPlatform::SnapshotWindows() {
    preexisting_.clear();
    Display* d = static_cast<Display*>(dpy_);
    if (!d)
        return;
    std::vector<WinInfo> wins;
    ScanTree(d, DefaultRootWindow(d), wins);
    for (const auto& w : wins)
        if (MatchWindow(w))
            preexisting_.insert(w.wid);
}

bool XvfbSessionPlatform::FindWindow() {
    Display* d = static_cast<Display*>(dpy_);
    if (!d) {
        OfficeLogErr("[%s] FindWindow: dpy_ is NULL (XOpenDisplay failed earlier?)", Tag());
        return false;
    }
    // 新窗口检测: 本 session 的窗口是快照之后新出现的匹配窗口
    constexpr int kFindWinMaxRetries = 300;
    constexpr int kFindWinRetryMs = 200;
    constexpr int kFindWinLogEvery = 10;
    for (int i = 0; i < kFindWinMaxRetries && !win_; i++) {
        std::vector<WinInfo> wins;
        ScanTree(d, DefaultRootWindow(d), wins);
        if (i % kFindWinLogEvery == 0)
            OfficeLogDbg("[%s] FindWindow scan %d: %zu windows, preexisting %zu",
                      Tag(), i, wins.size(), preexisting_.size());
        for (const auto& w : wins) {
            if (MatchWindow(w) &&
                preexisting_.find(w.wid) == preexisting_.end()) {
                win_ = w.wid;
                OfficeLog("[%s] new window 0x%lx %dx%d (title bytes: \"%.60s\")",
                          Tag(), w.wid, w.w, w.h, w.name.c_str());
                break;
            }
        }
        if (!win_)
            std::this_thread::sleep_for(std::chrono::milliseconds(kFindWinRetryMs));
    }
    return win_ != 0;
}

bool XvfbSessionPlatform::SizeWindowToSlot(int width, int height) {
    Display* d = static_cast<Display*>(dpy_);
    if (!d || !win_)
        return false;
    // 窗口放到本 session 的子屏位内: 位左上角 (slot*max_doc_width, 0),
    // 尺寸 = 文档输出分辨率 (≤ 位尺寸)。多 session 窗口互不重叠 ->
    // 抓帧无遮挡; 同时避免 LO 默认偏移 (如 (1,1)) 越出屏幕 BadMatch。
    const OfficeRuntimeConfig& cfg = runtime_.config();
    int x = slot_ * cfg.max_doc_width;
    int w = width > 0 ? width : cfg.max_doc_width;
    int h = height > 0 ? height : cfg.max_doc_height;
    if (w > cfg.max_doc_width) w = cfg.max_doc_width;
    if (h > cfg.max_doc_height) h = cfg.max_doc_height;
    XMoveWindow(d, win_, x, 0);
    XResizeWindow(d, win_, w, h);
    XSync(d, False);

    // 透显缺陷修复 (探针 bleed_probe 实证): LO 文档窗口存在未绘制区 (如 impress
    // 幻灯片窗口底部 37px 未被幻灯片覆盖, 见 [impress-bleed]),
    // 在无 backing store 的 Xvfb 上该区反射底层内容 —— calc 引导期曾以全屏
    // (30720x2160)渲染表格栅格, 其残留在共享大屏底层, 使 impress 帧底部透显出
    // xlsx 栅格。修复 = 设显式背景(黑)并重映射, 触发 LO 重绘其内容区,
    // 未绘制区落黑而非透显底层。
    // ORT_BLEED_FIX=0 可关闭 (仅诊断/回归 A/B 用, bleed_probe 基线测量依赖)。
    if (!(getenv("ORT_BLEED_FIX") && strcmp(getenv("ORT_BLEED_FIX"), "0") == 0)) {
        XSetWindowBackground(d, win_, BlackPixel(d, DefaultScreen(d)));
        XClearWindow(d, win_);   // 清掉已污染的未绘制区像素, 落黑
        XUnmapWindow(d, win_);
        XMapWindow(d, win_);     // 触发 Expose, LO 重绘文档内容区
        XSync(d, False);
    }
    // 布局延迟由帧泵消化 (同 SetWindowSize); 仅诊断模式需等布局完成才有意义
    if (getenv("ORT_DUMP_WINDOWS")) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        runtime_.CheckWindowOverlap();
        runtime_.DumpWindowEdges();
    }
    // 边圈黑化推迟到首帧 (edges_blackout_pending_): 本函数跑在 P8 (UI 隐藏 P9 之前),
    // P9 的 setMenuBar/hideElement 重排版会让 VCL 把顶行重新刷白 —— 实测 X 层清了
    // 2s 内即被复原; 首帧抓取发生在 P10 (P9 之后), 时序天然正确。
    edges_blackout_pending_ = true;
    return true;
}

bool XvfbSessionPlatform::SetWindowSize(int width, int height) {
    Display* d = static_cast<Display*>(dpy_);
    if (!d || !win_)
        return false;
    const OfficeRuntimeConfig& cfg = runtime_.config();
    if (width <= 0 || height <= 0 || width > cfg.max_doc_width || height > cfg.max_doc_height)
        return false;
    XResizeWindow(d, win_, width, height);
    XSync(d, False);
    edges_blackout_pending_ = true; // 改尺寸后边圈重露, 下次首帧再黑化
    // XSync 返回时窗口尺寸已生效 (Xvfb 同步模式)。LO 内核的重排版是异步的,
    // 由帧泵下一轮 CaptureFrame 自然消化 — 排版未完时抓到半成品帧, tick 后
    // 再抓即排完。此处不再 sleep 等待 (原 200ms 无实证依据, V2 卡死根因;
    // 帧泵轮询是正确的同步机制, 不需要 sleep 替代)。
    return true;
}

bool XvfbSessionPlatform::CaptureFrame(uint8_t*& pixels, int& width, int& height) {
    Display* d = static_cast<Display*>(dpy_);
    if (!d || !win_)
        return false;
    // 边圈黑化 (首帧/改尺寸后一次;  透显缺陷收尾):
    // VCL 框架内缩圈 (左2/顶1px) 是放映内容外的最后未绘制区 —— 顶行由 VCL 在
    // 每次曝光时主动刷白 (黑模板上呈 1px 白线), 左圈在窗口出生于其他文档之上时
    // 吸附外来像素 (透显残影)。LO 侧无解 (SetBackground 换不动曝光重绘, 负坐标
    // 平移被父矩形裁剪); X 层"无曝光清法"可持有: 重设背景像素 + 清边圈不产生
    // Expose, VCL 不知情故不重绘, 实测长期持有。放首帧执行 = P9 UI 隐藏之后,
    // 避开 P9 重排版的重新刷白窗口期。
    if (edges_blackout_pending_) {
        XWindowAttributes a;
        if (XGetWindowAttributes(d, win_, &a) && a.width > 2 && a.height > 2) {
            XSetWindowBackground(d, win_, BlackPixel(d, DefaultScreen(d)));
            XClearArea(d, win_, 0, 0, a.width, 1, False);           // 顶
            XClearArea(d, win_, 0, 0, 1, a.height, False);          // 左
            XClearArea(d, win_, a.width - 1, 0, 1, a.height, False); // 右
            XClearArea(d, win_, 0, a.height - 1, a.width, 1, False); // 底
            XSync(d, False);
            OfficeLogDbg("[%s] edges blackout applied to 0x%lx (%dx%d)",
                         Tag(), (unsigned long)win_, a.width, a.height);
        }
        edges_blackout_pending_ = false;
    }
    // 窗口位于独立 slot 区域 (互不重叠), 抓帧始终返回真实内容。
    // XShm + 字节序直拷优先, 失败回退 XGetImage (见 GrabBgra)。
    int w = 0, h = 0;
    if (!GrabBgra(d, win_, cap_bgra_, w, h, shm_))
        return false;
    pixels = cap_bgra_.data();
    width = w;
    height = h;
    return true;
}

// 递归收集 win 下所有子窗口 ID (含多层子窗口, 不限直接子)
static void CollectSubtreeIds(Display* d, Window win, std::vector<long>& out) {
    Window root, parent;
    Window* kids = nullptr;
    unsigned int n = 0;
    if (!XQueryTree(d, win, &root, &parent, &kids, &n) || n == 0) {
        if (kids) XFree(kids);
        return;
    }
    for (unsigned int i = 0; i < n; i++) {
        out.push_back(static_cast<long>(kids[i]));
        CollectSubtreeIds(d, kids[i], out);  // 递归子树
    }
    XFree(kids);
}

// 静音专项 (方案 A, ): 枚举本 session 放映主窗口 win_ 下的所有子窗口 ID,
// 用于 ffplay per-window 精确静音隔离 (子进程内 Manager 按 window_id 匹配引擎)。
// 跨进程可见: soffice.bin 子进程内 LO 为媒体 shape 创建的 X11 子窗口, parent 链
// 必然回溯到本 session 的放映主窗口 win_ (同一 Xvfb X server)。
std::vector<long> XvfbSessionPlatform::GetMediaWindowIds() {
    std::vector<long> ids;
    Display* d = static_cast<Display*>(dpy_);
    if (!d || !win_) {
        OfficeLogDbg("[%s] GetMediaWindowIds: dpy_/win_ NULL, returning empty", Tag());
        return ids;
    }
    CollectSubtreeIds(d, win_, ids);
    OfficeLogDbg("[%s] GetMediaWindowIds: %zu child windows under 0x%lx",
                 Tag(), ids.size(), static_cast<long>(win_));
    return ids;
}

void XvfbSessionPlatform::Cleanup() {
    shm_.Release();
    if (dpy_) {
        XCloseDisplay(static_cast<Display*>(dpy_));
        dpy_ = nullptr;
    }
    win_ = 0;
    cap_bgra_.clear();
    preexisting_.clear();
    runtime_.FreeSlot(slot_);
    runtime_.Release();
}

bool XvfbSessionPlatform::MatchWindow(const WinInfo& w) const {
    if (w.w < rule_.min_w || w.h < rule_.min_h)
        return false;
    for (const auto& k : rule_.cls_keywords)
        if (w.cls.find(k) != std::string::npos)
            return true;
    for (const auto& k : rule_.name_keywords)
        if (w.name.find(k) != std::string::npos)
            return true;
    return false;
}
