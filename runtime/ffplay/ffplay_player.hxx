#pragma once
#include <com/sun/star/media/XPlayer.hpp>
#include <cppuhelper/implbase1.hxx>

#include <string>
#include <vector>

// FfplayPlayer — 真播放器 (基于 ffplay 嵌入引擎, 经验 30/34):
// 引擎 = compat/ffplay_embed.c (FFPLAY_EMBED), 复用 ffplay 全部播放能力
// (解码/同步/音频/seek), 渲染到 LO 媒体子窗口 (SDL_CreateWindowFrom)。
// createPlayer 仅存 URL; 引擎在 createPlayerWindow (窗口句柄就绪) 时创建。
class FfplayPlayer final : public cppu::WeakImplHelper1<css::media::XPlayer> {
public:
    // XPlayer 接口 (实现见 ffplay_player.cxx)
    void SAL_CALL start() override;
    void SAL_CALL stop() override;
    sal_Bool SAL_CALL isPlaying() override;
    double SAL_CALL getDuration() override;
    void SAL_CALL setMediaTime(double) override;
    double SAL_CALL getMediaTime() override;
    void SAL_CALL setPlaybackLoop(sal_Bool) override;
    sal_Bool SAL_CALL isPlaybackLoop() override;
    void SAL_CALL setVolumeDB(sal_Int16) override;
    sal_Int16 SAL_CALL getVolumeDB() override;
    void SAL_CALL setMute(sal_Bool) override;
    sal_Bool SAL_CALL isMute() override;
    css::awt::Size SAL_CALL getPreferredPlayerWindowSize() override;
    css::uno::Reference<css::media::XPlayerWindow> SAL_CALL createPlayerWindow(
        const css::uno::Sequence<css::uno::Any>& rArguments) override;
    css::uno::Reference<css::media::XFrameGrabber> SAL_CALL createFrameGrabber() override;

    ~FfplayPlayer() override; // 引擎清理
    void SetUrl(const std::string& url) { url_ = url; } // createPlayer 时由 Manager 传入

    // 静音专项 (方案 A, 2026-08-21 重构): 用 parent_window 物理句柄做归属键,
    // 替代易错的 session_id 时序归属。引擎注册时记录 createPlayerWindow 传入的
    // 父窗口 X11 ID (LO mediawindow_impl.cxx args[0]); ImpressSession::SetMute 通过
    // XQueryTree 枚举自己放映主窗口下所有子窗口 ID 列表, 经 UNO remote ctx
    // setFastPropertyValue(MGR_PROP_MUTE_WINDOWS, {ids, mute}) 转发到本方法,
    // 子进程内遍历 g_engines 按 window_id 精确匹配。空列表 = no-op (不动任何引擎,
    // 避免共享内核模式下某 session 无 media 子窗口时误伤其他 session 引擎)。
    // 引擎注册/注销时机: createPlayerWindow 成功注册, ~FfplayPlayer 注销。
    static void SetMuteAll(const std::vector<long>& window_ids, bool mute);

private:
    std::string url_;          // createPlayer 的媒体 URL (引擎创建时使用)
    void* engine_ = nullptr;   // ffplay 引擎句柄 (createPlayerWindow 时创建)
    int volume_pct_ = 100;     // 音量 0-100 (volumeDB 换算基准)
    bool mute_ = false;        // 当前静音态 (isMute 不再硬编码 false)
    bool loop_ = false;

    static void RegisterEngine(void* engine, long window_id);
    static void UnregisterEngine(void* engine);
};
