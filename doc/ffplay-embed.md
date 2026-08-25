# FFplay 嵌入专项（HANDOFF.md 七章伴随文件 · 单点全源）

> **本文件 = ffplay 的唯一真相源**：收集自 `runtime/ffplay/` 全部模块头注释 +
> HANDOFF 分散条目，按"分类叙述"组织。代码注释只做锚点/一句话，详情一律这里查。
> 关联经验：17/18/19 (gst → ffplay 替代链) / 19b (Xvfb 无 GPU 软件渲染) / 28 (gst 依赖检测) /
> 28-30 (ffplay 注入 SDK 模式 + ORT_MEDIA_BACKEND) / 34 (补丁式复用回填纪律) / 37 (多实例并行播放)。

---

## 0. 定位与回退关系（A 类）

ffplay = **无 GPU 环境下的软解媒体后端**，替代 gst 崩溃链。

- **价值**：无 gst 时提供播放能力 + 音频；**不是防卡死**（卡死由 18 已修的同款 sink 问题引起）。媒体必需最小集（gst 回退用）：libgstreamer1.0-0 + plugins base/good/bad/ugly + x；gl 移除更安全（经验 28）。
- **后端开关**：默认 ffplay（`ORT_MEDIA_BACKEND`），gstreamer 为验证过的回退路径。EnsureKernel setenv 不覆盖宿主（经验 30）。
- **GL 全禁用**：`SAL_DISABLEGL=1`（转场，经验 21）+ ffplay 的 `SDL_FRAMEBUFFER_ACCELERATION=0` + SOFTWARE renderer（经验 37）——Xvfb 恒无 GPU，一切渲染固定软件路径（经验 19b）。ffplay 媒体画面在 LO 渲染管线之外（design-framepump J 类边界）。
- **LO 源码改动**：两处已固化远端 commit `83e0b9c3e`（gstplayer.cxx + mediawindow_impl.cxx）。

## 1. 注入机制（B 类）

- **组件**：`com.sun.star.comp.avmedia.Manager_FFPlay`，注册于 `ffplay.rdb`（`<implementation>` + `<service>`，无 `<singleton>`）。
- **触发**：LO `mediawindow_impl.cxx` 读 `ORT_MEDIA_BACKEND=ffplay` 选本组件（方案 A，经验 30）。
- **SDK 模式的坑**：`WeakImplHelper1`（非 `WeakImplHelper`）、`component_getImplementationEnvironment` 必须导出、rdb `environment="gcc3"`（经验 29）。
- **注入判据** = 帧间差异（不再是"全绿壳"，经验 34）。
- 代码入口：`ffplay_get_implementation`（ffplay.so 首次被 soffice.bin 加载时调用，见 §8 日志初始化）。

## 2. 引擎复用 / 补丁纪律（C 类）

嵌入引擎 = 补丁式复用 FFmpeg 官方 ffplay，**不 fork 上游**。

- `compat/ffplay.c` + `cmdutils.c/h` 与 SDK 上游（FFmpeg4.4.1SDK/source/ffmpeg-4.4/fftools）**diff=0**（2026-08-17 实测）。`ffplay_embed.patch` 重放 == `ffplay_embed.c`。
- **硬纪律（经验 34）**：`ffplay_embed.c` = `ffplay.c` + `ffplay_embed.patch`，**改 embed.c 必须回填 patch**（2026-08-17 已漂移一次，重生成+重放验证）。compat/ffplay.c 与上游 diff=0。
- **不动清单**：`cmdutils.c` / `ffplay.c`（上游代码，patch 纪律不动）；`ffplay_embed.c L1818` 的 `fprintf`（上游残留，仅 show_status==1 触发，嵌入模式不走）。
- **编译**：`FFPLAY_EMBED` + `_GNU_SOURCE`；kylin 需预包含 `/usr/include/time.h`（sys/time.h 不含 struct tm，经验 34）。
- `ffplay_engine.h` 为**配套自研 C API**（非上游 export），语义与 LO avmedia XPlayer 对齐（start/stop/pause/seek/volume/loop），见 §6。

## 3. 构建 / 部署（I 类）

