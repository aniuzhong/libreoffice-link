# NovaLibreOfficePlayer 交接文档

> 新会话起点:先读本文件,再按"三、规划"推进。
> 代码在 `NovaLibreOfficePlayer/`(calc/impress + common/ + office_runtime 含 ffplay)+ 上层 `NovaOfficeCore/`(dlopen links)。
> 经验编号被代码注释引用,**编号只增不改**;每次认知提升更新"二、经验"(带时间+置信度),完成事项移入"一、现状"。

---

## 一、项目现状

### 1.1 架构

```
NovaLibreOfficePlayer/    (NovaPlayerTools/cmake 单一树子项目; target: NovaLibreOffice-
  │                        PlayerDeprecated(旧独立进程方案,待废弃)/OfficeRuntime/
  │                        CalcLink/ImpressLink/FFplay)
  ├── common/              基础层 (零依赖 office_runtime)
  │     link_platform.h   LinkPlatform 统一平台接口 (工厂: CreateCalc/ImpressPlatform)
  │     link_utils.h/.cpp u2s/s2u + HideUiBlock UI 隐藏三件套 + DumpUiState 自省
  │     log.h             OfficeLog 声明 (实现唯一在 office_runtime.so, 勿编第二份)
  │     cmake/FindLibreOfficeSDK.cmake  SDK 查找 (缓存自愈)
  │     linux/xvfb_platform.*  XvfbSessionPlatform 单类参数化 (抓帧/slot/落位)
  │     linux/linux_platforms.cpp  工厂 (匹配规则即文档类型差异, 各 2 行)
  │     windows/win_platform.*  WindowsPlatform (CreateDesktop 独立进程模式; impress stub)
  ├── office_runtime/      OfficeRuntime → office_runtime.so — 进程级共享运行时 (Linux)
  │     Xvfb 大屏/LO 共享内核/slot shm/跨进程 BootLock/孤儿清场/BootLock/诊断
  │     office_runtime_test.cpp — 单测 (9 场景 49 检查, --stress N)
  │     ffplay/            FFplay → ffplay.so — 自治媒体后端 (Manager_FFPlay;
  │                         嵌入引擎 = 定制 ffplay.c 补丁式复用, compat/)
  ├── calc/                CalcLink → calclink.so (C ABI)
  ├── impress/             ImpressLink → impresslink.so (C ABI 与 calc 同构)
  └── writer/              未创建 (规划: writerlink.so 同构)
```

- **共享内核模式 (Linux)**:进程内一个 LO 内核(自研 `BootstrapOffice` 引导,复制官方 cppu::bootstrap 逻辑,独立 profile `~/.office-link/player`)+ 一个 Xvfb 大屏(默认 `8×3840×2160 = 30720x2160`,8 个 2160p 子屏位),多文档窗口动态落位互不重叠。调用者只需知道最大并发数 + 每文档最大分辨率。Windows 为每 session 独立 soffice + 独立桌面,不参与本模块。
- **上层**:`NovaOfficeCore/ppt/LibreOfficeImpressManager`(dlopen impresslink)、`excel/LibreOfficeCalcManager`(dlopen calclink);分发点 `PptCoreExport.cpp` 的 `PPT_PLAY_MODE_ANIMATION_LIBREOFFICE`。

### 1.2 已验证能力

- 单测 9 场景 49 检查(bootlock/slots/crossproc/acquire/adopt/dirtyenv/faultinj/linksmoke/gstcheck);加固后连续多轮全绿(经验 35)
- 探针回归(登记 6 个,`build_probes.sh`):impress_nextpage(翻页 20 页)/impress_multi(2 xlsx + pptx 并发,slot 0/1/2 无死锁)/media_green 双态(ffplay 默认 + gstreamer 回退,媒体页帧间差异判据)/ffplay_inject(注入 SUCCESS)/ffplay_engine(引擎推进/pause/seek/双实例)/xvfb_stress(尺寸上限/抓帧性能)
- **同页双视频并行播放**(dual_media.pptx 实证,经验 37);Demo 实测三画面/翻页正常;媒体页真实视频+音频
- 抓帧性能:XShm 1080p ~1ms/1440p ~2.5ms/2160p ~5.9ms;Xvfb 30720x2160 RSS ~300MB
- LO 源码两处改动已固化远端:commit `83e0b9c3e`(gstplayer.cxx + mediawindow_impl.cxx),master == origin
- compat/ffplay.c、cmdutils.c/h 与 SDK 上游(FFmpeg4.4.1SDK/source/ffmpeg-4.4/fftools)**diff=0**(2026-08-17 实测);ffplay_embed.patch 重放 == ffplay_embed.c(改 embed.c 必须回填 patch)

