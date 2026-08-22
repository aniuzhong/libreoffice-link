// ffplay_player.cxx — XPlayer 真播放器: 基于 ffplay 嵌入引擎 (compat/ffplay_embed.c,
// FFPLAY_EMBED), 复用官方 ffplay 全部播放能力 (解码/同步/音频/seek/循环)。
// 渲染: 引擎 SDL_CreateWindowFrom(LO 媒体子窗口) — 窗口句柄来自 createPlayerWindow
// 的 aArgs[0] (经验 30/34)。
// 生命周期: createPlayer 仅存 URL; createPlayerWindow (窗口句柄就绪) 创建引擎;
// LO 对 XPlayerWindow 的调用 (setPosSize 等) 由 PlayerWindowShell no-op 壳承接
// (引擎自渲染到 LO 子窗口, LO 侧窗口对象无需绘制逻辑)。
#include "ffplay_player.hxx"
#include "ffplay_window.hxx"
#include "ffplay_log.h"  // FFLOG_*

#include "compat/ffplay_engine.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <mutex>
#include <vector>

using namespace css;
using namespace css::uno;

// ---- 静音专项 (方案 A, 2026-08-21 重构): 物理窗口句柄归属 ----
// 设计: PptX 单 slide 多媒体 shape 各起一 XPlayer 实例, LO UNO 无 setMuteAll;
//   SetMuteAll 按物理 parent_window_id 过滤, 替代易错的 session_id 时序归属。
// 触发路径: impresslink.so (主进程) 经 UNO remote ctx
//   createInstance("Manager_FFPlay") → QI XFastPropertySet →
//   setFastPropertyValue(MGR_PROP_MUTE_WINDOWS, {window_ids, mute}) → 调本 SetMuteAll。
//   (FfplayManager 与 g_engines 同处 soffice.bin 子进程, 调用天然在子进程内执行。)
// 线程安全: 注册/注销/SetMuteAll 共享 g_engines_mutex。
// 生命周期: createPlayerWindow 成功注册, ~FfplayPlayer destroy 前注销
//   (避免悬挂指针; do_exit 全局销毁已 #ifndef FFPLAY_EMBED 圈掉, 不二次释放)。
// 归属键 = X11 媒体子窗口 ID (LO mediawindow_impl.cxx createPlayerWindow args[0]),
//   天然全局唯一且与 player 一一对应, 不依赖任何调用时序。
namespace {
struct EngineEntry { void* engine; long window_id; };  // window_id = X11 媒体子窗口 ID
std::mutex g_engines_mutex;
std::vector<EngineEntry> g_engines;
}

void FfplayPlayer::RegisterEngine(void* engine, long window_id) {
    if (!engine) return;
    std::lock_guard<std::mutex> lk(g_engines_mutex);
    g_engines.push_back({engine, window_id});
}

void FfplayPlayer::UnregisterEngine(void* engine) {
    if (!engine) return;
    std::lock_guard<std::mutex> lk(g_engines_mutex);
    for (auto it = g_engines.begin(); it != g_engines.end(); ++it) {
        if (it->engine == engine) { g_engines.erase(it); break; }
    }
}

void FfplayPlayer::SetMuteAll(const std::vector<long>& window_ids, bool mute) {
    std::lock_guard<std::mutex> lk(g_engines_mutex);
    // 空列表 = no-op (不动任何引擎)
    // 共享内核模式 (Linux) 实证 (2026-08-21): 切换 NovaPlayer item 时, 上层对切出/
    //   切入的非 active session 调 SetMute(false), 这些 session 当前可能未创建 media
    //   子窗口 (其 GetMediaWindowIds 返回空)。若空列表走 "all" 分支, 会误伤 active
    //   session 的引擎, 解除其静音。正确语义: 空列表 = 该 session 无需静音的引擎,
    //   no-op 即可, 不影响其他 session。
    // Windows 独立进程模式天然隔离: 本 session 进程内即使没 media 子窗口, 也不会有
    //   其他 session 的引擎在本进程 g_engines 内, "all" 语义历史上无副作用, 但为
    //   语义一致性统一改为 no-op; Windows 模式如需"全静音本进程所有引擎", 应让
    //   WindowsPlatform::GetMediaWindowIds() 真实枚举本 session 媒体子窗口 (TODO)。
    if (window_ids.empty()) {
        FFLOG_INFO("[FFPLAY] SetMuteAll(wids=0, %s) engines=%zu matched=0 (no-op, empty wids)",
                   mute ? "true" : "false", g_engines.size());
        return;
    }
    int count = 0;
    for (auto& e : g_engines) {
        if (std::find(window_ids.begin(), window_ids.end(), e.window_id) != window_ids.end()) {
            ffplay_engine_set_volume(e.engine, mute ? 0 : 100);
            count++;
        }
    }
    FFLOG_INFO("[FFPLAY] SetMuteAll(wids=%zu, %s) engines=%zu matched=%d (filtered)",
               window_ids.size(), mute ? "true" : "false",
               g_engines.size(), count);
}