- `FFplay` target → 产物 `ffplay.so`（`PREFIX ""` 去 lib 前缀，unorc 注册）。
- **部署** `office/program/`（与 office_runtime 同处，防 .so 双份致 GetRuntimeDir 错位，经验 33/31）。RUNPATH `$ORIGIN:$ORIGIN/..:$ORIGIN/../..` 自足；soffice 脚本自设的 LD_LIBRARY_PATH 也含 bin 根（双保险）。
- **dlopen in soffice.bin（共享内核进程）**：UNO 符号（libuno_*.so.3）由内核进程已加载库解析 → `--allow-shlib-undefined`；引擎真实调用 ffmpeg/SDL2 → NEEDED 带入 soffice 进程。
- **`-Wl,--no-as-needed` 必须**：SDL2/avfilter/avdevice/postproc 的 NEEDED 须全部保留，缺一个即 soffice dlopen undefined symbol（经验 34）。ffmpeg SDK 为 NovaPlayer 自带（NovaPlayer/include/ffmpeg，与库版本精确匹配 58.76.100/4.4.1）。

## 4. 模块职责（收集自各模块头注释）

| 文件 | 职责 |
|---|---|
| `ffplay_manager.cxx` | XManager + XFastPropertySet（注入 + 静音触发，见 §1/§7） |
| `ffplay_player.cxx/.hxx` | XPlayer 真播放器：嵌入引擎驱动（端口/解码/seek/音量），引擎在 createPlayerWindow（窗口句柄就绪）时创建；createPlayer 仅存 URL |
| `ffplay_window.cxx/.hxx` | XPlayerWindow no-op 壳：渲染由引擎 SDL_CreateWindowFrom(LO 媒体子窗口) 接管，LO 侧仅承接接口调用 |
| `ffplay_log.h` | 日志基础设施（见 §8） |
| `compat/ffplay_engine.h/.c` | 引擎 C API + 补丁式 ffplay（见 §2/§6） |
| `ffplay_player.cxx:createPlayerWindow` | 参数 `[0]=sal_IntPtr 媒体子窗口 X 句柄` / `[1]=awt::Rectangle 位置尺寸` / `[2][3]=LO 内部指针（SDK 模式不解析）`；窗口句柄 = `aArgs[0]`（经验 30/34） |

**生命周期**：createPlayer 仅存 URL → createPlayerWindow（窗口句柄就绪）创建引擎 → 引擎渲染到 LO 媒体子窗口，LO 侧 XPlayerWindow 无绘制逻辑。

## 5. 尺寸链治理（D 类，2026-08-19 已闭环）

> 修复后无回归；详见 git 历史 commit + `ffplay_window_size_probe.cpp`。

**问题**：`ffplay_embed.c video_open()` 用 `default_width/height` (640x480) 设置 `is->width/is->height`，嵌入模式与外部 X11 窗口实际尺寸无关，视频位置/缩放错误。

**尺寸链**（LO → ffplay）：mediawindow_impl.cxx Resize → createPlayerWindow aArgs[0]=parent / aArgs[1]=rect → ffplay_player.cxx 解析（rect 被忽略）→ ffplay_engine_create → SDL_CreateWindowFrom → video_open `is->width=default_width=640` ← **bug**。

**修复（方案A）**：video_open renderer 创建后加 `SDL_GetWindowSize(is->window, &w, &h)` 替代 640x480 硬编码。技术依据：SDL X11 驱动 `X11_CreateWindowFrom → SetupWindowData → XGetWindowAttributes` 设置 window->w/h（SDL2 源码实证）。修改 `ffplay_embed.c` (video_open) + `ffplay_embed.patch` 回填（经验 34）。

**验证**：`ffplay_window_size_probe`（engine 组，三方对比 期望/X11/ffplay，500x300 故意 ≠ 640x480）。修复后 PASS：三方一致。Demo 含视频 pptx 视频尺寸正确。

**开放问题（升级路径）**：方案A 仅首次创建，运行时 X11 resize 不感知（放映期尺寸固定为常见场景）。升级：方案B（`ffplay_engine_set_window_size` API + `PlayerWindowShell::setPosSize`）/ 方案C（SDL_WINDOWEVENT_RESIZED 事件监听）。

## 6. 引擎 C API 与多实例（E 类，2026-08-19 多实例根治落地）

### 6.1 引擎 C API（ffplay_engine.h，配套自研）

