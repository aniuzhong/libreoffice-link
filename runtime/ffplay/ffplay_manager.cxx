// ffplay_manager.cxx — XManager + XFastPropertySet (ffplay 自治媒体后端,
// service: com.sun.star.comp.avmedia.Manager_FFPlay)。
// 注入: LO mediawindow_impl.cxx 读 ORT_MEDIA_BACKEND=ffplay 选本组件 (方案 A, 经验 30)。
// 注入验证: LO 的 avmedia 创建媒体后端时解析到本组件 -> 视频区域渲染真实
// 视频/音频 (经验 34; 注入判据 = 帧间差异, 不再是全绿壳)
//
// 静音专项 (方案 A 重构, 2026-08-21): 用 parent_window 物理句柄做归属键。
// 本组件同时实现 XFastPropertySet, 暴露 MGR_PROP_MUTE_WINDOWS 属性 (handle=0)。
// impresslink (主进程) 经 UNO remote ctx createInstance("Manager_FFPlay")
// 取得本组件远程引用后 QI XFastPropertySet, 调
//   setFastPropertyValue(0, {Sequence<int32>(window_ids), mute(bool)})
// → 转发 FfplayPlayer::SetMuteAll (静态方法, 操作 g_engines 全局表)。
// 由于本组件与 g_engines 同处 soffice.bin 子进程, 调用天然在子进程内执行,
// 解决方案 A dlopen 跨进程失效问题 (主进程 dlopen 拿到的是空 g_engines 副本)。
// window_ids 由 ImpressSession 通过 XQueryTree 枚举自己放映主窗口下所有子窗口
// 获得 (LO 为媒体 shape 创建的 X11 子窗口, parent = 放映主窗口, 跨进程可见)。
// 空 window_ids = no-op (不动任何引擎); 避免共享内核模式下某 session 无 media
// 子窗口时误伤其他 session 引擎 (NovaPlayerDemo 切换 item 实证 2026-08-21)。
//
// 日志专项: ffplay_log.h (header-only spdlog logger) 初始化时注册 av_log callback,
// ffmpeg 库内诊断经 [FFmpeg/<module>] 前缀落盘 ~/.office-link/logs/ffplay_<pid>.log
// (复用 ORT_LOG/ORT_LOG_LEVEL 控制; HANDOFF ffplay 日志专项)。
#include <com/sun/star/media/XManager.hpp>
#include <com/sun/star/beans/XFastPropertySet.hpp>
#include <com/sun/star/uno/XComponentContext.hpp>
#include <cppuhelper/implbase2.hxx>
#include <cstdio>
#include <uno/lbnames.h>
#include <uno/environment.h>

#include "ffplay_player.hxx"
#include "ffplay_log.h"  // FFLOG_* + Init() (注册 av_log callback)

using namespace css;
using namespace css::uno;
using rtl::OUString;

// XFastPropertySet handle: 静音触发开关 (方案 A, 2026-08-21)。
// handle 0 = MGR_PROP_MUTE_WINDOWS: per-window 精确隔离 (替代易错的 session_id 时序归属)。
//   Any = Sequence<Any> = {Sequence<sal_Int32>(window_ids), mute(bool)}
//   window_ids = ImpressSession 通过 XQueryTree 枚举自己放映主窗口下的所有子窗口 ID
//   (LO 为每个媒体 shape 创建的 X11 子窗口, parent = 放映主窗口, 跨进程可见)。
//   空列表 = no-op (SetMuteAll 内部判定, 不动任何引擎; 避免共享内核模式下
//   某 session 无 media 子窗口时误伤其他 session 引擎, NovaPlayerDemo 切换 item 实证)。
constexpr sal_Int32 MGR_PROP_MUTE_WINDOWS = 0;

