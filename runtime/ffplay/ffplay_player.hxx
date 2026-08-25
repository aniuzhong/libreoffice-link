#pragma once
#include <com/sun/star/media/XPlayer.hpp>
#include <cppuhelper/implbase1.hxx>

#include <string>
#include <vector>

// FfplayPlayer — XPlayer 播放器 (ffplay 嵌入引擎, 经验 30/34); 设计见 doc/ffplay-embed.md。
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

    // 静音: 物理窗口句柄归属 (方案 A), 机制见 doc/ffplay-embed.md §7。
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