- 每实例独立 `VideoState`；渲染目标 = `create` 传入的外部 X 窗口句柄（LO 媒体子窗口）。
- `create` 后即开始解码（read_thread），播放状态为**暂停**（与 ffplay 官方 Create 后语义一致，由 `ffplay_engine_play` 开始）。
- 函数与 LO XPlayer 语义对齐：`create/destroy/play/pause/is_playing/seek/get_media_time/get_duration/set_loop/set_volume(0-100)/get_volume/debug/get_window_size`。
- `destroy` 停止线程 + 清理，不退出进程；`SDL_Quit` 由引擎末实例统一处理。
- **注意**：本 header 的逐函数契约注释**保留在代码**（公共 C API 文档），此处仅记录设计意图。

### 6.2 多实例根治

> ffplay 单实例 CLI 遗产：文件作用域全局 `window`/`renderer`/`audio_dev` 持有"唯一实例"状态。embed 补丁让 `VideoState` per-instance，却未把渲染/音频入口的全局状态实例化 → 多实例 = 多泵线程写同一份全局。经验 37 的 `SDL_FRAMEBUFFER_ACCELERATION=0 + SOFTWARE renderer` 修了 GL/swrast 崩溃，但软件渲染下**残留的全局状态竞态仍在**（侥幸不崩，非构造性安全）。本根治处理之。

**三个 bug（代码层；原 ffplay-multi-instance.md，2026-08-19 已整合本节）**：

| # | bug | 机制 | 位置 |
|---|---|---|---|
| 1 | audio_dev 跨实例覆盖/误关 | 全局 `audio_dev` 被实例2 open 覆盖；实例1 close 经全局指针关掉实例2 的设备 | audio_open / stream_component_close |
| 2 | 渲染全局竞态 | 两泵线程并发 `window=is->window; renderer=is->renderer;` 后 SDL_RenderPresent，交叉使用对方 renderer (UB) | video_display |
| 3 | do_exit 全局销毁误删他实例 | `SDL_DestroyRenderer(renderer)`/`DestroyWindow(window)` 销毁全局=最后渲染实例 | do_exit |

**修复（治病不治症——不加全局锁兜底，而是消灭/收敛全局状态）**：
- **① audio_dev 下沉 VideoState**：`SDL_AudioDeviceID audio_dev` 移入 `VideoState`；audio_open 写 `is->audio_dev`。SDL2 支持多次 open 默认设备并内部混音，多实例音频天然分流。
- **② render_mutex 串行 video_display**：`static SDL_mutex *render_mutex`，随 SDL 首次 init 创建一次（创建串行由 BootLock/500ms 错峰保证，经验 37）。video_display 进出 Lock/Unlock——所有全局 renderer 读位均在 video_display 调用树内，一把锁覆盖整个全局访问面。软件渲染本就单 CPU，串行无并发损失（经验 19b/37）。
- **③ 每实例渲染资源销毁**：stream_close 销毁 `is->window`/`is->renderer`（实例自有）；do_exit 全局 destroy 以 `#ifndef FFPLAY_EMBED` 圈掉（CLI 单实例走全局，embed 走 per-instance）。

**根因证据（修复前段错误回溯）**：两实例并发 crash 栈 `SDL_Blit_ARGB8888_RGB888_Scale → ... → video_display → ffplay_engine_pump_thread`。**关键纠偏**：原独立 .md 探针虚构把 crash 归因"音频设备共享"，实证发现音频路径无 crash（SDL2 支持多设备内部混音），crash 实际在**渲染竞态**；修复方向随之调整。

**探针验证**（ffplay_engine_probe，test_tone.mp4 = 20s h264+aac，DISPLAY=:90）：两实例并发 audio_clock 推进、duration 一致；**销毁隔离 OK**（销毁实例0 后实例1 仍推进/播放）；**控制 OK**（实例1 seek 被接受）；判据 `iso_ok = alive_after && (t_after > t_before + 0.3)`、`ctrl_ok = is_playing && seek_accepted`。

**并发创建竞态（经验 37④，非引擎缺陷）**：微秒级连创时第二实例 read_thread 可能不启动（t=0/duration=0）；错开 500ms 即好。LO 真实路径两 player 创建间隔 = 媒体临时文件拷贝耗时，天然满足。探针模拟该节奏。

**audio_volume 原子化**：移出本根治（int 对齐读写 x86 实践原子，属单实例线程安全细节）。若静音链路引入高频 setMute 调用再评估。

## 7. 静音专项（F 类，方案 A 2026-08-21 定稿）

