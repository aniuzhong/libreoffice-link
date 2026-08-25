// writer_session.cpp — Writer 会话实现 (自治 PDF 位图管线, 见 writer_session.h,
// 方案与落地决策见 HANDOFF 经验 38)。
#include "session.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "md5.h"
#include <base/office_paths.h> // .office-link 路径统一 (header-only, 零依赖)
#include <base/link_utils.h>   // u2s/s2u/kFrameFormatBGRA/to_path/KernelHost (跨平台会话工具)
#include <base/log.h> // OfficeLog (Linux 实现 office_runtime, Windows 实现 common)

#include <osl/file.hxx>
#include <com/sun/star/awt/Size.hpp>
#include <com/sun/star/awt/XBitmap.hpp>
#include <com/sun/star/beans/PropertyValue.hpp>
#include <com/sun/star/drawing/XDrawPage.hpp>
#include <com/sun/star/drawing/XDrawPages.hpp>
#include <com/sun/star/drawing/XDrawPagesSupplier.hpp>
#include <com/sun/star/drawing/XSlideRenderer.hpp>
#include <com/sun/star/frame/XComponentLoader.hpp>
#include <com/sun/star/frame/XDesktop.hpp>
#include <com/sun/star/frame/XStorable.hpp>
#include <com/sun/star/lang/XMultiComponentFactory.hpp>
#include <com/sun/star/util/XCloseable.hpp>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#endif

using css::uno::Reference;
using css::uno::UNO_QUERY;

using link_utils::u2s;
using link_utils::s2u;
using link_utils::to_path; // G 缝: 上收 link_utils (设计 E 表)

namespace {

// NPOfficeCache: NovaOfficeCore 缩略图链缓存 (GlobalDataSet::DoConvertDocumentW,
// 键 = 源文件 MD5; Linux /tmp 易失, Windows %APPDATA%)。writer 只读消费,
// 路径单点 (经验 38③ 依赖单向纪律: 不反向耦合, 上层决定其布局)。
std::string NpOfficeCacheDir() {
#ifdef _WIN32
    char appdata[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata)))
        return std::string(appdata) + "\\NPOfficeCache";
    return "C:\\NPOfficeCache";
#else
    return "/tmp/NPOfficeCache";
#endif
}

// 原子写: 先写 <dst>.<pid>.tmp 再 rename (并发同播同文档无冲突)
bool AtomicCopyFile(const std::string& src, const std::string& dst) {
    std::error_code ec;
    std::filesystem::create_directories(to_path(dst).parent_path(), ec);
    std::string tmp = dst + "." + std::to_string(
#ifdef _WIN32
        GetCurrentProcessId()
#else
        getpid()
#endif
    ) + ".tmp";
    {
        std::ifstream in(to_path(src), std::ios::binary);
        std::ofstream out(to_path(tmp), std::ios::binary | std::ios::trunc);
        if (!in || !out)
            return false;
        out << in.rdbuf();
        if (!out.good())
            return false;
    }
    std::filesystem::rename(to_path(tmp), to_path(dst), ec);
    return !ec;
}

} // namespace

WriterSession::~WriterSession() {
    Destroy();
}

