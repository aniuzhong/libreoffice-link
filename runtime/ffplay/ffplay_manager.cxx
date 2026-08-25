// ffplay_manager.cxx — Manager_FFPlay 组件 (注入 + 静音 + 日志, service:
// com.sun.star.comp.avmedia.Manager_FFPlay)。注入机制见 [ffplay-embed] §1,
// 静音方案 A 见 §7, 日志见 §8。
#include <cstdio>

#include <com/sun/star/beans/XFastPropertySet.hpp>
#include <com/sun/star/media/XManager.hpp>
#include <com/sun/star/uno/XComponentContext.hpp>
#include <cppuhelper/implbase2.hxx>
#include <uno/environment.h>
#include <uno/lbnames.h>

#include "ffplay_log.h"  // FFLOG_* + Init() (注册 av_log callback)
#include "ffplay_player.hxx"

using namespace css;
using namespace css::uno;
using rtl::OUString;

// XFastPropertySet handle 0 = MGR_PROP_MUTE_WINDOWS (per-window 静音, 见 [ffplay-embed] §7)。
//   Any = Sequence<Any> = {Sequence<sal_Int32>(window_ids), mute(bool)}
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