> PptX 单 slide 多媒体 shape 各起一 XPlayer 实例，LO UNO 无 setMuteAll 接口，需上层（LibreOfficeImpressManager::SetMute override）跨进程触发 soffice 子进程内 ffplay.so 静音。

### 7.1 方案演进

| 方案 | 触发方式 | 状态 |
|---|---|---|
| 方案 A（dlopen 跨组件） | 主进程 dlopen 拿空 g_engines 副本 | 退役（跨进程不可见） |
| 方案 C1 简化版（UNO 全局） | `handle 0 = MUTE_ALL`，遍历全部 g_engines `set_volume(engine,0)` | 2026-08-19 落地 → 2026-08-21 被方案 A 重构取代 |
| **方案 A 重构（per-window）** | `handle 0 = MGR_PROP_MUTE_WINDOWS`，按物理 `parent_window` 句柄过滤 | **当前定稿（2026-08-21）** |

**方案 A 重构核心**（相对 C1 的演进，避免误伤他 session）：
- 归属键 = X11 媒体子窗口 ID（`mediawindow_impl.cxx createPlayerWindow args[0]`），天然全局唯一且与 player 一一对应，不依赖任何调用时序。
- 静音触发链：ImpressSession（主进程）经 XQueryTree 枚举自己放映主窗口下所有子窗口 ID → UNO remote ctx `setFastPropertyValue(MGR_PROP_MUTE_WINDOWS, {window_ids, mute})` → 子进程内 `FfplayPlayer::SetMuteAll(window_ids, mute)` 按 window_id 精确匹配。
- 由于本组件与 g_engines 同处 soffice.bin 子进程，调用天然在子进程内执行，规避方案 A（dlopen）的跨进程失效。

### 7.2 空列表 = no-op（关键语义）

**空 window_ids = no-op（不动任何引擎）**。共享内核模式（Linux）实证（2026-08-21）：切换 NovaPlayer item 时，上层对切出/切入的非 active session 调 SetMute(false)，这些 session 当前可能未创建 media 子窗口（GetMediaWindowIds 返回空）。若空列表走 "all" 分支，会**误伤 active session 的引擎解除静音**。Windows 独立进程模式天然隔离（本进程内不会有他 session 引擎），历史上 "all" 无副作用，但为语义一致性统一改为 no-op；Windows 如需"全静音本进程所有引擎"，应由 `WindowsPlatform::GetMediaWindowIds()` 真实枚举（TODO）。

**注册/注销时机**：createPlayerWindow 成功注册 → `~FfplayPlayer` 先注销再 destroy（防悬挂）；`do_exit` 全局销毁已 `#ifndef FFPLAY_EMBED` 圈掉，不二次释放。

### 7.3 C1 时代技术要点（保留备查，部分仍适用）

- **ctx_ 性质（remote）**：`ImpressSession::ctx_` 是 BootstrapOffice 返回的 remote XComponentContext（runtime.cpp UnoUrlResolver::resolve 后 return remote），非 local。
- **Manager_FFPlay 非 singleton**：ffplay.rdb 无 `<singleton>`，每次 createInstance 都 new 新实例；但 SetMuteAll 是模块级静态方法操作 g_engines 模块级静态表，新实例同样能调到全部引擎。
- **XFastPropertySet 不继承 XPropertySet**：仅需 setFastPropertyValue + getFastPropertyValue 两个虚函数（原首版误继承导致编译错误）。

### 7.4 触发链路（方案 A 重构）

```
UI checkBoxMute → NovaPPTPlayer … → LibreOfficeImpressManager::SetMute (override)
→ ImpressSession::SetMute (impresslink.so, 主进程)
→ XQueryTree 枚举放映主窗口子窗口 ID (window_ids)
→ ctx_->getServiceManager()->createInstanceWithContext("Manager_FFPlay", ctx_)
→ QI XFastPropertySet → setFastPropertyValue(MGR_PROP_MUTE_WINDOWS, {ids, mute})
→ soffice.bin 子进程内 FfplayManager::setFastPropertyValue → FfplayPlayer::SetMuteAll(window_ids, mute)
→ 遍历 g_engines 按 window_id 匹配 → ffplay_engine_set_volume(engine, mute?0:100)
```

### 7.5 端到端回归（已闭环）

PASS：video-loop.pptx + ORT_MEDIA_BACKEND=ffplay，SetMuteAll 命中（UNO marshalling 跨进程生效）。ABI 不变：`ImpressSessionSetMute(void*, int)` 签名不变，libNovaOfficeCore.so 无需重建。