bool WriterSession::Create(const char* path, const char* password, const char* guid,
                           WriterFrameCallback cb, void* opaque, int width, int height) {
    (void)guid; // 会话无 per-instance profile (共享内核独立 profile, 经验 27)
    if (!path || !path[0] || !cb)
        return false;
    cb_ = cb;
    opaque_ = opaque;
    target_w_ = width > 0 ? width : link_utils::kDefaultWidth;
    target_h_ = height > 0 ? height : link_utils::kDefaultHeight;

    // G 缝 ([platform-isolation] Part 2 G): 引导缝收进 link_utils::KernelHost,
    // writer 会话零 #ifdef (Linux: Acquire+BootLock+EnsureKernel; Win: BootstrapSession)。
    kernel_host_ = std::make_unique<link_utils::KernelHost>("writerlink", guid ? guid : "");
    kernel_host_->BeginBoot();
    ctx_ = kernel_host_->ObtainCtx();
    if (!ctx_.is()) {
        Destroy();
        return false;
    }

    // PDF 缓存协同 + 获取 (经验 38③)
    if (!EnsurePdf(path, password ? password : "")) {
        Destroy();
        return false;
    }

    // PDF → Draw (Hidden; 位图管线输入)
    Reference<css::lang::XMultiComponentFactory> factory = ctx_->getServiceManager();
    try {
        // desktop_ 保留引用: Windows Destroy 时 terminate 独立 soffice
        // (calc/impress 同款; 否则进程残留,  冒烟实测)
        desktop_.set(factory->createInstanceWithContext("com.sun.star.frame.Desktop", ctx_), UNO_QUERY);
        Reference<css::frame::XComponentLoader> loader(desktop_, UNO_QUERY);
        rtl::OUString pdfUrl;
        osl::FileBase::getFileURLFromSystemPath(s2u(pdf_path_), pdfUrl);
        css::uno::Sequence<css::beans::PropertyValue> props(2);
        props[0].Name = "Hidden";
        props[0].Value <<= true;
        props[1].Name = "FilterName";
        props[1].Value <<= rtl::OUString("draw_pdf_Import");
        draw_doc_ = loader->loadComponentFromURL(pdfUrl, "_blank", 0, props);
    } catch (const css::uno::Exception& e) {
        OfficeLogErr("[WriterLink] draw_pdf_Import failed: %s", u2s(e.Message).c_str());
    }
    if (!draw_doc_.is()) {
        Destroy();
        return false;
    }
    Reference<css::drawing::XDrawPagesSupplier> sup(draw_doc_, UNO_QUERY);
    if (!sup.is()) {
        OfficeLogErr("[WriterLink] not a draw doc");
        Destroy();
        return false;
    }
    page_count_ = sup->getDrawPages()->getCount();
    OfficeLog("[WriterLink] pdf=%s pages=%d", pdf_path_.c_str(), page_count_);

    // G 缝: Release (P5 后, 窗口查找可并行, 经验 5; Linux 真锁 / Win 空)
    kernel_host_->Release();

    // 首页预热 (创建即有首帧可用)
    if (!EnsurePageBitmap(0)) {
        OfficeLogErr("[WriterLink] first page render failed");
        Destroy();
        return false;
    }

    // 阶段3: 初始化 FramePump (统一帧泵, 替代原 poll_thread_/paused_/force_frame_)
    // writer 策略: probe=force_frame_脏位 (翻页置位, ChangeFn 内 exchange 消费),
    //   heartbeat=100ms (静态页取帧模式需要帧流, 经验 38), hbp=false (Pause 冻结),
    //   tick=5ms (与原 PollThread 一致), backoff=200ms
    {
        FramePumpPlan pp;
        pp.tick_ms = 5;
        pp.heartbeat_ms = 100;
        pp.heartbeat_when_paused = false;  // writer 原 PollThread: !paused_ && heartbeat_due
        pp.fail_backoff_ms = 200;
        pump_ = std::make_unique<FramePump>("writer", pp,
            [this]() { return PushFrame(); },
            [this]() { return force_frame_.exchange(false); });  // 脏位 probe
    }

    created_ = true;
    return true;
}