### 1.3 构建/部署/验证

```bash
# 统一构建 (唯一入口, 单一树 debug/)
cd NovaPlayerTools && ./build_in_linux.sh
# 定向: cmake --build debug --target OfficeRuntime CalcLink ImpressLink FFplay office_runtime_test -j$(nproc)
# 产物直出部署目录 NovaPlayer/bin_<arch>_<sys>/office/program (单副本)。
# 注意: 部署目录在 NovaPlayer/ 下 (不是 NovaPlayerTools/, 后者同名目录为空)。

# office_runtime 单测 (前提: 无其他 office_runtime 使用者; 跑前清场)
rm -f /dev/shm/nova_office_slots_v1 /dev/shm/sem.nova_office_boot /tmp/.X9*-lock
NovaPlayer/bin_x86_64_kylin/office_runtime_test [--stress N]

# 探针编译 (仓库根 xvfb_calc_demo/, 项目外工具; 登记表见脚本注释)
cd xvfb_calc_demo && ./build_probes.sh [probe_name ...]

# 探针/单测可免 LD_LIBRARY_PATH 直接跑 (烧入绝对 RUNPATH, 经验 36);
# 手动设置时必须绝对路径。跑前清场: kill Xvfb + rm lock + rm /tmp/.X11-unix/X9*

# 并发回归 (2 xlsx + pptx)
xvfb_calc_demo/impress_multi_probe "志愿分析.xlsx" "7-8月报销明细表-正式版.xlsx" "AI时代.pptx"
# 媒体回归 (双态)
xvfb_calc_demo/media_green_probe "AI时代.pptx"                      # 默认 ffplay
ORT_MEDIA_BACKEND=gstreamer xvfb_calc_demo/media_green_probe "..."  # 回退 gst
```

### 1.4 关键文件

- `office_runtime/office_runtime.cpp` — 全部运行时逻辑(Xvfb 扫号 90-99/adopt/残留清理、BootstrapOffice、slot shm、BootLock、CleanupOrphanSoffice、窗口诊断、CheckGstDeps)
- `office_runtime/ffplay/compat/` — `ffplay.c`(上游 diff=0)+ `ffplay_embed.c`(= ffplay.c + `ffplay_embed.patch`)+ `ffplay_engine.h` 引擎 C API + 手写 `config.h`
- `common/linux/xvfb_platform.cpp` — XShm 抓帧 + BGRX 字节序直拷 + 窗口扫描/落位
- `calc|impress/*_session.cpp` — 会话(加载/控制/轮询;calc 滚动/切表/缩放,impress XPresentation2 窗口化放映 + gotoNextEffect 翻页)
- `xvfb_calc_demo/build_probes.sh` — 探针登记表(过时探针不登记,旧二进制可手动跑)

### 1.5 当前状态与注意事项

- **媒体后端**:默认 ffplay(`ORT_MEDIA_BACKEND`,EnsureKernel setenv 不覆盖宿主);gstreamer 为验证过的回退路径(ximagesink 补丁版 libavmediagst.so 保留;.bak 为补丁前备份)
- **GL 全禁用**:SAL_DISABLEGL=1(转场,经验 21)+ ffplay 的 SDL_FRAMEBUFFER_ACCELERATION=0 + SOFTWARE renderer(经验 37)——Xvfb 恒无 GPU,一切渲染固定软件路径
- 日志:统一 `[前缀]` + 时间戳,`~/.office-link/logs/office_<pid>.log`(stderr 副本有缓冲差异,排查以文件为准);ORT_LOG=file|stderr|both|off
- **所有者退出连坐**:共享内核/屏的所有者进程退出,其他进程会话断开;双实例部署需同时使用
- Windows:impress stub + win_platform 未编译验证,需 Windows 侧确认
- UNO_PATH/URE_BOOTSTRAP 依赖部署位置(office/program),部署路径变化需同步(经验 23/33)
- 已知待清理:① calc_session 的 per-instance profile seed 在 **Linux 下是死开销**(每次 Create 复制整个 office/user,但 Linux 引导不用它;仅 Windows bootstrap 消费,2026-08-17 注释审查发现);② `[CALC-T]/[IMP-T]` 等 fprintf 诊断日志(定位已完成,建议清理;`[SCROLL]/[UIHIDE]` 有线上价值保留)
- 诊断开关:ORT_DUMP_WINDOWS=1(窗口树/重叠/边缘像素);ffplay video_open 打印 renderer 后端