## 8. 日志专项（G 类，2026-08-20 落地）

> ffplay.so（soffice.bin 子进程内被 dlopen）自己初始化一份 spdlog logger，落盘 `~/.office-link/logs/ffplay_<pid>.log`。复用 ORT_LOG/ORT_LOG_LEVEL 控制，与主进程 office_<pid>.log 同目录同格式。

- **目录/文件**：`~/.office-link/logs/`（复用 office_paths::logs_dir()）；`ffplay_<pid>.log`（pid = soffice.bin 子进程 pid；rotating 5MB×3）；格式 `[%Y-%m-%d %H:%M:%S.%e] [%l] %v`。
- **flush_on(info)**：子进程异常退出（SIGKILL/SIGTERM 或 dlclose 未触发析构）默认 buffer 不 flush 丢日志；`flush_on(info)` 保证每条 info 及以上立即落盘 + 运行期 tail -f 实时监控。OfficeLog（runtime.cpp InitOfficeLog）同样加 flush_on。
- **为什么 ffplay.so 不能复用 OfficeLog**：ffplay.so 在 soffice.bin 子进程内 dlopen，office_runtime.so 在主进程内（BootstrapOffice 用 osl_executeProcess 启动子进程），跨进程 dlsym 不可行。
- **av_log 接入**：av_log_set_callback 注册 AvLogToSpdlog，输出 `[FFmpeg/<module>]` 前缀（module = AVClass.class_name，如 avi/h264/mp3；avcl==NULL 时 module="ffmpeg"）。av_log_set_level 同步 ORT_LOG_LEVEL。
- **为什么单文件混合而非按视频拆分**：av_log callback 进程级全局无法区分来源 / g_engines 全局表属全体 / 跨视频时序对照需求；引擎 ID 标记 + grep 即可单视频提取。
- **header-only**：inline + FFLOG 宏，C++17 magic statics 保证 per-process 单实例（多 TU 包含不冲突）。
- **配对方式**：主进程 office log 打印 soffice child started initial_pid=N（osl_executeProcess 启动 soffice 脚本，脚本 exec oosplash → fork soffice.bin 二级 fork，final pid ≠ initial pid）。配对：`ls -t ~/.office-link/logs/ffplay_*.log | head -1`（按 mtime）。
- **不动清单**：`ffplay_embed.c L1818` fprintf 保留（上游残留）；cmdutils.c / ffplay.c 不动（经验 34）。
- **已知小问题**：`[FFmpeg/ffmpeg]` 后偶尔空消息（ffmpeg 退出路径 av_log(NULL, AV_LOG_QUIET, "") 触发，不影响功能）。

## 9. 探针回归（H 类）

| 探针 | 验证 |
|---|---|
| `ffplay_engine_probe` | 引擎推进/pause/seek/双实例 + 销毁隔离（见 §6.2） |
| `ffplay_window_size_probe` | 尺寸链三方对比（见 §5） |
| `ffplay_inject_probe` | 注入 SUCCESS |
| `media_green_probe` | 双态（ffplay 默认 + gstreamer 回退，帧间差异判据） |
| `dual_media.pptx` | 同页双视频并行播放（经验 37） |

> 探针源码在 `tools/linux/`，素材 `tools/data/`；CMake `-DBUILD_TOOLS=ON`，engine 组探针（ffplay_engine/window_size）直接链 compat/ffplay_embed.c（FFPLAY_EMBED）+ cmdutils.c。

---

## 关联索引

**关联经验**：17/18/19 (gst → ffplay 替代链) / 19b (Xvfb 无 GPU) / 21 (GL 转场必崩) / 28 (gst 依赖检测) / 29/30 (ffplay 注入 SDK 模式 + ORT_MEDIA_BACKEND) / 31/33 (构建/部署) / 34 (补丁式复用回填纪律) / 37 (多实例并行播放)
**关联待办**：HANDOFF 3.1 ffplay 能力增强（XFrameGrabber/硬解/媒体信息）/ ffplay 引擎并发创建竞态（紧邻创建需引擎内串行化）
**失效条件**：见 [experiences.md](experiences.md)（经验 29/30/34 失效条件表）
**项目级上下文**：HANDOFF 1.6 媒体后端 / 1.6 GL 全禁用 / 1.6 诊断开关