bool WriterSession::EnsurePdf(const std::string& doc_path, const std::string& password) {
    std::string md5 = Md5FileHex(doc_path);
    if (md5.empty()) {
        OfficeLogErr("[WriterLink] md5 failed: %s", doc_path.c_str());
        return false;
    }
    std::string cache_dir = office_paths::writer_cache_dir();
    std::error_code ec;
    std::filesystem::create_directories(to_path(cache_dir), ec);
    pdf_path_ = cache_dir + "/" + md5 + ".pdf";

    auto t0 = std::chrono::steady_clock::now();
    const char* source = "writer_cache-hit";
    bool pdf_exists = std::filesystem::exists(to_path(pdf_path_));
    if (!pdf_exists) {
        // Nova 缩略图链产物 (键互通: 同 MD5 算法); 命中总是拷贝 (易失 /tmp +
        // 免疫外部清理, 经验 38③; 反向协同不做 — 依赖单向纪律)
        std::string np = NpOfficeCacheDir() + "/" + md5 + ".pdf";
        bool np_exists = std::filesystem::exists(to_path(np));
        if (np_exists) {
            if (!AtomicCopyFile(np, pdf_path_)) {
                OfficeLogErr("[WriterLink] copy NPOfficeCache failed (%s)", np.c_str());
                return false;
            }
            source = "NPOfficeCache-copy";
        } else {
            // 自转: 同内核 storeToURL (writer_pdf_Export) → 原子落 writer_cache
            source = "convert";
            Reference<css::lang::XMultiComponentFactory> factory = ctx_->getServiceManager();
            Reference<css::frame::XComponentLoader> loader;
            loader.set(factory->createInstanceWithContext("com.sun.star.frame.Desktop", ctx_), UNO_QUERY);
            rtl::OUString docUrl;
            if (osl::FileBase::getFileURLFromSystemPath(s2u(doc_path), docUrl) != osl::FileBase::E_None)
                return false;
            css::uno::Sequence<css::beans::PropertyValue> props(2);
            props[0].Name = "Hidden";
            props[0].Value <<= true;
            // ReadOnly: 只读转换, 避免在源目录创建/校验文档锁 (缺陷报告根除项)
            props[1].Name = "ReadOnly";
            props[1].Value <<= true;
            if (!password.empty()) {
                props.realloc(3);
                props[2].Name = "Password";
                props[2].Value <<= s2u(password);
            }
            Reference<css::lang::XComponent> wdoc;
            try {
                wdoc = loader->loadComponentFromURL(docUrl, "_blank", 0, props);
            } catch (const css::uno::Exception& e) {
                OfficeLogDbg("[WriterLink] docx load exception: %s", u2s(e.Message).c_str());
            }
            if (!wdoc.is()) {
                std::string lock = link_utils::GetLockFileIfExists(doc_path);
                std::string why = lock.empty()
                    ? std::string("no lock file")
                    : std::string("stale lock file detected: ") + lock;
                OfficeLogErr("[WriterLink] docx load failed: %s; %s", doc_path.c_str(), why.c_str());
                return false;
            }
            std::string tmp = pdf_path_ + ".convert.tmp";
            rtl::OUString pdfUrl;
            osl::FileBase::getFileURLFromSystemPath(s2u(tmp), pdfUrl);
            bool ok = false;
            try {
                Reference<css::frame::XStorable> st(wdoc, UNO_QUERY);
                css::uno::Sequence<css::beans::PropertyValue> exp(1);
                exp[0].Name = "FilterName";
                exp[0].Value <<= rtl::OUString("writer_pdf_Export");
                st->storeToURL(pdfUrl, exp);
                ok = true;
            } catch (const css::uno::Exception& e) {
                OfficeLogErr("[WriterLink] pdf export failed: %s", u2s(e.Message).c_str());
            }
            Reference<css::util::XCloseable> cw(wdoc, UNO_QUERY);
            if (cw.is())
                cw->close(false);
            if (!ok)
                return false;
            std::filesystem::rename(to_path(tmp), to_path(pdf_path_), ec);
            if (ec) {
                OfficeLogErr("[WriterLink] rename failed");
                return false;
            }
        }
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    OfficeLog("[WriterLink] pdf ready (%s, %lld ms): %s", source, (long long)ms, pdf_path_.c_str());
    EnforceCacheLimit();
    return true;
}

// 缓存总量回收 (简单策略, ): 目录总量超上限时按 mtime 最旧删除。
// 上限 ORT_WRITER_CACHE_MB 覆盖, 默认 500MB; 排除当前会话使用的 pdf_path_
// (天然最新, 防御单文件超限场景)。失败静默 (回收是尽力而为, 不影响播放)。
void WriterSession::EnforceCacheLimit() {
    namespace fs = std::filesystem;
    try {
        uint64_t limit_mb = 500;
        if (const char* v = getenv("ORT_WRITER_CACHE_MB")) {
            long n = atol(v);
            if (n > 0)
                limit_mb = (uint64_t)n;
        }
        std::string dir = office_paths::writer_cache_dir();
        fs::path dpath(to_path(dir));
        fs::path keep(to_path(pdf_path_));
        struct Item { fs::path path; uint64_t size; fs::file_time_type mtime; };
        std::vector<Item> items;
        uint64_t total = 0;
        for (const auto& e : fs::directory_iterator(dpath)) {
            if (!e.is_regular_file())
                continue;
            std::error_code ec;
            auto sz = e.file_size(ec);
            auto mt = e.last_write_time(ec);
            if (ec)
                continue;
            total += sz;
            items.push_back({e.path(), sz, mt});
        }
        uint64_t limit = limit_mb * 1024 * 1024;
        if (total <= limit)
            return;
        std::sort(items.begin(), items.end(),
                  [](const Item& a, const Item& b) { return a.mtime < b.mtime; });
        for (const auto& it : items) {
            if (total <= limit)
                break;
            if (fs::equivalent(it.path, keep))
                continue; // 当前会话在用
            std::error_code ec;
            fs::remove(it.path, ec);
            if (!ec) {
                total -= it.size;
                OfficeLog("[WriterLink] cache evict %llu MB (total %llu -> %llu MB, limit %llu): %s",
                          (unsigned long long)(it.size >> 20), (unsigned long long)((total + it.size) >> 20),
                          (unsigned long long)(total >> 20), (unsigned long long)(limit_mb),
                          it.path.filename().string().c_str());
            }
        }
    } catch (const std::exception& e) {
        OfficeLogDbg("[WriterLink] EnforceCacheLimit failed: %s", e.what());
    }
}

bool WriterSession::BmpToBgra(const std::vector<uint8_t>& dib, std::vector<uint8_t>& bgra,
                              int& w, int& h) {
    // XBitmap::getDIB 返回 BMP 文件格式 (经验 38): 'BM' + 像素偏移@10 +
    // BITMAPINFOHEADER(biWidth@18, biHeight@22, biBitCount@28)。
    // 按偏移取值, 勿假设 40 字节头。
    if (dib.size() < 54 || dib[0] != 'B' || dib[1] != 'M')
        return false;
    const uint8_t* d = dib.data();
    int32_t px_off;
    int32_t bw, bh;
    uint16_t bpp;
    memcpy(&px_off, d + 10, 4);
    memcpy(&bw, d + 18, 4);
    memcpy(&bh, d + 22, 4);
    memcpy(&bpp, d + 28, 2);
    if (bw <= 0 || bh == 0 || (bpp != 24 && bpp != 32))
        return false;
    bool top_down = bh < 0;
    int ah = top_down ? -bh : bh;
    int nbytes = bpp / 8;
    int row = ((bw * bpp + 31) / 32) * 4; // 行 4 字节对齐
    if (dib.size() < (size_t)px_off + (size_t)row * ah)
        return false;
    bgra.resize((size_t)bw * ah * 4);
    for (int y = 0; y < ah; y++) {
        const uint8_t* r = d + px_off + (size_t)y * row;
        int dy = top_down ? y : (ah - 1 - y); // BMP 默认 bottom-up → BGRA top-down
        uint8_t* dst = bgra.data() + (size_t)dy * bw * 4;
        for (int x = 0; x < bw; x++) {
            const uint8_t* p = r + (size_t)x * nbytes; // BGR(A)
            dst[x * 4 + 0] = p[0];
            dst[x * 4 + 1] = p[1];
            dst[x * 4 + 2] = p[2];
            dst[x * 4 + 3] = 0xFF;
        }
    }
    w = bw;
    h = ah;
    return true;
}

bool WriterSession::EnsurePageBitmap(int page) {
    if (page < 0 || page >= page_count_)
        return false;
    auto it = page_cache_.find(page);
    if (it != page_cache_.end()) {
        // LRU 触碰
        lru_order_.erase(std::remove(lru_order_.begin(), lru_order_.end(), page), lru_order_.end());
        lru_order_.push_back(page);
        return true;
    }
    Reference<css::lang::XMultiComponentFactory> factory = ctx_->getServiceManager();
    Reference<css::drawing::XSlideRenderer> sr;
    Reference<css::drawing::XDrawPagesSupplier> sup(draw_doc_, UNO_QUERY);
    try {
        sr.set(factory->createInstanceWithContext(
                   rtl::OUString("com.sun.star.drawing.SlideRenderer"), ctx_),
               UNO_QUERY);
    } catch (const css::uno::Exception& e) {
        OfficeLogDbg("[WriterLink] SlideRenderer creation failed: %s", u2s(e.Message).c_str());
    }
    if (!sr.is() || !sup.is())
        return false;
    Reference<css::drawing::XDrawPage> pg;
    try {
        pg.set(sup->getDrawPages()->getByIndex(page), UNO_QUERY);
    } catch (const css::uno::Exception& e) {
        OfficeLogDbg("[WriterLink] page %d retrieval failed: %s", page, u2s(e.Message).c_str());
        return false;
    }
    if (!pg.is())
        return false;
    auto t0 = std::chrono::steady_clock::now();
    Reference<css::awt::XBitmap> bmp =
        sr->createPreview(pg, css::awt::Size(target_w_, target_h_), 1);
    if (!bmp.is()) {
        OfficeLogErr("[WriterLink] createPreview(%d) NULL", page);
        return false;
    }
    css::uno::Sequence<sal_Int8> dib = bmp->getDIB();
    std::vector<uint8_t> buf(dib.getConstArray(), dib.getConstArray() + dib.getLength());
    std::vector<uint8_t> bgra;
    int w = 0, h = 0;
    if (!BmpToBgra(buf, bgra, w, h)) {
        OfficeLogErr("[WriterLink] bmp parse failed (page %d)", page);
        return false;
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    page_cache_[page] = std::move(bgra);
    lru_order_.push_back(page);
    width_ = w; // 当前页位图尺寸 (帧回调使用)
    height_ = h;
    EvictFarPages(page);
    OfficeLogDbg("[WriterLink] page %d rendered %dx%d (%lld ms, cache=%zu)",
              page, w, h, (long long)ms, page_cache_.size());
    return true;
}

void WriterSession::EvictFarPages(int current) {
    // 按需渲染 + 当前页±2 保留 (定向淘汰 current±3, 非真 LRU; 经验 38②)
    for (int p : {current - 3, current + 3}) {
        auto it = page_cache_.find(p);
        if (it != page_cache_.end()) {
            page_cache_.erase(it);
            lru_order_.erase(std::remove(lru_order_.begin(), lru_order_.end(), p), lru_order_.end());
        }
    }
}

bool WriterSession::PushFrame() {
    if (destroyed_ || !cb_)  // V4: Destroy 窗口期帧泵线程安全退出
        return false;
    // 阶段3: FrameFn 契约 — 持 mu_ 访问 page_cache_ (frame_mutex_ -> mu_ 锁序,
    // 无反向: pump_ 方法不得持 mu_)
    std::lock_guard<std::mutex> lk(mu_);
    auto it = page_cache_.find(current_page_);
    if (it == page_cache_.end() || width_ <= 0)
        return false;
    cb_(it->second.data(), width_, height_, width_ * 4,
        (int32_t)(it->second.size()), link_utils::kFrameFormatBGRA, opaque_);
    return true;
}

// 阶段3: Start/Stop/Pause/Resume/UpdateFrame 委托 FramePump; 锁纪律: pump 方法不持 mu_
bool WriterSession::Start() {
    if (destroyed_)
        return false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!created_)
            return false;
    }
    if (pump_)
        pump_->Start();
    return true;
}

