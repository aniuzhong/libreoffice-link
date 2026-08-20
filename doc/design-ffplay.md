# FFplay 嵌入专项（HANDOFF.md 六章伴随文件）

> ffplay 嵌入引擎 (office_runtime/ffplay, 补丁式复用 FFmpeg ffplay.c) 的尺寸链治理。
> 关联经验 34 (补丁式复用)、19b (软件渲染/Xvfb 无 GPU)、37 (并发创建竞态)。
> 探针: xvfb_calc_demo/ffplay_window_size_probe.cpp (engine 组, 直接验证引擎)。

## 1. video_open 尺寸修复 — 已闭环 (2026-08-19)

> 修复后无回归。详细尺寸链/探针验证见 git 历史 commit + ffplay_window_size_probe.cpp。

**问题**: ffplay_embed.c `video_open()` 用 `default_width/height` (640x480) 设置 `is->width/is->height`, 嵌入模式下与外部 X11 窗口实际尺寸无关, 视频位置/缩放错误。

**尺寸链** (LO → ffplay): mediawindow_impl.cxx Resize → createPlayerWindow aArgs[0]=parent / aArgs[1]=rect → ffplay_player.cxx 解析 (rect 被忽略) → ffplay_engine_create → SDL_CreateWindowFrom → video_open `is->width=default_width=640` ← **bug**。

**修复 (方案A)**: video_open renderer 创建后加 `SDL_GetWindowSize(is->window, &w, &h)` 替代 640x480 硬编码。技术依据: SDL X11 驱动 `X11_CreateWindowFrom → SetupWindowData → XGetWindowAttributes` 设置 window->w/h (SDL2 源码实证)。修改文件: ffplay_embed.c (video_open) + ffplay_embed.patch 回填 (经验 34)。

**验证**: ffplay_window_size_probe (engine 组, 三方对比 期望/X11/ffplay, 500x300 故意 ≠ 640x480)。修复后 PASS: 三方一致。Demo 含视频 pptx 视频尺寸正确。

**开放问题 (升级路径)**: 方案A 仅首次创建, 运行时 X11 resize 不感知 (放映期尺寸固定为常见场景)。升级路径: 方案B (`ffplay_engine_set_window_size` API + `PlayerWindowShell::setPosSize` 调用) / 方案C (SDL_WINDOWEVENT_RESIZED 事件监听)。

## 2. 多实例根治 (2026-08-19 落地)

> ffplay 单实例 CLI 遗产: 文件作用域全局 `window`/`renderer`/`audio_dev` 持有"唯一实例"状态。embed 补丁让 `VideoState` per-instance, 却未把渲染/音频入口的全局状态实例化 → 多实例 = 多泵线程写同一份全局。经验 37 的 SDL_FRAMEBUFFER_ACCELERATION=0 + SOFTWARE renderer 修了 GL/swrast 崩溃, 但软件渲染下**残留的全局状态竞态仍在** (侥幸不崩, 非构造性安全)。本根治处理之。

**三个 bug (代码层; 原独立 .md (ffplay-multi-instance.md, 2026-08-19 已整合本节) 分析方向正确但探针虚构, 此处实证闭合)**:

| # | bug | 机制 | 位置 |
|---|---|---|---|
| 1 | audio_dev 跨实例覆盖/误关 | 全局 `audio_dev` 被实例2 open 覆盖; 实例1 close 经全局指针关掉实例2 的设备 | audio_open L2620 / stream_component_close L1240 |
| 2 | 渲染全局竞态 | 两泵线程并发 `window=is->window; renderer=is->renderer;` 后 SDL_RenderPresent, 交叉使用对方 renderer (UB) | video_display L1429-1430 |
| 3 | do_exit 全局销毁误删他实例 | `SDL_DestroyRenderer(renderer)`/`DestroyWindow(window)` 销毁全局=最后渲染实例的, 若为实例B 则误删 B | do_exit L1340-1343 |