---

## 二、历史经验(勿回退;编号被代码注释引用)

> 时间=提出/验证时间;置信度:高=源码级或多次实测,中=单次实测,低=推断。

### 2.1 运行时架构与生命周期

| # | 经验 | 时间 | 置信度 |
|---|---|---|---|
| 1 | **Xvfb 窗口存储缺陷**:XGetImage(被遮挡窗口)返回背景;XShm 遮挡同样、raise 不可靠。解法=大屏+slot 分区,窗口天然不重叠 | 08-12 前 | 高 |
| 2 | **OfficeRuntime 必须是 so 单例**(Instance() 函数静态),跨 links 共享 | 08-12 前 | 高 |
| 3 | **PDEATHSIG 多线程误杀 Xvfb**:改 std::atexit;崩溃遗留靠下次启动清理 | 08-12 前 | 高 |
| 4 | **进程退出不能析构 LO 内核**(静态析构期 UNO 调用段错误):Instance 故意泄漏 | 08-12 前 | 高 |
| 7 | **Hidden 加载**:slot 方案下 Hidden 加载正常无闪烁;可见加载方案已废弃 | 08-12 前 | 高 |
| 9 | **退出时 X IO Error 噪音无影响**(atexit 杀 Xvfb 时 LO 打印 Fatal 后退出) | 08-12 前 | 高 |
| 19c | 会话重建不做:确定性故障重建仍崩;改为崩溃检测+明确告警 | 08-12 | 高 |

### 2.2 跨进程协调(共享屏/内核/slot)