bool WriterSession::Stop() {
    if (destroyed_)
        return false;
    if (pump_)
        pump_->Stop();
    return true;
}

bool WriterSession::Pause() {
    if (destroyed_)
        return false;
    if (pump_)
        pump_->Pause();
    return true;
}

bool WriterSession::Resume() {
    if (destroyed_)
        return false;
    if (pump_)
        pump_->Resume();
    return true;
}

// 阶段3: UpdateFrame 委托 FramePump (同步立即帧, 任意状态有效; frame_mutex_ 串行)
bool WriterSession::UpdateFrame() {
    if (destroyed_ || !pump_)
        return false;
    return pump_->UpdateFrame();
}

bool WriterSession::SetResolution(int width, int height) {
    if (destroyed_)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!created_ || width <= 0 || height <= 0)
        return false;
    target_w_ = width;
    target_h_ = height;
    page_cache_.clear(); // 尺寸相关, 全失效重渲染
    lru_order_.clear();
    return EnsurePageBitmap(current_page_);
}

bool WriterSession::NextPage() {
    if (destroyed_)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!created_ || current_page_ + 1 >= page_count_)
        return false;
    current_page_++;
    if (!EnsurePageBitmap(current_page_))
        return false;
    force_frame_ = true;
    return true;
}

bool WriterSession::PreviousPage() {
    if (destroyed_)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!created_ || current_page_ <= 0)
        return false;
    current_page_--;
    if (!EnsurePageBitmap(current_page_))
        return false;
    force_frame_ = true;
    return true;
}