**修复 (三处一并治, 治病不治症 — 不加全局锁兜底, 而是消灭/收敛全局状态)**:
- **① audio_dev 下沉 VideoState**: `SDL_AudioDeviceID audio_dev` 移入 `VideoState` (L245); audio_open 写 `is->audio_dev` (opaque 即 VideoState, callback userdata 已 per-instance); pause/close 读 `is->audio_dev`; 删全局 L376。SDL2 支持多次 open 默认设备并内部混音, 多实例音频天然分流
- **② render_mutex 串行 video_display**: `static SDL_mutex *render_mutex` (L378), 随 SDL 首次 init 创建一次 (创建串行由 BootLock/500ms 错峰保证, 经验 37 ④)。video_display 进出 Lock/Unlock — video_open 仅由 video_display 调用 (L1433), 所有全局 renderer 读位 (RenderClear/Copy/Present + image/audio display helpers) 均在 video_display 调用树内, **一把锁覆盖整个全局访问面**。软件渲染本就单 CPU, 串行无并发损失 (经验 19b/37)
- **③ 每实例渲染资源销毁**: stream_close 销毁 `is->window`/`is->renderer` (实例自有, L1321-1328); do_exit 全局 destroy 以 `#ifndef FFPLAY_EMBED` 圈掉 (CLI 单实例走全局, embed 走 per-instance)。消灭全局悬空 + 跨实例误删 + per-instance 泄漏三重问题

**探针验证** (ffplay_engine_probe 扩展销毁隔离段, test_tone.mp4 = 20s h264+aac 带音轨, DISPLAY=:90):
- 两实例并发: audio_clock 推进 (0.51→2.50, 音频设备独立开启 + callback 运行); duration=20.02s 双实例一致
- **销毁隔离 OK**: `destroy[0] 后 [1]: t 7.25 -> 8.35 playing 1->1 销毁隔离 OK` — 销毁实例0 后实例1 仍推进/仍播放 (audio_dev + renderer 双隔离生效)
- **控制 OK**: 实例1 seek(2.0) 被接受 (t 8.35→0.73 回跳), playing=1 — renderer/audio_dev 未被误删
- done rc=0 无崩

**判据**: `iso_ok = alive_after && (t_after > t_before + 0.3)`; `ctrl_ok = is_playing && seek_accepted (t_seek < t_after)`。修复前预期 (未实测, 非 git 无法 stash 回退): destroy[0] → do_exit 全局 destroy/renderer + CloseAudioDevice(全局) → 实例1 渲染崩/音频停。代码分析 + 修复后实证闭合。

**根因证据 (修复前段错误回溯, 源自 ffplay-multi-instance.md 整合)**:
两实例并发时 crash 栈: `SDL_Blit_ARGB8888_RGB888_Scale → SDL_SoftBlit → SW_RunCommandQueue → SDL_RenderPresent_REAL → video_display → video_refresh → ffplay_engine_pump_thread`
根因: 两泵线程并发写全局 `window`/`renderer` 指针 —
```
// video_display():
window = is->window;       // 线程 A: 设为自己的窗口
renderer = is->renderer;   // 线程 A: 设为自己的渲染器
                           // ← 线程 B: 覆盖为 B 的渲染器
SDL_RenderPresent(renderer); // 线程 A: 用的是 B 的渲染器 → 崩溃!
```
**关键纠偏**: 原独立 .md 探针虚构把 crash 归因"音频设备共享", 实证发现音频路径无 crash (SDL2 支持多设备内部混音), crash 实际在渲染竞态。修复方向随之调整: 不动音频全局状态 (改用 per-instance audio_dev), 引入 render_mutex 串行 video_display 全局访问面。