| # | 经验 | 时间 | 置信度 |
|---|---|---|---|
| 5 | **并发 Create 必须串行化引导+加载**:BootLock = so 内 mutex + 跨进程命名信号量(/nova_office_boot,60s 强制恢复)。私有锁无法跨 link/跨进程(踩过卡死) | 08-12 前 | 高 |
| 6 | **Xvfb 锁检查必须查僵尸**(kill(pid,0) 对僵尸仍成功,须查 /proc state;踩过:僵尸被当活服务器→adopt 失败→显示号漂移) | 08-12 | 高 |
| 10 | **共享虚拟屏是机器级的**:发现活 Xvfb 直接 adopt,否则渲染在别人屏上抓帧全空(踩过双实例全黑) | 08-12 | 高 |
| 11 | **slot 位图必须跨进程共享**(POSIX shm + flock + owner PID + 崩溃回收),私有位图会重复分配(踩过) | 08-12 | 高 |
| 12 | **启动清场** CleanupOrphanSoffice:cmdline 匹配 + (ppid==1 或 90-99 号段 Xvfb 已死);号段判定防误杀 :0 转换进程(踩过转换被杀) | 08-12 | 高 |
| 22 | **同 UserInstallation 必复用(转发)**:LO 按 pipe `SingleOffice_<md5>` 转发命令行,--headless/--nolockcheck 都拦不住。推论:外部 soffice 调用必须显式独立 profile,否则污染共享内核 | 08-13 | 高 |
| 23 | **自研 BootstrapOffice**(复制 cppu::bootstrap,零 LO 源码改动,-env:UserInstallation 支持独立内核)。四个坑:① 先 set URE_BOOTSTRAP(缺→binaryurp 段错误)② 客户端进程需 UNO_PATH ③ 连接串 StarOffice.ComponentContext + UNO_QUERY_THROW ④ osl_executeProcess 原样。引导 ~504ms(旧 ~1004ms) | 08-13 | 高 |
| 24 | **env 经 osl_executeProcess 快照继承**:SAL_*/GST_*/ORT_* 等运行时 getenv 的变量只需引导前 setenv,**不必写 run.sh**;例外(启动期决定):LD_LIBRARY_PATH/libstdc++/LD_PRELOAD | 08-13 | 高 |
| 25 | **libstdc++ SONAME 单例**:宿主预加载系统 6.0.28 后 dlopen calclink 缺 GLIBCXX 符号失败。已落地:links 加 `-Wl,-rpath,<deploy>` + 显式链部署目录 libstdc++ 6.0.30(主程序 RUNPATH 参与间接依赖解析)。run.sh 的 $CURDIR(ffmpeg 库)仍必需;soffice 侧靠脚本自设。**陷阱**:env -i 缺 LANG 时 LO 报 type detection failed,勿误判为库问题 | 08-13 | 高 |
| 31 | **统一构建树**(2026-08-13):NovaLibreOfficePlayer 并入 NovaPlayerTools cmake 单一树,build_links_linux.sh 退役,ABI 同步由依赖图承接(links 链 OfficeRuntime target)。坑:① 伞 target 改名后 -Bsymbolic 需手动应用 ③ ld 对直接 .so 输入按 basename 记 DT_NEEDED ⑤ ffplay 链 ffmpeg 需 --no-as-needed ⑥ 探针不参与统一树(build_probes.sh,项目根 xvfb_calc_demo/) | 08-13 | 高 |
| 32 | **平台层归组重构**(2026-08-14,消 77% 重复):按环境归组 common/{linux,windows},规则参数化(窗口匹配规则=文档类型差异,工厂各 2 行);会话层不强提基类;common STATIC 链入各 link,log.h 实现保持 office_runtime 单例(双份=spdlog 双写) | 08-14 | 高 |
| 33 | **.so 路径错位 + CMake 缓存自愈**:① 双份 .so 时 dladdr(GetRuntimeDir)错位→UNO_PATH 错→rc=4;修复=四件套 per-target 输出部署目录单副本 ② FindLibreOfficeSDK 模块移动后旧缓存 FATAL;修复=NOT EXISTS 时 FORCE 重推导 | 08-14 | 高 |
| 35 | **Xvfb 垂死窗口竞态家族(2026-08-17)**:① 大屏 Xvfb(~300MB)SIGKILL 后垂死窗口内 socket 仍监听,同号立即重启必 "server already running" 失败;残留 socket 文件(无活 server)无害。杀后必须等死透(ProcAlive 轮询;**waitpid 对非子进程 ECHILD 无效**)。测试/探针侧 CleanXvfbBattlefield/SafeKill/StopXvfb 均已"杀→等死透→清 lock+socket";office_runtime 侧 StopXvfb 清 lock+socket、StartXvfb fork 前清残留+lost-race 清残局(扫号重试本身已是正确自愈) ② **kill(0)/kill(-1) 灭组**:pid 来自 lock/管道读回,竞态下可能 0/-1(kill(0)=杀进程组,实测测试+tail 全家死,无 core 无日志);SafeKill 统一 guard(pid>0 && ≠self)+ SpawnOrphan 读回校验 + 断言不假设 :90(display 号感知) | 08-17 | 高 |

### 2.3 抓帧与性能

| # | 经验 | 时间 | 置信度 |
|---|---|---|---|
| 13 | **XShm + BGRX 字节序直拷**:Xvfb TrueColor24 视觉=32bpp LSBFirst BGRX → XShmGetImage(0.01ms)+memcpy+alpha(1080p ~1ms vs XGetImage 转换 ~9ms)。非 BGRX 自动回退。links 必须链 X11::Xext | 08-12 | 高 |
| 14 | **屏高 ≥ 最大文档分辨率**(2160p 窗口在 1080 屏 BadMatch);StartXvfb 按 max_doc_height 定高 | 08-12 | 高 |
| 15 | 屏尺寸 16 位坐标上限 32767 内无阻碍;30720x2160 RSS ~300MB 可起 | 08-12 | 高 |

