// runtime.h — 进程级共享 LibreOffice 运行时 (Linux 共享内核模式)
// 管理: Xvfb 虚拟屏 / LO 共享内核 / 大屏 Slot 分区。
// 生命周期强耦合 (LO 依赖 Xvfb 显示, slot 属于 Xvfb 大屏), 合并管理:
// 首个文档 session Acquire 时创建全部; Xvfb/内核为进程级存活 (末个 Release
// 仅计数归零, 不销毁)。退出清理: Xvfb 由 atexit 回调清理 (仅本进程自起的);
// LO 内核进程断管道自退, 残留由下次 Acquire 的 CleanupOrphanSoffice 兜底。
// 供 calclink/impresslink/writerlink 共用; 媒体后端子模块
// ffplay 为独立组件 (不依赖本运行时核心)。
// Windows 为每 session 独立 soffice 进程 + 独立桌面, 无进程级共享需求,
// 不参与本模块。若未来 Windows 引入共享内核, 再按平台分层。
#pragma once

#include <semaphore.h>

#include <mutex>
#include <string>
#include <vector>

#include <com/sun/star/uno/Reference.hxx>
#include <com/sun/star/uno/XComponentContext.hpp>

// ---- 统一日志 (声明见 ../common/log.h; Linux 实现唯一在 office_runtime.so
// 单例, Windows 实现 common/win_office_log.cpp, 经验 39) ----
#include <base/log.h>

// 虚拟屏分区配置 (首个 Acquire 定屏, 后续须一致)。
// 调用者只需知道两个数: 最大并发文档数 + 每文档最大分辨率。
// 画布 (静态) = max_docs × max_doc_width   × max_doc_height
// 子屏位 (静态位图, max_docs 个)           → 文档动态分配一个位
// 文档窗口 (动态) 按实际分辨率落位在子屏位内左上角。
struct OfficeRuntimeConfig {
    int max_docs = 8;          // 最大并发文档数 (子屏位数, 即分配上限)
    int max_doc_width = 3840;  // 每文档最大分辨率宽 (2160p)
    int max_doc_height = 2160; // 每文档最大分辨率高 (2160p)
};

class OfficeRuntime {
public:
    // 进程级共享实例: office_runtime.so 内静态, 所有依赖者 (calclink/
    // impresslink/writerlink 的各 platform 实例) 共享同一运行时。
    static OfficeRuntime& Instance();

    OfficeRuntime() = default;
    ~OfficeRuntime();

    OfficeRuntime(const OfficeRuntime&) = delete;
    OfficeRuntime& operator=(const OfficeRuntime&) = delete;

    // ---- 生命周期 (引用计数, 线程安全) ----
    // 首次调用: 启动 Xvfb + 设置 DISPLAY; 之后仅计数 +1。
    bool Acquire(const OfficeRuntimeConfig& config);
    // 计数 -1 (归零不销毁: Xvfb/内核进程级存活, 退出时 atexit 清理)。
    void Release();

    // ---- 显示环境 ----
    // 当前虚拟屏显示号, 如 ":90"。
    const std::string& display() const;
    // setenv("DISPLAY", ...), 内核引导前调用 (bootstrap 子进程继承)。
    void SetDisplayEnv() const;

    // ---- LO 共享内核 (首个引导, 后续复用; 内部互斥串行化) ----
    // 引导实现为自研 BootstrapOffice (复制官方 cppu::bootstrap 逻辑, 见
    // office_runtime.cpp), 支持可选独立 UserInstallation:
    //   user_installation 空 = 播放内核独立 profile ~/.office-link/xvfb
    //     ( 由 player/ 更名, 见经验 40; 不维护部署 office/user:
    //     LO 会重建/写回运行时配置; UI 隐藏由
    //     UNO 动态控制; 与外部默认 profile 的 soffice 调用隔离, 经验 27);
    //   非空 = 指定 profile 内核 (转换等隔离场景, 多内核并存暂不支持,
    //   已引导不同 profile 时忽略并复用现有内核)。
    bool EnsureKernel(const std::string& user_installation = std::string());
    const css::uno::Reference<css::uno::XComponentContext>& kernel() const;

    // ---- 引导段串行化 (跨 link + 跨进程) ----
    // LO 内核的 bootstrap + loadComponentFromURL + setVisible 必须互斥
    // (多 link 并发 Create 会卡死); 窗口查找可并行, 调用方在 setVisible
    // 后 Unlock()。进程内用 so 单例 mutex (所有 link 共享); 跨进程用
    // 命名信号量 —— 共享内核按 UserInstallation 被多进程复用时, 引导+加载段
    // 必须全机器串行 (同进程多线程用 mutex 即可, 跨进程才需要信号量)。
    // 用法: OfficeRuntime::BootLock boot_lk; ... boot_lk.Unlock();
    class BootLock {
    public:
        // timeout_ms: 跨进程信号量卡死的强制恢复等待 (默认 60s; 测试可缩短)
        explicit BootLock(int timeout_ms = 60000);
        ~BootLock();
        void Lock();
        void Unlock();
    private:
        int timeout_ms_ = 60000;
        std::mutex* mtx_ = nullptr;  // 进程内 (so 单例静态)
        sem_t* sem_ = SEM_FAILED;    // 跨进程 (命名信号量)
    };

    // ---- Slot 分区 (返回 -1 表示已满) ----
    int AllocSlot();
    void FreeSlot(int slot);

    // ---- 诊断: X 窗口自省 (只读, 供排障/回归; 不改变运行时行为) ----
    // 打印窗口树 (位置/尺寸/map/class), 输出走 [ORT] 日志。
    void DumpWindows();
    // 检测可见 LO 窗口 (class 含 libreofficedev) 之间的矩形重叠,
    // 有重叠打印 OVERLAP 日志; 返回重叠对数 (0 = 无重叠)。
    // 注: 父子窗口重叠视为正常 (LO 框架与视图结构), 只检测非父子对。
    int CheckWindowOverlap();
    // 采样每个 LO 窗口 4 边 + 中心的像素颜色分布 (排查边缘黑/串流)
    void DumpWindowEdges();

    // ---- gstreamer 依赖检测 ----
    // 含音视频的 pptx 媒体页依赖 gst; 缺失时 LO 媒体页会卡死/黑屏 (静默失败,
    // 实测: 无插件时 slideshow 事件链阻塞)。EnsureKernel 引导时调用并告警。
    // 检测: 核心库 (dlopen 实测) + 插件目录 + 关键插件文件。
    // 返回: 0=完整; 1=缺核心库; 2=缺插件目录; 3=缺关键插件。
    // 参数化 (plugin_dir_override) 便于单测模拟缺失场景; nullptr = 系统默认。
    int CheckGstDeps(const char* plugin_dir_override, std::string* detail = nullptr);

    const OfficeRuntimeConfig& config() const;

private:
    bool StartXvfb();
    void StopXvfb();

    OfficeRuntimeConfig config_;
    int ref_count_ = 0;
    std::mutex mutex_;                    // 生命周期/内核/slot 统一锁序

    pid_t xvfb_pid_ = -1;
    bool xvfb_owned_ = true;       // 本进程启动的 Xvfb (退出时杀); 采用共享屏时为 false
    std::string display_;

    css::uno::Reference<css::uno::XComponentContext> kernel_;
    std::string kernel_profile_; // 引导时用的 UserInstallation (空参数时 = ~/.office-link/xvfb)
    // slot 占用位图已迁至跨进程共享内存 (shm + flock + 所有者 PID),
    // 见 office_runtime.cpp 的 OpenSlotShm / AllocSlot / FreeSlot。
};
