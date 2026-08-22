// writer_session.h — Writer 会话 (自治 PDF 位图管线, 经验 38)。
// 链路: 共享内核 → PDF(缓存协同: writer_cache → NPOfficeCache → 自转)
//   → draw_pdf_Import → XSlideRenderer::createPreview 逐页位图 → BMP 解析
//   → BGRA 帧回调。无平台层 (LinkPlatform)/无窗口/无 slot/无抓帧 —
//   writer 是静态排版内容, 离屏位图模型 (经验 38 落地决策④)。
// Windows 差异 (经验 39): 三参 bootstrap (link_utils::BootstrapSession, 桌面名
//   为空=离屏) + 原生文件操作宽路径 (U2W, 中文路径) + Destroy terminate
//   独立 soffice + 链 common (共享工具, 平台层不落地)。
// 会话状态机 (V4 治理, HANDOFF 七): created_(PDF 就绪) -> destroyed_(终态)。
// 公开方法入口判 destroyed_, 销毁后调用一律 no-op; C ABI 层另有
// SessionRegistry 入口守卫 (common/session_registry.h) 双层防护。
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <com/sun/star/uno/Reference.hxx>
#include <com/sun/star/uno/XComponentContext.hpp>
#include <com/sun/star/lang/XComponent.hpp>
#include <com/sun/star/frame/XDesktop.hpp>

#include <base/link_utils.h> // KernelHost (G 缝, 引导缝封装)
#include <base/frame_pump.h> // 阶段3: 统一帧泵 (替代 poll_thread_/paused_/force_frame_)

typedef void (*WriterFrameCallback)(const uint8_t* data, int32_t width, int32_t height,
                                    int32_t row_pitch, int32_t size, int32_t format, void* opaque);

class WriterSession {
public:
    WriterSession() = default;
    ~WriterSession();

    bool Create(const char* path, const char* password, const char* guid,
                WriterFrameCallback cb, void* opaque, int width, int height);
    void Destroy();

    bool Start();
    bool Stop();
    bool Pause();
    bool Resume();
    // 锁序约束 (V5): UpdateFrame 持 frame_mutex_, 不得在其他方法 (NextPage/
    //   Stop 等, 持 mu_) 的调用栈内嵌套调用。同理, 帧回调 cb_ 内不得调用
    //   任何 session API (cb_ 在 frame_mutex_ 持有期间执行)。
    bool UpdateFrame();
    bool SetResolution(int width, int height);
    bool NextPage();
    bool PreviousPage();
    bool GoToPage(int page);
    int  GetCurrentPage();
    int  GetPageCount();
    int  GetWidth();
    int  GetHeight();

private:
    // PDF 获取 (缓存协同三路径, 经验 38③): writer_cache 命中 → NPOfficeCache
    // 拷贝 → 同内核自转; 返回 pdf_path_ 就绪的绝对路径
    bool EnsurePdf(const std::string& doc_path, const std::string& password);
    // writer_cache 总量回收: 超限(ORT_WRITER_CACHE_MB, 默认 500MB)按 mtime
    // 最旧删除, 排除当前 pdf_path_ (尽力而为, 失败静默)
    void EnforceCacheLimit();
    // 页位图按需渲染 + LRU (当前±2); 成功后 page_bitmap_ 就绪
    bool EnsurePageBitmap(int page);
    // BMP(DIB) 解析 → BGRA (BM 头 + px_off@10 + 24bpp BGR 行对齐, 经验 38)
    static bool BmpToBgra(const std::vector<uint8_t>& dib, std::vector<uint8_t>& bgra,
                          int& w, int& h);
    bool PushFrame();  // 阶段3: 返回 bool (FrameFn 契约); 持 mu_ 访问 page_cache_
    void EvictFarPages(int current);

private:
    WriterFrameCallback cb_ = nullptr;
    void* opaque_ = nullptr;
    int target_w_ = 1920;
    int target_h_ = 1080;
    int width_ = 0;    // 当前页位图尺寸 (随页面比例)
    int height_ = 0;

    css::uno::Reference<css::uno::XComponentContext> ctx_;
    css::uno::Reference<css::frame::XDesktop> desktop_; // Windows: Destroy terminate
    css::uno::Reference<css::lang::XComponent> draw_doc_; // PDF 导入的 Draw 文档
    int page_count_ = 0;
    int current_page_ = 0;

    std::string pdf_path_; // 本会话使用的 PDF (writer_cache 内)

    // 页位图 LRU: page → BGRA (当前±2 之外淘汰)
    std::map<int, std::vector<uint8_t>> page_cache_;
    std::deque<int> lru_order_;

    std::mutex mu_;  // 会话锁 (page_cache_/UNO 操作保护; 调用 pump_ 方法时不得持有)
    // 阶段3: 脏位 probe (翻页置位, ChangeFn 内 exchange 消费)
    std::atomic<bool> force_frame_{false};
    // Frame pump (阶段3: 统一帧泵, 替代原 poll_thread_/paused_/force_frame_)
    // writer 策略: probe=force_frame_脏位, heartbeat=100ms, hbp=false (Pause 冻结), tick=5ms
    std::unique_ptr<FramePump> pump_;
    bool created_ = false;
    // V4 状态机: destroyed 终态标志 (Destroy 幂等 + 公开方法入口守卫)
    std::atomic<bool> destroyed_{false};
    std::unique_ptr<link_utils::KernelHost> kernel_host_; // G 缝: 引导缝封装 (Acquire/BootLock/EnsureKernel / BootstrapSession)
};