### 2.4 媒体播放

| # | 经验 | 时间 | 置信度 |
|---|---|---|---|
| 17 | **Xvfb 无 GPU 的 GST 崩溃链**:非 GTK 平台 gst 回退 autovideosink→glimagesink→EGL/swrast 段错误→soffice.bin 崩→bridge disposed 全会话毁;媒体渲染失败还会阻塞 slideshow 事件链 | 08-12 | 高 |
| 18 | **gst 修复(LO 源码)**:gstplayer.cxx 回退分支优先 ximagesink。生效组件=**libavmediagst.so**(非 libavmedialo.so);已入远端 commit 83e0b9c3e | 08-12 | 高 |
| 19 | **弯路勿重走**:① gst 环境变量禁 GL 在 1.16.3 无效(rank 不变)② 卸载 gl 包不解决挂起(挂起=渲染失败非 GL)③ LD_PRELOAD 拦截不可靠 ④ VirtualGL/离屏 GL 均不可行 ⑤ ffplay 软解可行(定论基础) | 08-12 | 高 |
| 19b | **Xvfb 恒无 GPU,媒体渲染固定软件路径,不做动态决策**(与经验 21/37 同哲学) | 08-12 | 高 |
| 19d-g | **注入探索历程(已被 29/30 取代)**:UNO_SERVICES 同名替换机制源码级确认但未采用;unorc 注入路径 VCL abort 已规避(SDK 模式+独立 service 名+环境变量开关为最终方案);ffplay 编译参数(bear 提取 gstplayer 编译命令)与坑(WeakImplHelper1/component_getImplementationEnvironment/rdb environment="gcc3")沉淀于 19g | 08-12 | 中 |
| 20 | **幻灯片属性对齐旧方案**:IsEndless=true 循环保活等,与 source/Communicator.cpp 一致 | 08-12 | 高 |
| 21 | **GL 转场必崩(独立于媒体)**:X11 generic 平台无条件 supportsOpenGL=true→ogltrans→EGL/swrast 崩。修复=SAL_DISABLEGL=1(EnsureKernel setenv,勿回退;转场退化为 CPU 渲染效果保留) | 08-13 | 高 |
| 26 | **窗口黑边/瞬态/串流三层**:① user 配置窗口状态→全屏瞬态根源(结构修复:窗口属性配置 1920x1080 同 Calc)② 旧 user UI 残留 23px 黑边→UNO 动态隐藏(setMenuBar(null)+hideElement,不依赖 user 配置)③ LO 窗口固有 3px 边框不可控(接受)。UNO setPosSize/visible-toggle 在 slideshow 运行中会黑屏,勿用。诊断:DumpWindowEdges/CheckWindowOverlap(ORT_DUMP_WINDOWS=1) | 08-13 | 高 |
| 27 | **播放内核独立 profile ~/.office-link/player**:与任何默认 profile 的 soffice 彻底隔离(经验 22);部署 office/user 不再被写 | 08-13 | 高 |
| 28 | **gst 依赖检测 CheckGstDeps**(参数化可测,EnsureKernel 引导时检测):仅 gstreamer 回退路径需要。**实测定性**:无 gst 库时 LO 优雅降级(不卡死),gst 存在+sink 渲染失败才卡死(18 已修);ffplay 价值=无 gst 时提供播放能力+音频,非防卡死。媒体必需最小集(回退用):libgstreamer1.0-0 + plugins base/good/bad/ugly + x;gl 移除更安全 | 08-13 | 高 |
| 29 | **ffplay 正规注入(SDK 模式)**:独立 service 名 Manager_FFPlay;SDK 模式坑:WeakImplHelper1(非 WeakImplHelper)、component_getImplementationEnvironment 必须导出、rdb environment="gcc3" | 08-13 | 高 |
| 30 | **媒体后端开关(方案 A)**:LO mediawindow_impl.cxx 读 ORT_MEDIA_BACKEND(已入远端 83e0b9c3e);EnsureKernel 默认 ffplay(不覆盖宿主;export gstreamer 回退)。createPlayerWindow 参数:[0]=sal_IntPtr 窗口句柄 [1]=awt::Rectangle,SDK 可解析 | 08-13 | 高 |
| 34 | **ffplay 嵌入引擎(补丁式复用)**:compat/ffplay.c 与 SDK 上游 diff=0(2026-08-17 复验);关键=用 Nova 定制 ffplay.c(官方 release/4.4 与定制库不兼容会 find_stream_info 卡死)。补丁点:VideoState per-instance 窗口/渲染器/泵线程、video_display 开头同步全局句柄(**勿宏映射 window→is->window**泵线程复刻轮询语义、引擎 C API。编译:SDL2/avfilter/avdevice/postproc 全链 --no-as-needed;kylin 需 -include /usr/include/time.h。**改 embed.c 必须回填 ffplay_embed.patch**(2026-08-17 已漂移一次,重生成+重放验证)。性能:媒体播放时 calc 抓帧 5→10-16ms(可接受) | 08-14 | 高 |
| 37 | **duration 语义 + 多实例并行播放已打通(2026-08-17)**:① duration 接口非桩,create 后立即查询=0 是流探测未完成(1s 后正常,实测 39.63s);② get_media_time 对未初始化时钟归零(原 nan)③ **双实例崩溃根因(源码级)**:SDL 软件渲染器 present 链(SW_RenderPresent→UpdateWindowSurface)默认走 texture framebuffer,SDL 内部**显式只找 ACCELERATED 渲染器**(不读 RENDER_DRIVER hint)→mesa 软件 GLX→swrast;**单实例单线程 JIT 侥幸存活(此前一直慢跑 swrast),双实例并发 LLVM JIT abort**。修复=SDL_HINT_FRAMEBUFFER_ACCELERATION="0"(走 X11 原生 XShm,GL 完全不加载)+显式 SOFTWARE renderer;单实例也受益 ④ 引擎微秒级并发创建竞态(第二实例 read_thread 可能不启动),错开 500ms 即好;LO 路径创建间隔=媒体临时文件拷贝耗时,天然满足 ⑤ dual_media.pptx(python-pptx 构造)双视频页全绿 | 08-17 | 高 |

### 2.5 构建与部署

| # | 经验 | 时间 | 置信度 |
|---|---|---|---|
| 8 | **run.sh 依赖**:$CURDIR(ffmpeg 库)+ $CURDIR/office/program(UNO 库)必需;$CURDIR 展开为绝对路径 | 08-12 前 | 高 |
| 16 | **ABI 同步已由依赖图承接**(office_runtime.h 变更重编三件套);历史教训:只重编 office_runtime 导致 undefined symbol | 08-12 | 高 |
| — | **组件名陷阱**:gstreamer 生效组件是 libavmediagst.so(libavmedialo.so 是另一组件) | 08-12 | 高 |
| 36 | **相对 LD_LIBRARY_PATH 陷阱(2026-08-17)**:手动跑探针/单测用相对路径→dladdr→UNO_PATH 相对化→BootstrapOffice DeploymentException SIGABRT。产品不受影响(run.sh $CURDIR 绝对)。**探针/单测烧入绝对 RUNPATH,免设直接跑(推荐)**;手动设置必须绝对路径。部署目录=NovaPlayer/bin_<arch>_<sys>(非 NovaPlayerTools/) | 08-17 | 高 |

---

### 2.6 文档渲染(writer 规划)

| # | 经验 | 时间 | 置信度 |
|---|---|---|---|
| 38 | **writer 渲染方案可行性(2026-08-17 探针实测)**:**方案 A(自治 PDF, 采用)**: docx → PDF → PDF 导入 Draw → 逐页 XSlideRenderer::createPreview → XBitmap::getDIB → BGRA。全链路 UNO 公开接口, LO 自治零第三方(mupdf/poppler 均不需要); 不需要 Xvfb/窗口/抓帧(纯离屏渲染) —— 契合"不用截屏和虚拟屏"与"内核稳定优先"。**接口细节(落地直接复用)**: ① 转 PDF 用 `XStorable::storeToURL(url, {FilterName="writer_pdf_Export"})`(**XModel 无 storeToURL**; 同内核内转换, 不需要外部 --convert-to 进程, 独立 profile 隔离仍适用); ② PDF 导入 `loadComponentFromURL(pdf, FilterName="draw_pdf_Import")`(Hidden); ③ `XSlideRenderer` 服务名 `com.sun.star.drawing.SlideRenderer`(实现 com.sun.star.comp.Draw.SlideRenderer, sd/source/ui/presenter/SlideRenderer.cxx), `createPreview(XDrawPage, awt::Size(宽,高), superSample)` → `awt::XBitmap` —— 输出尺寸按页面比例适配(竖版 A4 @1080 高 → 763x1080, 完整页面); ④ `XBitmap::getDIB()` 返回 **BMP 文件格式**(非裸 DIB!): 'BM'(0-1) + 像素偏移(10-13=0x36=54) + BITMAPINFOHEADER(biWidth@18, biHeight@22, biBitCount@28=24bpp) + 行对齐 4 字节 —— 解析陷阱, 按 offset 10 的像素偏移取值, 勿假设 40 字节头。**性能实测**(pdf_render_probe, build_probes.sh 已登记): 戴奥良-简历.docx(1页): 转换 102-113ms + 导入 217-264ms + 首渲染 81ms(总 ~0.5s); NovaPlayer概要设计说明书.doc(90页): 转换 ~2.9s + 导入 ~6.1s + 逐页渲染 19-72ms/页(总首开 ~9s, 一次性); 翻页 20-70ms/页(翻页语义足够)。**方案 B(直接渲染 XRenderable)排除**: Writer 文档 `XRenderable::getRendererCount=0` —— XRenderable 是导出器基础设施(PDF 导出内部用, filter/source/pdf/pdfexport.cxx), UNO 公开层 render 的 xOptions 是导出选项, 无位图输出路径。**探针坑**: 中文路径必须 `OStringToOUString(UTF8)`(createFromAscii 损坏→mojibake→type detection failed); LO type detection 依赖 LANG(经验 25 陷阱, 探针 setenv 兜底)。**writer link 设计**: C ABI 同构 calc/impress, 复用 office_runtime 内核/BootLock; **无平台层**(不需要 LinkPlatform); 页表 = Draw 文档 XDrawPages, 翻页 = createPreview 当前页; 大文档首开 9s 的优化方向: 转换缓存/后台预转 | 08-17 | 高(实测) |

## 三、项目规划

> ★★★=立即;★★=中期;★=远期。

### 3.1 待办/讨论

| 排序 | 事项 | 说明 |
|---|---|---|
| ★★★ | **writer 模块**:照 impress 模式(writerlink.so;XPageCursor 页导航,直接渲染不转 PDF) | 核心交付 |
| ★★ | calc_session Linux 下 profile seed 死开销(每次 Create 复制整个 office/user 但 Linux 不消费;Windows bootstrap 才用)| 注释审查发现,讨论后改 |
| ★★ | 诊断日志清理:[CALC-T]/[IMP-T] 等 fprintf(定位已完成);[SCROLL]/[UIHIDE] 保留 | 收尾 |
| ★★ | ffplay 能力增强(按需):XFrameGrabber 帧抓取/硬解/媒体信息 | 引擎底座就绪 |
| ★ | ffplay 引擎并发创建竞态(错开即好,LO 天然满足;紧邻创建场景需引擎内串行化) | 按需 |

### 3.2 功能/平台

| 排序 | 事项 | 说明 |
|---|---|---|
| ★★ | 2160p 混合分辨率落位产品化验证(默认配置已支持) | 配置验证 |
| ★★ | slot 管理策略(超限语义/动态轮替/最大并发数) | 策略决策 |
| ★★ | 废弃 source/ 旧方案(target 已标 Deprecated) | 收尾 |
| ★ | Windows impress 平台补全(当前 stub)+ Windows 编译验证 | 平台补全 |
| ★ | 环境自检(字体/音频缺失明确报错)与崩溃检测告警 | 部署稳健性 |

---

*已关闭:gstreamer 路径清理(08-14);LO 改动同步远端(08-17,commit 83e0b9c3e);ffplay 多实例并行播放(08-17,经验 37);office_runtime 防御增强与单测加固(08-17,经验 35)。*
