// ffplay_engine.h — ffplay 嵌入引擎 C API (与 ffplay_embed.patch 配套, 非上游)。
// 由 compat/ffplay_embed.c 在 FFPLAY_EMBED 编译下导出; 语义与 LO avmedia
// XPlayer 对齐 (start/stop/pause/seek/volume/loop)。
// 多实例: 每实例独立 VideoState; 渲染目标 = create 时传入的外部 X 窗口句柄。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 创建播放实例: url = 媒体文件, parent_window = 外部 X11 窗口句柄 (LO 媒体子窗口)。
// 返回引擎句柄 (失败返回 NULL)。create 后即开始解码 (read_thread), 播放状态为暂停
// (与 ffplay 官方 Create 后语义一致, 由 ffplay_engine_play 开始)。
void *ffplay_engine_create(const char *url, void *parent_window);

// 销毁实例 (停止线程 + 清理; 不退出进程, SDL_Quit 由引擎末实例统一处理)。
void ffplay_engine_destroy(void *handle);

// 播放控制 (与 LO XPlayer 对齐)
void   ffplay_engine_play(void *handle);       // start/resume
void   ffplay_engine_pause(void *handle);      // pause
int    ffplay_engine_is_playing(void *handle);
void   ffplay_engine_seek(void *handle, double seconds);  // setMediaTime
double ffplay_engine_get_media_time(void *handle);        // getMediaTime
double ffplay_engine_get_duration(void *handle);          // getDuration
void   ffplay_engine_set_loop(void *handle, int count);
void   ffplay_engine_set_volume(void *handle, int percent); // 0-100

// 查询当前音量 (0-100, 与 set_volume 的 percent 语义一致)。
// 探针用: 验证 set_volume/setMute 是否真正修改了引擎内部音量值。
// 返回 -1 表示 handle 无效。
int    ffplay_engine_get_volume(void *handle);

// 调试: 打印内部时钟/队列状态 (探针用)
void ffplay_engine_debug(void *handle);

// 查询引擎内部视频绘制矩形尺寸 (is->width/is->height, 由 video_open 设置)。
// 探针用: 对比 LO 期望尺寸 / X11 窗口实际尺寸 / 引擎内部尺寸, 验证尺寸链一致性。
// 返回 0 表示 video_open 尚未执行 (播放开始前 is->width/height=0)。
void ffplay_engine_get_window_size(void *handle, int *w, int *h);

#ifdef __cplusplus
}
#endif