bool WriterSession::GoToPage(int page) {
    if (destroyed_)
        return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!created_ || page < 0 || page >= page_count_)
        return false;
    current_page_ = page;
    if (!EnsurePageBitmap(current_page_))
        return false;
    force_frame_ = true;
    return true;
}

int WriterSession::GetCurrentPage() { return destroyed_ ? -1 : (created_ ? current_page_ : -1); }
int WriterSession::GetPageCount() { return destroyed_ ? -1 : (created_ ? page_count_ : -1); }
int WriterSession::GetWidth() { return destroyed_ ? 0 : width_; }
int WriterSession::GetHeight() { return destroyed_ ? 0 : height_; }

void WriterSession::Destroy() {
    // V4 状态机: destroyed 终态幂等入口 (析构/重复调用只清理一次)
    if (destroyed_.exchange(true))
        return;
    // 阶段3: pump_->Stop 不得持 mu_ (FramePump 契约); 直调 pump_->Stop 而非
    // 公开 Stop() (后者已被 destroyed_ 守卫拦截, V4)
    if (pump_)
        pump_->Stop();
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (draw_doc_.is()) {
            try {
                Reference<css::util::XCloseable> cw(draw_doc_, UNO_QUERY);
                if (cw.is())
                    cw->close(false);
                else
                    draw_doc_->dispose();
            } catch (const css::uno::Exception& e) {
                OfficeLogDbg("[WriterLink] close draw_doc failed: %s", u2s(e.Message).c_str());
            }
            draw_doc_.clear();
        }
        page_cache_.clear();
        lru_order_.clear();
        // G 缝: terminate 按 KernelHost::ShouldTerminateOnDestroy 门控 (数据驱动)
        // Windows 每 session 独立 soffice 须 terminate 退出; Linux 共享内核不 terminate
        if (kernel_host_ && kernel_host_->ShouldTerminateOnDestroy() && desktop_.is()) {
            try {
                desktop_->terminate();
            } catch (const css::uno::Exception& e) {
                OfficeLogDbg("[WriterLink] desktop terminate failed: %s", u2s(e.Message).c_str());
            }
        }
        desktop_.clear();
        ctx_.clear();
    }
    // G 缝: kernel_host_ 析构 Release runtime 引用 (Linux) / 无 (Win)
    kernel_host_.reset();
    pump_.reset();  // 阶段3: pump_ 最后释放 (FrameFn 捕获 this, 需保证 this 存活到 Stop)
    created_ = false;
    OfficeLog("[WriterLink] session destroyed");
}