**与 ijkplayer 对照** (file:///home/hido/仓库/ijkplayer): ijkplayer 用 `FFPlayer` 结构体 + per-instance `SDL_Aout`/`SDL_Vout` 彻底消灭全局 (工业级根治佐证); Nova 取其"方向" (全局进实例结构体), 保持 patch 模式 (Nova 改动面窄, ijksdl shim + fork 分家对 Nova 过重)。

**audio_volume 原子化**: 移出本根治 (int 对齐读写 x86 实践原子, 属单实例线程安全细节, 非多实例正确性)。若 Phase 2 静音链路引入高频 setMute 调用再评估。

**关联经验**: 34 (改 embed.c 必须回填 patch, 已回填 401 行) / 37 (多实例并行播放, SDL 修复; 本根治补软件渲染下残留全局竞态) / 19b (Xvfb 恒无 GPU, 软件渲染串行无并发损失)
**补丁纪律**: 改 ffplay_embed.c 必须回填 ffplay_embed.patch (经验 34); compat/ffplay.c 与 SDK 上游 diff=0

## 3. 静音专项 (2026-08-19 落地, C1 简化版)

> PptX 单 slide 多媒体 shape 各起一 XPlayer 实例, LO UNO 无 setMuteAll 接口,
> 需上层 (LibreOfficeImpressManager::SetMute override) 跨进程触发 soffice 子进程内
> ffplay.so 全局静音。本节记录方案演进与最终落地的 C1 简化版。

### 3.1 方案演进

**方案 A (dlopen 跨组件, 已退役)**: impresslink.so (主进程) `link_utils::MuteAllFfplayEngines`
通过 dlopen("ffplay.so") + dlsym("ffplay_set_mute_all") 触发。**失败根因**:
office_runtime.cpp BootstrapOffice 用 osl_executeProcess 启动独立 soffice.bin 子进程,
主进程 dlopen ffplay.so 拿到的是主进程副本的空 g_engines 表, **engines 永远=0**,
跨进程不可见。回归日志 (Phase 1 验证):
```
[FFPLAY] SetMuteAll(true) engines=0    ← 期望 ≥1, 实际 0
[ImpressLink] SetMute(true) -> ffplay OK  ← 链路通但无效
```

**方案 C1 简化版 (UNO 远程调用, 已落地)**: 利用 ffplay.so 本身是注册到 ffplay.rdb 的
标准 LO 组件 (Manager_FFPlay, 子进程内被 dlopen), UNO ServiceManager 知道它的存在。
给它加 XFastPropertySet 接口 (handle 0 = MUTE_ALL), 主进程通过 remote ctx
createInstance + QI + setFastPropertyValue 跨进程触发。复用 UNO pipe + urp
marshalling 现成 RPC 机制, 无需打 LO 源码补丁, 无需自建 IPC。

### 3.2 触发链路 (C1 简化版)

```
UI checkBoxMute (NovaPlayerDemod)
→ PPTPlayerItem::SetMute → NovaPPTPlayer → PPTInstance → PPTCore::SetMute
→ OfficeLibraryManager::NPptSetMute → m_NPptSetMute 函数指针
→ libNovaOfficeCore.so: NPptSetMute
→ GetPptManager(guid)->SetMute(mute)                  [虚函数分发]
→ LibreOfficeImpressManager::SetMute                  [Phase 2 override]
→ m_pSetMute(m_session, mute)                        [dlsym 加载的 ImpressSessionSetMute]
→ ImpressSession::SetMute                             [impresslink.so, 主进程]
→ ctx_->getServiceManager()->createInstanceWithContext("Manager_FFPlay", ctx_)
                                                       [UNO marshalling 跨进程 pipe]
→ soffice.bin 子进程内 FfplayManager 构造             [ffplay.so 子进程内]
→ QI XFastPropertySet → setFastPropertyValue(0, true) [跨进程调用]
→ FfplayManager::setFastPropertyValue(0, true)        [子进程内]
→ FfplayPlayer::SetMuteAll(true)                      [静态方法, 子进程内]
→ 遍历 g_engines (子进程模块级静态表)
→ ffplay_engine_set_volume(engine, 0)                  [真正静音生效]
```

### 3.3 关键设计点

**① ctx_ 性质 (remote)**: ImpressSession::ctx_ 是 BootstrapOffice 返回的 remote
XComponentContext (office_runtime.cpp:380-391 UnoUrlResolver::resolve 后 return remote)。
ctx_ 不是 cppu::defaultBootstrap_InitialComponentContext 的 local (那个仅作 UnoUrlResolver
客户端能力, 不传出 BootstrapOffice 边界)。

**② Manager_FFPlay 非 singleton (重要)**: ffplay.rdb 只声明 `<implementation>` + `<service>`,
无 `<singleton>` 元素。每次 createInstance 都 new 一个新 FfplayManager 实例。
**但 C1 简化版不受影响**: SetMuteAll 是 ffplay.so 模块级静态方法, 操作 g_engines
模块级静态表 (子进程全进程共享), 新 Manager 实例同样能调到已注册的全部引擎。

**③ g_engines 仍是 ffplay.so 模块级静态表**: 与方案 A 一致, 不改 RegisterEngine/
UnregisterEngine/SetMuteAll 设计。仅触发方式从 dlopen C ABI 换成 UNO 远程调用。

**④ XFastPropertySet 不继承 XPropertySet**: 实现时仅需 setFastPropertyValue +
getFastPropertyValue 两个虚函数, 不需要实现 XPropertySet 的 7 个方法 (易踩坑,
原首版误继承导致编译错误)。

### 3.4 探针可行性验证 (ffplay_inject_probe 扩展)

**早期 6 层探针路径** (源自 ppt-mute.md 整合, 2026-08-19; Phase 2 落地前的诊断 SOP):

| 层 | 探针/方法 | 结论 |
|---|---|---|
| 1. ffplay 引擎层 | ffplay_mute_probe (engine 组) — set_volume/get_volume 直调 | ✅ 引擎层 set_volume(0) 设 audio_volume=0,SDL 混音器输出静音;静音后 media_time 仍推进,解码正常 |
| 2. FfplayPlayer 映射 | 代码审查 ffplay_player.cxx setMute/setVolumeDB/isMute | ✅ setMute/setVolumeDB 映射正确;❌ isMute 硬编码 false (Phase 2 已修) |
| 3. LO UNO XPlayer 接口 | ffplay_mute_uno_probe (office 组) — createPlayer+setMute/isMute | ⚠️ createPlayer 后 duration=0, start() 后 mediaTime=0 — 因 createPlayerWindow 未被调 (引擎在 createPlayerWindow 创建, parent=0 时不创建); LO 真实流程会调,正常播放时引擎存在 |
| 4. LibreOfficeImpressManager | 代码审查 LibreOfficeImpressManager.cpp | ❌ 未 override SetMute/GetMuteStatus,走基类 return false (Phase 2 已修) |
| 5. impresslink C ABI | ffplay_mute_abi_probe (dlopen 组) — nm + dlsym | ❌ ImpressSessionSetMute 缺失 (Phase 2 已加) |
| 6. 完整链路验证 | Phase 2 落地后 media_green_probe 端到端 | ✅ engines=1 SetMuteAll 命中 (见 3.5) |

**6 层探针 SOP 价值**: 不需要每次都从 UI 起跑,可逐层定位问题(引擎层/映射/UNO/ABI/管理器),加快回归。Phase 2 后已知问题 1-5 全部修复。

**Phase 2 UNO 跨进程基础路径验证** (ffplay_inject_probe 扩展, 2026-08-19):
在跑端到端回归前, 先用 ffplay_inject_probe 验证 UNO 跨进程远程调用基础路径:
```cpp
// 通过 remote ctx createInstance + createPlayer + 调 setMute/isMute 远程往返
auto mgr = sm->createInstanceWithContext("Manager_FFPlay", ctx);  // OK 子进程加载
auto player = mgr->createPlayer(url);                            // OK 跨进程创建
player->setMute(true);                                           // OK 跨进程生效
sal_Bool m = player->isMute();                                  // OK 读回正确
```
日志验证 (file:///...test_media/media1.mp4 + ORT_MEDIA_BACKEND=ffplay):
```
[FFPLAY] 组件被加载!                              ← createInstance 触发子进程加载
[FFPLAY] FfplayManager 构造! (Manager_FFPlay 注入生效)
[INJECT] Manager_FFPlay created OK
[FFPLAY] createPlayer 被调用! URL=... (ffplay 接管媒体!)
[UNO-REMOTE] initial isMute=0
[UNO-REMOTE] after setMute(true) isMute=1  OK 跨进程生效
[UNO-REMOTE] after setMute(false) isMute=0  OK 恢复
[UNO-REMOTE] second createInstance: mgr2=OK  (新实例, 非 singleton — C1 需伪单例改造)
```
结论: UNO marshalling 完全支持跨进程调用 XPlayer::setMute/isMute, 往返状态正确。
"奇巧"处 —— 不需要打 LO 源码补丁, ffplay.so 自己作为 LO 组件就能接收远程消息。

### 3.5 端到端回归 — 已闭环 (2026-08-19)

PASS: video-loop.pptx + ORT_MEDIA_BACKEND=ffplay, engines=1 SetMuteAll(true/false) 命中 (UNO marshalling 跨进程生效)。详细日志见 git 历史。

### 3.6 ABI 不变性 (上层透明)

- impresslink C ABI: `ImpressSessionSetMute(void* session, int mute)` 签名不变
- LibreOfficeImpressManager::SetMute override 实现不变 (D 层, 已在 libNovaOfficeCore.so)
- libNovaOfficeCore.so 不需要重新构建 (D 层调用方式未变, 只是内部 ImpressSessionSetMute 实现改了)

### 3.7 教训

1. **跨进程通信前先确认进程架构**: 方案 A 假设 ffplay.so 与 impresslink.so 同进程,
   未考虑 office_runtime 的子进程架构 (BootstrapOffice 用 osl_executeProcess 启动
   独立 soffice.bin)。dlopen 拿到的是主进程副本, g_engines 跨进程不可见。
2. **UNO 是 LO 原生 RPC 机制**: 跨进程调用 LO 子进程内组件应优先用 UNO pipe +
   urp marshalling, 不应自建 dlopen/dlsym 链路。LO 组件天然能接收 UNO 远程消息。
3. **Manager 非 singleton 不一定是问题**: 即使每次 createInstance 都 new 新实例,
   只要触发的方法操作的是模块级静态表 (而非实例成员), 新实例同样可访问全部状态。
4. **XFastPropertySet 不继承 XPropertySet**: 实现时只需 setFastPropertyValue +
   getFastPropertyValue, 不要误实现 XPropertySet 的 7 个方法。
5. **先探针验证再实施**: 方案 C 的 UNO 远程调用路径先用 ffplay_inject_probe 探针验证
   基础可行性 (createInstance + setMute/isMute 往返), 再做端到端实施, 避免大量返工。

**关键代码引用**: ffplay_manager.cxx (XFastPropertySet) / impress_session.cpp:437 (SetMute UNO) / ffplay_player.cxx:51 (SetMuteAll 静态方法)

## 4. ffplay 日志专项 (2026-08-20 落地)

> ffplay.so (soffice.bin 子进程内被 dlopen) 自己初始化一份 spdlog logger,
> 落盘 ~/.office-link/logs/ffplay_<pid>.log。复用 ORT_LOG/ORT_LOG_LEVEL 控制,
> 与主进程 office_<pid>.log 同目录同格式, 跨进程时序对照友好。
> ffmpeg 库内 av_log 经 callback 接入, 输出 [FFmpeg/<module>] 前缀。

### 4.1 设计

**目录/文件命名**:
- 目录: `~/.office-link/logs/` (复用 office_paths::logs_dir(), 与 OfficeLog 同处)
- 文件: `ffplay_<pid>.log` (pid = soffice.bin 子进程 pid; rotating 5MB×3)
- 格式: `[%Y-%m-%d %H:%M:%S.%e] [%l] %v` (与 OfficeLog 一致)
- 控制: ORT_LOG=both|file|stderr|off + ORT_LOG_LEVEL=debug|info|warn|error

**flush_on(info)**: 主进程/子进程异常退出 (SIGKILL/SIGTERM 或 dlclose 未触发析构)
时 spdlog 默认 buffer 不 flush 会丢日志。`lg->flush_on(spdlog::level::info)` 保证
每条 info 及以上立即落盘, 同时支持运行期 `tail -f` 实时监控。OfficeLog (office_runtime.cpp
InitOfficeLog) 同样加 flush_on。

**为什么 ffplay.so 不能复用 OfficeLog**: ffplay.so 在 soffice.bin 子进程内 dlopen,
office_runtime.so 在主进程内 (BootstrapOffice 用 osl_executeProcess 启动子进程)。
跨进程 dlsym 不可行, ffplay.so 必须自己初始化 spdlog logger。

**av_log 接入**: ffmpeg 库内 av_log() 经 av_log_set_callback 注册的自定义 callback
(AvLogToSpdlog) 转发到 spdlog, 输出 `[FFmpeg/<module>] <message>` 前缀。
module = AVClass.class_name (如 "avi"/"h264"/"mp3"); avcl==NULL 时 module="ffmpeg"。
av_log_set_level 同步 ORT_LOG_LEVEL (debug→AV_LOG_DEBUG=48, info→AV_LOG_INFO=32 等)。

**为什么单文件混合而非按视频拆分**:
1. av_log callback 是进程级全局, 多视频并发时无法区分来源
2. g_engines 全局表 — SetMuteAll 操作所有引擎, 不属于任何单视频
3. 跨视频时序对照需求 (静音操作影响所有视频)
4. 引擎 ID 标记 + grep 即可单视频提取 (后续可加 engine_id)

### 4.2 实施

| 文件 | 改动 |
|---|---|
| `ffplay_log.h` (新建, header-only) | spdlog logger 懒初始化 + FFLOG_* 宏 + AvLogToSpdlog callback + SyncAvLogLevel + Init() (注册 callback) |
| `ffplay_manager.cxx` | FFLOG_* 替换 fprintf + `ffplay_log::Init()` 在 ffplay_get_implementation 入口注册 |
| `ffplay_player.cxx` | FFLOG_* 替换 fprintf (英文化) |
| `ffplay_window.cxx` | FFLOG_* 替换 fprintf (英文化) |
| `ffplay_embed.c` (L1400 video_open / L4040 ENGINE-DBG) | fprintf → av_log (经 callback 自动落盘) |
| `ffplay_embed.patch` | 同步回填 (经验 34) |
| `ffplay/CMakeLists.txt` | 加 spdlog include 路径 (`../../include`) |
| `office_runtime.cpp` InitOfficeLog | 加 `logger->flush_on(spdlog::level::info)` |
| `office_runtime.cpp` BootstrapOffice | osl_getProcessInfo 拿子进程 pid + 配对提示文案 |

### 4.3 配对方式

**主进程 office log** 内含提示:
```
[OfficeRuntime] soffice child started initial_pid=N (soffice script;
  final soffice.bin pid differs due to 2-stage fork;
  ffplay log: see latest ~/.office-link/logs/ffplay_*.log by mtime)
```

**注意 pid 不匹配**: osl_executeProcess 启动 soffice 脚本 (initial_pid), 脚本 exec
oosplash → fork soffice.bin (final pid), ffplay.so 在最终 soffice.bin 内执行 getpid()。
配对方式: `ls -t ~/.office-link/logs/ffplay_*.log | head -1` (按 mtime 排序最新)。

### 4.4 回归验证 — 已闭环 (2026-08-20)

PASS: ffplay log 落盘 (4357 字节, flush_on 修复 0 字节问题) + 日志全英文 + av_log 接入 (前缀 [FFmpeg/ffmpeg] [FFPLAY]) + engines=1 SetMuteAll 命中 (静音不回归) + stderr 已无裸 fprintf + flush_on 保证实时 tail -f。详细日志见 git 历史。

### 4.5 不动清单 (patch 纪律)

- **ffplay_embed.c L1818**: `if (show_status == 1 && AV_LOG_INFO > av_log_get_level()) fprintf(stderr, "%s", buf.str);` — 上游 ffplay.c fallback 路径, 仅 show_status==1 时触发, 嵌入模式不走
- **cmdutils.c / ffplay.c**: 上游 ffplay.c 代码, 经验 34 patch 纪律不动
- **ffplay_embed.c L1818 fprintf**: 保留 (上游残留)

**已知小问题**: [FFmpeg/ffmpeg] 后偶尔空消息 (ffmpeg 退出路径 av_log(NULL, AV_LOG_QUIET, "") 触发, 不影响功能)

---

## 关联索引 (本文件)

**关联经验**: 17/18/19 (gst → ffplay 替代链) / 28/29/30 (ffplay 注入 SDK 模式 + ORT_MEDIA_BACKEND) / 34 (补丁式复用回填纪律) / 37 (多实例并行播放)
**关联待办**: HANDOFF.md 3.1 ffplay 能力增强 (XFrameGrabber/硬解/媒体信息) / ffplay 引擎并发创建竞态
**已闭环**: 2026-08-19 video_open 尺寸修复 (§1) / 2026-08-19 多实例根治 (§2) / 2026-08-19 静音专项 (§3) / 2026-08-20 日志专项 (§4)
**探针**: ffplay_window_size_probe / ffplay_engine_probe / ffplay_inject_probe / media_green_probe
**项目级上下文**: HANDOFF.md 1.6 媒体后端 / 1.6 GL 全禁用 / 1.6 诊断开关