FfplayPlayer::~FfplayPlayer() {
    if (engine_) {
        UnregisterEngine(engine_); // 先注销再 destroy, 防止 SetMuteAll 命中悬挂
        ffplay_engine_destroy(engine_);
        engine_ = nullptr;
    }
}

// ---- XPlayer 生命周期映射到引擎 ----
void SAL_CALL FfplayPlayer::start() {
    if (engine_)
        ffplay_engine_play(engine_);
    FFLOG_INFO("[FFPLAY] start");
}
void SAL_CALL FfplayPlayer::stop() {
    if (engine_)
        ffplay_engine_pause(engine_);
    FFLOG_INFO("[FFPLAY] stop");
}
sal_Bool SAL_CALL FfplayPlayer::isPlaying() {
    return engine_ ? ffplay_engine_is_playing(engine_) : false;
}
double SAL_CALL FfplayPlayer::getDuration() {
    return engine_ ? ffplay_engine_get_duration(engine_) : 0.0;
}
void SAL_CALL FfplayPlayer::setMediaTime(double seconds) {
    if (engine_)
        ffplay_engine_seek(engine_, seconds);
}
double SAL_CALL FfplayPlayer::getMediaTime() {
    return engine_ ? ffplay_engine_get_media_time(engine_) : 0.0;
}
void SAL_CALL FfplayPlayer::setPlaybackLoop(sal_Bool bLoop) {
    loop_ = bLoop;
    if (engine_)
        ffplay_engine_set_loop(engine_, bLoop ? -1 : 1); // -1 = 无限循环 (ffplay 语义)
}
sal_Bool SAL_CALL FfplayPlayer::isPlaybackLoop() {
    return loop_;
}
void SAL_CALL FfplayPlayer::setVolumeDB(sal_Int16 db) {
    // LO 音量以 dB 计 (0 默认); 映射到 0-100%: db<=-60 静音, db>=0 满音量
    double pct = 100.0 * pow(10.0, db / 20.0);
    volume_pct_ = (int)(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
    if (engine_)
        ffplay_engine_set_volume(engine_, volume_pct_);
}
sal_Int16 SAL_CALL FfplayPlayer::getVolumeDB() {
    return (sal_Int16)(20.0 * log10(volume_pct_ / 100.0 + 1e-9));
}
void SAL_CALL FfplayPlayer::setMute(sal_Bool bMute) {
    mute_ = bMute; // 跟踪静音态 (Phase 2: isMute 不再硬编码 false)
    if (engine_)
        ffplay_engine_set_volume(engine_, bMute ? 0 : volume_pct_);
}
sal_Bool SAL_CALL FfplayPlayer::isMute() {
    return mute_; // 实例级静音态 (UNO XPlayer 单实例查询用)
}
css::awt::Size SAL_CALL FfplayPlayer::getPreferredPlayerWindowSize() {
    return { 640, 480 }; // 引擎自适配视频尺寸, 此值仅占位
}

Reference<css::media::XPlayerWindow> SAL_CALL FfplayPlayer::createPlayerWindow(
    const css::uno::Sequence<css::uno::Any>& rArguments) {
    // 参数 (mediawindow_impl.cxx:436-442):
    //   [0] sal_IntPtr 媒体子窗口 X 句柄 (公开整数类型, SDK 可解析)
    //   [1] awt::Rectangle 位置/尺寸 (公开 IDL 结构)
    //   [2]/[3] LO 内部指针 (SDK 模式不解析)
    sal_IntPtr parent = 0;
    css::awt::Rectangle rect;
    if (rArguments.getLength() > 0)
        rArguments[0] >>= parent;
    if (rArguments.getLength() > 1)
        rArguments[1] >>= rect;
    FFLOG_INFO("[FFPLAY] createPlayerWindow parent=0x%lx rect=%dx%d@(%d,%d)",
               (unsigned long)parent, (int)rect.Width, (int)rect.Height,
               (int)rect.X, (int)rect.Y);

    // 引擎创建: 窗口句柄就绪, 渲染到 LO 媒体子窗口
    if (!engine_ && !url_.empty() && parent) {
        engine_ = ffplay_engine_create(url_.c_str(), (void*)parent);
        if (engine_) {
            // 归属键 = X11 媒体子窗口 ID (物理句柄, 跨进程可见)
            RegisterEngine(engine_, (long)parent);
            FFLOG_INFO("[FFPLAY] engine create OK window_id=%ld (%s)",
                       (long)parent, url_.c_str());
        } else {
            FFLOG_ERR("[FFPLAY] engine create FAILED (%s)", url_.c_str());
        }
    }
    // 返回 no-op 窗口壳 (引擎自渲染; LO 侧调用由 PlayerWindowShell no-op 承接)
    return new PlayerWindowShell(getenv("DISPLAY"), parent, rect);
}

Reference<css::media::XFrameGrabber> SAL_CALL FfplayPlayer::createFrameGrabber() {
    return nullptr;
}