class FfplayManager final : public cppu::WeakImplHelper2<css::media::XManager,
                                                        css::beans::XFastPropertySet> {
public:
    FfplayManager() { FFLOG_INFO("[FFPLAY] FfplayManager ctor (Manager_FFPlay injection active)"); }
    ~FfplayManager() override { FFLOG_INFO("[FFPLAY] FfplayManager dtor"); }

    Reference<css::media::XPlayer> SAL_CALL createPlayer(const OUString& rURL) override {
        std::string url = rtl::OUStringToOString(rURL, RTL_TEXTENCODING_UTF8).getStr();
        FFLOG_INFO("[FFPLAY] createPlayer URL=%s (ffplay takes over media)", url.c_str());
        FfplayPlayer* p = new FfplayPlayer();
        p->SetUrl(url); // 引擎在 createPlayerWindow (窗口句柄就绪) 时创建;
                        // 归属键 = createPlayerWindow 时已知的 parent_window_id, 无需预归属
        return p;
    }

    // ---- XFastPropertySet (静音专项, 方案 A) ----
    // setFastPropertyValue(MGR_PROP_MUTE_WINDOWS, {window_ids, mute}):
    //   per-window 精确隔离。转发到 FfplayPlayer::SetMuteAll(window_ids, mute),
    //   只操作 window_id 在 window_ids 列表内的引擎。空列表 = no-op (不动任何引擎)。
    void SAL_CALL setFastPropertyValue(sal_Int32 nHandle, const Any& aValue) override {
        if (nHandle == MGR_PROP_MUTE_WINDOWS) {
            // Any = Sequence<Any> = {Sequence<sal_Int32>(window_ids), mute(bool)}
            Sequence<Any> args;
            if (aValue >>= args) {
                if (args.getLength() >= 2) {
                    Sequence<sal_Int32> ids;
                    sal_Bool mute = false;
                    if ((args[0] >>= ids) && (args[1] >>= mute)) {
                        std::vector<long> wids;
                        wids.reserve(ids.getLength());
                        for (sal_Int32 i = 0; i < ids.getLength(); ++i)
                            wids.push_back((long)ids[i]);
                        FFLOG_INFO("[FFPLAY] setFastPropertyValue(MUTE_WINDOWS, ids=%zu, %s) via UNO",
                                   wids.size(), mute ? "true" : "false");
                        FfplayPlayer::SetMuteAll(wids, mute);
                        return;
                    }
                }
            }
            FFLOG_WARN("[FFPLAY] setFastPropertyValue(MUTE_WINDOWS) arg format: "
                       "expected Sequence<Any>{Sequence<int32>(window_ids), mute(bool)}");
            return;
        }
        FFLOG_WARN("[FFPLAY] setFastPropertyValue unknown handle=%ld", (long)nHandle);
    }

    Any SAL_CALL getFastPropertyValue(sal_Int32 nHandle) override {
        // 占位: 静音态真实查询需遍历 g_engines 检查 audio_volume, 当前无需求
        (void)nHandle;
        return Any();
    }
};

extern "C" SAL_DLLPUBLIC_EXPORT css::uno::XInterface* ffplay_get_implementation(
    css::uno::XComponentContext*, css::uno::Sequence<css::uno::Any> const&) {
    // ffplay.so 首次被 soffice.bin 加载时触发: 初始化 logger + 注册 av_log callback
    // (idempotent, std::call_once 内部保证; 多次 ffplay_get_implementation 只首次生效)
    ffplay_log::Init();
    FFLOG_INFO("[FFPLAY] component loaded (ffplay.so dlopen by soffice.bin)");
    return cppu::acquire(new FfplayManager());
}

// ---- 标准组件导出 (SDK 模式, 与 odk/examples/cpp/counter 同款) ----
extern "C" SAL_DLLPUBLIC_EXPORT void SAL_CALL
component_getImplementationEnvironment(
    char const ** ppEnvTypeName, uno_Environment **)
{
    *ppEnvTypeName = CPPU_CURRENT_LANGUAGE_BINDING_NAME;
}

extern "C" SAL_DLLPUBLIC_EXPORT void* SAL_CALL
component_getDescription(char const ** ppDescription)
{
    *ppDescription = "ffplay autonomous media backend (Manager_FFPlay)";
    return nullptr;
}
