// link_utils.h — 跨平台会话工具 (calc/impress/writer 共用)。
// u2s/s2u: OUString <-> std::string; HideUiBlock: LO UI 隐藏三件套
// (FullScreen dispatch + setMenuBar(null) + LayoutManager hideElement)。
// 纯 UNO 操作, 与运行平台无关 (Windows 会话同样使用)。
#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <com/sun/star/frame/XDispatchProvider.hpp>
#include <com/sun/star/frame/XFrame.hpp>
#include <com/sun/star/lang/XMultiComponentFactory.hpp>
#include <com/sun/star/uno/XComponentContext.hpp>
#include <rtl/ustring.hxx>

namespace link_utils {

// 帧格式常量: AV_PIX_FMT_BGRA = 28 (与 Nova 管线对齐, calc/impress/writer 共用)
constexpr int kFrameFormatBGRA = 28;

// 默认分辨率 (会话 Create 的 width/height ≤ 0 时回退值; calc/impress/writer 统一)
constexpr int kDefaultWidth = 1920;
constexpr int kDefaultHeight = 1080;

std::string u2s(const rtl::OUString& s);
rtl::OUString s2u(const std::string& s);

#ifdef _WIN32
// UTF-8 -> UTF-16 (Windows 窄字符 API 按 ANSI 码页解释, UTF-8 中文路径需显式转换)
std::wstring u2w(const std::string& s);
#endif

// 本模块 (DLL) 所在目录 (Windows: GetModuleHandleExA 定位自身; 部署形态下 ==
// soffice program dir)。Linux 调用方不需要 (office_runtime/dladdr 路径), 返回空串。
std::string GetLinkDir();

// Windows: 每 session 独立 soffice 的三参 bootstrap (soffice 目录 + 独立
// profile + 目标桌面名, 空桌面名 = 无桌面切换需求, 如 writer 离屏管线)。
// 供平台层 EnsureKernel 与 writer 会话共用 (经验 38④: calc_session 同款模式,
// 从会话层 #ifdef _WIN32 下沉统一); 失败返回空引用并记日志。
// Linux: 共享内核走平台层 EnsureKernel, 本函数不适用 (返回空)。
css::uno::Reference<css::uno::XComponentContext> BootstrapSession(
    const std::string& link_dir, const std::string& profile, const std::string& desktop);

// UI 隐藏三件套 (与 calc/impress 会话 Create 中原块一致, 提取为公共函数):
//   1. .uno:FullScreen dispatch (prov 由调用方传: calc 传 desktop, impress 传 frame)
//   2. setMenuBar(null) (sleep 1000 让 UI 稳定后执行)
//   3. LayoutManager hideElement(menubar)
// 日志走统一 OfficeLog ([UIHIDE] 前缀)。
void HideUiBlock(const css::uno::Reference<css::frame::XDispatchProvider>& prov,
                 const css::uno::Reference<css::frame::XFrame>& frame,
                 const css::uno::Reference<css::lang::XMultiComponentFactory>& factory,
                 const css::uno::Reference<css::uno::XComponentContext>& ctx);

// UI 状态自省 (UNO): 打印 frame 激活态/窗口可见性 + LayoutManager 各 UI 元素
// (menubar/toolbar/statusbar/sidebar) 可见状态。用于分析 UI 隐藏是否生效,
// 日志驱动排查 (HideUiBlock 前后各调一次, state[before]/state[after] 对比)。
// 元素不可查 (属性不存在/异常) 打印 ERR, 不中断。
void DumpUiState(const css::uno::Reference<css::frame::XFrame>& frame,
                 const css::uno::Reference<css::lang::XMultiComponentFactory>& factory,
                 const css::uno::Reference<css::uno::XComponentContext>& ctx,
                 const char* tag);

// UTF-8 字符串 -> filesystem::path (Windows 走 UTF-16, Linux 直传)。
// 消除各会话 std::filesystem 调用的 #ifdef _WIN32 u2w(x) 重复 (设计 E 表
// "to_path 应上收 link_utils 三链共用"; G 缝, [platform-isolation] Part 2 G)。
std::filesystem::path to_path(const std::string& utf8);

// LO 文档锁文件检测 (缺陷: 残留 `.~lock.<basename>#` 锁文件 → loadComponentFromURL
// 静默返回 null, 播放器表现为加载失败)。计算同目录锁文件路径; 存在则返回其
// UTF-8 路径, 否则返回空串。用于加载失败时诊断残留锁 (见缺陷报告/经验)。
std::string GetLockFileIfExists(const std::string& doc_path);

// 源文件外部写锁预检 (Windows 专属防御, 平台差异收基础层函数内部 — u2w/to_path
// 同款模式, 会话层保持零 #ifdef)。检测源文件是否被外部进程以写方式占用
// (CreateFileW 试开 GENERIC_READ|WRITE 命中 ERROR_SHARING_VIOLATION)。
// 用途: 命中时跳过普通模式直接 ReadOnly 打开 —— 正常模式撞外部写锁会在
// 隐藏桌面弹模态 "Document in Use" 对话框, 卡死整个会话。
// Linux: 无共享冲突模态风险, 恒返回 false。
bool SourceWriteLocked(const std::string& path);

// 内核宿主 (G 缝, [platform-isolation] Part 2 G): writer 无 LinkPlatform 层 (经验 38④
// 无窗口/无抓帧), 引导缝 (Acquire/BootLock/EnsureKernel vs BootstrapSession)
// 收进本工具, writer 会话零 #ifdef。calc/impress 走 LinkPlatform 体系, 不用本类。
//
// 生命周期: 构造(Acquire/profile 准备) → BeginBoot(串行区) → ObtainCtx(内核)
//           → ... 加载文档 ... → Release(P5 后, 窗口查找可并行, 经验 5)
//           → 析构(Release runtime 引用 / Win 无)
// ShouldTerminateOnDestroy(): 平台是否须 terminate 独立 soffice (Win true / Linux false)。
class KernelHost {
public:
    KernelHost(const char* profile_subdir, const char* guid);
    ~KernelHost();
    void BeginBoot();
    void Release();
    css::uno::Reference<css::uno::XComponentContext> ObtainCtx();
    bool ShouldTerminateOnDestroy() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace link_utils

// UNO 异常处理辅助宏 (减少 try/catch 样板代码, 确保异常不被静默吞掉):
//
// UNO_GUARD(expr, fallback, tag):
//   执行 expr, 异常时记 debug 日志并返回 fallback。
//   用于简单的一表达式函数 (如 GetSheetCount/GetCurrentPage 等)。
//   示例: return UNO_GUARD(names->getElementNames().getLength(), 0, "CalcLink");
//
// UNO_SILENT(expr, tag):
//   执行 expr, 异常时记 debug 日志但不中断。
//   用于 Destroy/Pause/Resume 等生命周期方法 (异常不影响主流程)。
//   示例: UNO_SILENT(slideshow_->pause(), "ImpressLink");
//
// 注意: 宏展开后包含 try/catch, 不要在表达式中使用 return/break/continue。
// 复杂逻辑仍应手写 try/catch 以便精确控制。

#define UNO_GUARD(expr, fallback, tag)                                       \
    [&]() -> decltype(auto) {                                                \
        try {                                                                \
            return (expr);                                                   \
        } catch (const css::uno::Exception& e) {                             \
            OfficeLogDbg("[%s] UNO exception in %s: %s",                     \
                      (tag), #expr, link_utils::u2s(e.Message).c_str());     \
            return static_cast<decltype((expr))>(fallback);                  \
        }                                                                    \
    }()

#define UNO_SILENT(expr, tag)                                                \
    do {                                                                     \
        try {                                                                \
            (expr);                                                          \
        } catch (const css::uno::Exception& e) {                             \
            OfficeLogDbg("[%s] UNO exception (silent): %s: %s",              \
                      (tag), #expr, link_utils::u2s(e.Message).c_str());     \
        }                                                                    \
    } while (0)
