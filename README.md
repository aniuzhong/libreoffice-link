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
  │                        CalcLink/ImpressLink/FFplay/WriterLink)
  ├── common/              基础层 (零依赖 office_runtime)
  │     link_platform.h   LinkPlatform 统一平台接口 (工厂: CreateCalc/ImpressPlatform)
  │     link_utils.h/.cpp u2s/s2u/u2w(Windows UTF-8→UTF-16)+ kFrameFormatBGRA/
  │                        kDefaultWidth/Height 常量 + UNO_GUARD/UNO_SILENT 宏 +
  │                        HideUiBlock UI 隐藏三件套 + DumpUiState 自省
  │     log.h             OfficeLog 声明 (实现唯一在 office_runtime.so, 勿编第二份)
  │     cmake/FindLibreOfficeSDK.cmake  SDK 查找 (缓存自愈)
  │     linux/xvfb_platform.*  XvfbSessionPlatform 单类参数化 (抓帧/slot/落位)
  │     linux/linux_platforms.cpp  工厂 (匹配规则即文档类型差异, 各 2 行)
  │     windows/win_platform.*  WindowsPlatform (CreateDesktop 独立进程模式; impress stub)
  ├── office_runtime/      OfficeRuntime → office_runtime.so — 进程级共享运行时 (Linux)
  │     Xvfb 大屏/LO 共享内核/slot shm/跨进程 BootLock/孤儿清场/BootLock/诊断
  │     office_runtime_test.cpp — 单测 (9 场景 50 检查, --stress N)
  │     ffplay/            FFplay → ffplay.so — 自治媒体后端 (Manager_FFPlay;
  │                         嵌入引擎 = 定制 ffplay.c 补丁式复用, compat/)
  ├── calc/                CalcLink → calclink.so (C ABI)
  ├── impress/             ImpressLink → impresslink.so (C ABI 与 calc 同构)
  └── writer/              WriterLink → writerlink.so (自治 PDF 位图管线, 经验 38: 无平台层, 页表 = Draw XDrawPages)
```

- **共享内核模式 (Linux)**:进程内一个 LO 内核(自研 `BootstrapOffice` 引导,复制官方 cppu::bootstrap 逻辑,独立 profile `~/.office-link/xvfb`,2026-08-18 由 `player/` 更名,见经验 40)+ 一个 Xvfb 大屏(默认 `8×3840×2160 = 30720x2160`,8 个 2160p 子屏位),多文档窗口动态落位互不重叠。调用者只需知道最大并发数 + 每文档最大分辨率。Windows 为每 session 独立 soffice + 独立桌面,不参与本模块。
- **上层**:`NovaOfficeCore/ppt/LibreOfficeImpressManager`(dlopen impresslink)、`excel/LibreOfficeCalcManager`(dlopen calclink);分发点 `PptCoreExport.cpp` 的 `PPT_PLAY_MODE_ANIMATION_LIBREOFFICE`。

### 1.2 已验证能力

- 单测 9 场景 50 检查(bootlock/slots/crossproc/acquire/adopt/dirtyenv/faultinj/linksmoke/gstcheck);加固后连续多轮全绿(经验 35)
- 探针回归(登记 9 个,`build_probes.sh`):impress_nextpage/impress_multi(2 xlsx + pptx 并发,slot 0/1/2 无死锁)/media_green 双态(ffplay 默认 + gstreamer 回退,帧间差异判据)/ffplay_inject(注入 SUCCESS)/ffplay_engine(引擎推进/pause/seek/双实例)/xvfb_stress(尺寸上限)/pdf_render(writer 两方案可行性)/writer(翻页/缓存/Prev)/word_core(NovaOfficeCore 分发)
- **同页双视频并行播放**(dual_media.pptx 实证,经验 37);Demo 实测三画面/翻页正常;媒体页真实视频+音频
- **writer 自治 PDF 播放全链路已闭环**(writerlink,经验 38):底层(翻页 20-53ms/页、LRU、缓存命中 0ms、Prev 验证)→ NovaOfficeCore(LibreOfficeWriterManager, mode=3 正式分发)→ NovaPlayer 核心(WordInstance 映射)→ demo(Word 模式下拉框 图片/LibreOffice,StepBack 修复);demo 实测 90 页文档翻页/上一页正常,纹理 763x1080
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

### 1.4 关键路径(随设计演进更新)

```
~/.office-link/                  项目用户级数据根
  ├─ xvfb/                       共享内核工作 profile (2026-08-18 由 player/ 更名; 引导时从
  │                                部署 templates/user fresh copy 初始化, 见经验 40)
  ├─ desktops/<link>/<guid>/     Windows 每 session 独立桌面工作 profile (同模板初始化)
  ├─ logs/                       OfficeLog 日志 office_<pid>.log (5MB×3 轮转)
  └─ writer_cache/               writer PDF 内容缓存 (键=源文件 MD5, 经验 38; 已落地与
                                   /tmp/NPOfficeCache 协同复用: 命中即拷贝, 未命中自转)
/tmp/NPOfficeCache/              Nova 现有转换缓存 (缩略图链 GlobalDataSet::DoConvertDocumentW
                                   写入: <md5>.pdf 全量 + <md5>_N.pdf 页版(Windows PageRange;
                                   Linux 分支无滤镜实际全量); 键=源文件 MD5; /tmp 易失重启清空;
                                   convertuser/<md5>/ 为其转换用独立 profile(经验 22), 用后清)
/tmp/.X<n>-lock + /tmp/.X11-unix/X<n>   Xvfb 显示号 90-99 (共享运行时号段)
/dev/shm/nova_office_slots_v1    跨进程 slot 位图 (flock+owner PID)
/dev/shm/sem.nova_office_boot    跨进程引导信号量
部署 office/program/templates/user/   user 模板 (仓库 NovaLibreOfficePlayer/templates/user
                                   净化 xcu, git 管理, 双平台共享唯一初始配置来源; office/user 退役)
```

### 1.5 关键文件

- `office_runtime/office_runtime.cpp` — 全部运行时逻辑(Xvfb 扫号 90-99/adopt/残留清理、BootstrapOffice、slot shm、BootLock、CleanupOrphanSoffice、窗口诊断、CheckGstDeps)
- `include/scope_guard.hpp` — 第三方库(Neargye/scope_guard 0.9.4,MIT),提供 `DEFER` 宏用于 C 资源清理(XCloseDisplay/munmap/close);office_runtime.cpp 使用
- `office_runtime/ffplay/compat/` — `ffplay.c`(上游 diff=0)+ `ffplay_embed.c`(= ffplay.c + `ffplay_embed.patch`)+ `ffplay_engine.h` 引擎 C API + 手写 `config.h`
- `common/linux/xvfb_platform.cpp` — XShm 抓帧 + BGRX 字节序直拷 + 窗口扫描/落位
- `calc|impress/*_session.cpp` — 会话(加载/控制/轮询;calc 滚动/切表/缩放,impress XPresentation2 窗口化放映 + gotoNextEffect 翻页)
- `xvfb_calc_demo/build_probes.sh` — 探针登记表(过时探针不登记,旧二进制可手动跑)

### 1.6 当前状态与注意事项

- **媒体后端**:默认 ffplay(`ORT_MEDIA_BACKEND`,EnsureKernel setenv 不覆盖宿主);gstreamer 为验证过的回退路径(ximagesink 补丁版 libavmediagst.so 保留;.bak 为补丁前备份)
- **GL 全禁用**:SAL_DISABLEGL=1(转场,经验 21)+ ffplay 的 SDL_FRAMEBUFFER_ACCELERATION=0 + SOFTWARE renderer(经验 37)——Xvfb 恒无 GPU,一切渲染固定软件路径
- **日志体系(2026-08-18 收尾定稿)**:统一入口 `OfficeLog/Dbg/Warn/Err`(varargs,LogMsg 等历史包装已删);前缀 = target 名 `[OfficeRuntime]/[CalcLink]/[ImpressLink]/[WriterLink]/[Common]`(子场景点分如 `[CalcLink.Scroll]`);ffplay 组件在 soffice 进程内(office_runtime.so 不在),保留独立 fprintf + `[FFPLAY]`。级别:info=生命周期主线 / debug=诊断细节(窗口扫描/UI 自省/渲染计时) / warn=防御拦截与回退 / error=失败;文件格式 `[时间] [level] [前缀] 消息`,双平台一致(win_office_log 对偶)。开关:ORT_LOG=both(默认)|file|stderr|off(**off 真 silent**——仅跳过初始化时 spdlog 默认 logger 仍打 stdout,已修)、ORT_LOG_LEVEL=debug|info(默认)|warn|error。落位 `office_paths::logs_dir()/office_<pid>.log`(Linux spdlog 5MB×3 轮转;stderr 副本有缓冲差异,排查以文件为准)
- **所有者退出连坐**:共享内核/屏的所有者进程退出,其他进程会话断开;双实例部署需同时使用
- Windows:impress stub + win_platform 未编译验证,需 Windows 侧确认
- UNO_PATH/URE_BOOTSTRAP 依赖部署位置(office/program),部署路径变化需同步(经验 23/33)
- **旧独立进程方案已清理 (2026-08-17)**:source/ 目录、NovaLibreOfficePlayerDeprecated target、NovaLibreOfficePlayer.vcxproj、PptAnimationManagerLinux/LibreOffice(零实例化, PptCoreExport 全走新链/图片模式)、sln 工程引用、孤儿可执行 全部删除(git 可恢复)。Windows 侧为文本对应清理(CMake/sln/vcxproj),**需 Windows 编译确认**。ShareMemoryReaderLinux/NamePipe* 为 PDF 链/公共设施,保留
- 已知待清理:① ~~calc profile seed 死开销~~(已清, 2026-08-17);② ~~[CALC-T]/[IMP-T] 等诊断日志~~(已清, 2026-08-18 日志体系统一:前缀/级别/单入口,见上条;[CalcLink.Scroll]/[Common.UIHide] 转入 debug 级,ORT_LOG_LEVEL=debug 可见);③ ~~过时探针~~(已清, 2026-08-18: calc 系旧 ABI/uno 系/注入 txt 等 22 文件,探针目录缩至 9 个全登记)
- **writer_cache 总量回收已落地(2026-08-18,简单策略)**:写入后总量超限(ORT_WRITER_CACHE_MB,默认 500MB)按 mtime 最旧删除,排除当前会话文件;实测 1MB 上限 3 文档触发淘汰正常
- 诊断开关:ORT_DUMP_WINDOWS=1(窗口树/重叠/边缘像素);ffplay video_open 打印 renderer 后端
- **环境约束(TRAE sandbox, 2026-08-18 实测)**:TRAE sandbox 阻断子进程写 `~/.office-link/`(`SeedKernelProfile` 的 `fs::copy_file` 报 Permission denied → `BootstrapOffice` 抛 DeploymentException)。影响:所有需 LO bootstrap 的探针(impress_nextpage/media_green/impress_multi/writer/pdf_render/word_core/ffplay_inject)在 sandbox 内 bootstrap 阶段失败;office_runtime 单测(用 fake soffice,不引真内核)和 xvfb_stress_probe(纯 Xvfb,无 LO)不受影响。代码逻辑经 shell 手动 `cp` 验证正确,纯属 sandbox 文件策略。真实部署环境无此限制。回归时需在 sandbox 配置放行 `~/.office-link/` 读写,或在无 sandbox 环境跑。

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
| 20 | **幻灯片属性对齐旧方案**:IsEndless=true 循环保活等(语义源自旧独立进程方案, 2026-08-17 已清理, git 历史可查) | 08-12 | 高 |
| 21 | **GL 转场必崩(独立于媒体)**:X11 generic 平台无条件 supportsOpenGL=true→ogltrans→EGL/swrast 崩。修复=SAL_DISABLEGL=1(EnsureKernel setenv,勿回退;转场退化为 CPU 渲染效果保留) | 08-13 | 高 |
| 26 | **窗口黑边/瞬态/串流三层**:① user 配置窗口状态→全屏瞬态根源(结构修复:窗口属性配置 1920x1080 同 Calc)② 旧 user UI 残留 23px 黑边→UNO 动态隐藏(setMenuBar(null)+hideElement,不依赖 user 配置)③ LO 窗口固有 3px 边框不可控(接受)。UNO setPosSize/visible-toggle 在 slideshow 运行中会黑屏,勿用。诊断:DumpWindowEdges/CheckWindowOverlap(ORT_DUMP_WINDOWS=1) | 08-13 | 高 |
| 27 | **播放内核独立 profile**(原 `~/.office-link/player`,2026-08-18 更名为 `~/.office-link/xvfb`,见经验 40):与任何默认 profile 的 soffice 彻底隔离(经验 22);部署 office/user 不再被写 | 08-13 | 高 |
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

### 2.6 文档渲染(writer, 已落地 2026-08-17)

| # | 经验 | 时间 | 置信度 |
|---|---|---|---|
| 40 | **user 模板机制 + office-link 命名定稿 (2026-08-18)**:① **UI 控制三层优先级(定论)**: UNO API > 平台窗口 API(X11/Win32) > user 模板配置 — 单一层做不到完全控制, 模板是基线兜底不承担运行时控制; Windows 的 per-session fresh copy 正是该层配套防御(运行期写回的 UI 状态不跨 session 存活) ② **命名**: Linux `~/.office-link/xvfb/`(内核跑在 Xvfb 上, 名字直指机制; 原 player/ 更名, 运行时数据无迁移负担)、Windows `desktops/<link>/<guid>/`(每 session 独立桌面); office_paths: `xvfb_profile()/desktop_profile()/user_template()` ③ **模板 = 仓库 `templates/user/registrymodifications.xcu` 单文件**(126→66 item 净化: 保留 3 工具栏 Visible=false+Locked/TabBarVisible=false/SlideSorterBar 按视图/Misc.Start 放映 4 条/Sidebar ContextList 10 条/FirstRun=false/两个 Factory 窗口属性=固定值 `10,1,1920,1080;1;,,,;`(原值机器相关 3725x1992, 模板须跨机器); 剔除: 最近文件/Recovery/绝对路径/时间戳/Linguistic/ooLocale(让环境决定)/默认值写回约 60 条)。构建随 OfficeRuntime 部署到 office/program/templates/ ④ **消费语义双平台统一**: 引导/会话创建时 fresh copy(回模板基线), Linux `SeedKernelProfile`(EnsureKernel 引导前; **活内核防护**: cmdline 含 soffice.bin+该 profile 的进程活着时跳过 — 跨进程共享内核复用路径绝不能删正在运行的内核的 profile), Windows 平台层 seed(office/user 退役) ⑤ **实证**: 模板三要素(工具栏/TabBar/窗口属性固定值)在运行 profile 中生效且 LO 写回不覆盖; 全链探针 20/20 ⑥ **孤儿文档锁坑(新)**: 用户 UI soffice 会话退出后 `.~lock.<doc>#` 残留(锁跨 profile 生效!)→ 播放链 Hidden 加载返回空组件("doc loaded FAILED"), 表现为"任何 profile/模板配置下都失败" — 排查先查文档同目录锁文件; 2026-08-18 实测差点误判为模板回归 | 08-18 | 高(实证) |
| 38 | **writer 渲染方案可行性(2026-08-17 探针实测)**:**方案 A(自治 PDF, 采用)**: docx → PDF → PDF 导入 Draw → 逐页 XSlideRenderer::createPreview → XBitmap::getDIB → BGRA。全链路 UNO 公开接口, LO 自治零第三方(mupdf/poppler 均不需要); 不需要 Xvfb/窗口/抓帧(纯离屏渲染) —— 契合"不用截屏和虚拟屏"与"内核稳定优先"。**接口细节(落地直接复用)**: ① 转 PDF 用 `XStorable::storeToURL(url, {FilterName="writer_pdf_Export"})`(**XModel 无 storeToURL**; 同内核内转换, 不需要外部 --convert-to 进程, 独立 profile 隔离仍适用); ② PDF 导入 `loadComponentFromURL(pdf, FilterName="draw_pdf_Import")`(Hidden); ③ `XSlideRenderer` 服务名 `com.sun.star.drawing.SlideRenderer`(实现 com.sun.star.comp.Draw.SlideRenderer, sd/source/ui/presenter/SlideRenderer.cxx), `createPreview(XDrawPage, awt::Size(宽,高), superSample)` → `awt::XBitmap` —— 输出尺寸按页面比例适配(竖版 A4 @1080 高 → 763x1080, 完整页面); ④ `XBitmap::getDIB()` 返回 **BMP 文件格式**(非裸 DIB!): 'BM'(0-1) + 像素偏移(10-13=0x36=54) + BITMAPINFOHEADER(biWidth@18, biHeight@22, biBitCount@28=24bpp) + 行对齐 4 字节 —— 解析陷阱, 按 offset 10 的像素偏移取值, 勿假设 40 字节头。**性能实测**(pdf_render_probe, build_probes.sh 已登记): 戴奥良-简历.docx(1页): 转换 102-113ms + 导入 217-264ms + 首渲染 81ms(总 ~0.5s); NovaPlayer概要设计说明书.doc(90页): 转换 ~2.9s + 导入 ~6.1s + 逐页渲染 19-72ms/页(总首开 ~9s, 一次性); 翻页 20-70ms/页(翻页语义足够)。**方案 B(直接渲染 XRenderable)排除**: Writer 文档 `XRenderable::getRendererCount=0` —— XRenderable 是导出器基础设施(PDF 导出内部用, filter/source/pdf/pdfexport.cxx), UNO 公开层 render 的 xOptions 是导出选项, 无位图输出路径。**探针坑**: 中文路径必须 `OStringToOUString(UTF8)`(createFromAscii 损坏→mojibake→type detection failed); LO type detection 依赖 LANG(经验 25 陷阱, 探针 setenv 兜底)。**writer link 设计**: C ABI 同构 calc/impress, 复用 office_runtime 内核/BootLock; **无平台层**(不需要 LinkPlatform); 页表 = Draw 文档 XDrawPages, 翻页 = createPreview 当前页; 大文档首开 9s 的优化方向: 转换缓存/后台预转。**落地决策(2026-08-17 讨论定稿, 二轮修订)**: ① 不做懒转换(保留优化空间); ② 内存 = 按需渲染 + 当前页±2 LRU 缓存(渲染 19-72ms/页, 按需足够); ③ **PDF 缓存键 = 源文件 MD5**(修订: 原 SHA-1, 为与 /tmp/NPOfficeCache 统一——一次计算双向兼容): 转换前先查 `/tmp/NPOfficeCache/<md5>.pdf`(Nova 缩略图链产物, GlobalDataSet::DoConvertDocumentW, 外部 soffice+独立 profile convertuser/<md5> 用后清), **命中总是拷贝**到 `~/.office-link/writer_cache/<md5>.pdf`(/tmp 易失+免疫外部清理; 总量上限最旧回收, 大文件阈值等优化空间保留); 未命中才自转(同内核 storeToURL), 写 writer_cache(`<md5>.pdf.<pid>.tmp` → rename 原子, 并发同播无冲突); 命中/自转后播放链直接 draw_pdf_Import(**跳过 docx 加载+转换**, 90 页场景 9s→~6.2s; draw_pdf_Import 为进程内对象, 跨会话不可缓存 = 命中后成本下限); `_N.pdf` 后缀是缩略图页版(Windows PageRange; **Linux 分支无滤镜实际全量**, 实测与主文件同字节)——writerlink 只认无后缀全量版。**反向协同不做(Nova 缩略图链不查 writer_cache)——依赖方向纪律: writerlink 定位为 NovaOfficeCore 插件, 依赖必须单向(上层→下层), 上层感知下层缓存即反向耦合**; ④ 架构 = 无平台层定案, Windows 侧 bootstrap 落 calc_session 的 `#ifdef _WIN32` 同款模式; ⑤ 并行会话协作约定: 清场命令(kill Xvfb)只处理自己的 display 号或先互查(:90 是共享运行时的, 12:46 实测互踩过一次)。**上层接线(2026-08-17 二轮定稿)**: ⑥ NovaOfficeCore/word 新增 LibreOfficeWriterManager(dlopen writerlink, 同构 LibreOfficeImpressManager); WordCoreExport 的 **WORD_PLAY_MODE 参数已存在但当前被忽略**——启用为正式分发(加枚举值, 定义在 NovaPlayer 侧头 NP_WORD_PLAY_MODE, 加值需跨仓库同步); 实际落地为**正式 mode 分发**(比原计划更进一步): WORD_PLAY_MODE_ANIMATION_LIBREOFFICE=3 走新链, 其余走 WordManager(PDF 链, 不动); 上层 Manager 方案(IWordManager/LibreOfficeWriterManager)2026-08-18 正式落地: IWordManager 抽象基类提交, WordManager/LibreOfficeWriterManager 共同实现, WordCoreExport 的 g_map_word 持 IWordManager* 分发; NWordExportThumbnail 缩略图接口不动(自带缓存链, 与播放链互不干扰)。**质量收尾(2026-08-17 三轮)**: ⑦ **UpdateFrame 已修**: 原实现只置标志等轮询, Stop 后轮询线程已停→标志无人消费→"停止后取一帧"黑屏(writer_probe 复现 frames=0); 现锁内直接 PushFrame(语义对齐 impress); ⑧ **LO 统一尺寸认知(draw_pdf_Import)**: 混合页面尺寸 PDF(横竖混排实测 612x792/842x595/595x842)导入 Draw 后**所有页统一为第一页尺寸**, createPreview 全部同尺寸输出(834x1080)——"缓存命中不刷新 width_/height_ 的错配前提不存在"(per-page 尺寸处理不需要); writer_probe 的 WRITER_MIXED 段留作回归锚点(LO 行若变会 FAIL 提醒); ⑨ writerlink 纳入 linksmoke(ABI 一致性同机制, 单测 49→50 检查); ⑩ calc_session 精简 include 后 syscall 需显式 <unistd.h>(传递包含被移除暴露); 2026-08-18 改进: 加 <sys/syscall.h> 用 SYS_gettid 宏替代硬编码 186(x86_64=186, aarch64 不同, 可移植) | 08-17 | 高(实测) |

## 三、项目规划

> ★★★=立即;★★=中期;★=远期。

### 3.1 待办/讨论

| 排序 | 事项 | 说明 |
|---|---|---|
| ★★ | calc_session Linux 下 profile seed 死开销(每次 Create 复制整个 office/user 但 Linux 不消费;Windows bootstrap 才用)| 注释审查发现,讨论后改 |
| ★★ | ffplay 能力增强(按需):XFrameGrabber 帧抓取/硬解/媒体信息 | 引擎底座就绪 |
| ★ | ffplay 引擎并发创建竞态(错开即好,LO 天然满足;紧邻创建场景需引擎内串行化) | 按需 |

### 3.2 功能/平台

| 排序 | 事项 | 说明 |
|---|---|---|
| ★★ | 2160p 混合分辨率落位产品化验证(默认配置已支持) | 配置验证 |
| ★★ | slot 管理策略(超限语义/动态轮替/最大并发数) | 策略决策 |
| ★ | Windows impress 平台补全(当前 stub)+ Windows 编译验证(含 2026-08-17 清理后的 CMake/sln 文本改动) | 平台补全 |
| ★ | 环境自检(字体/音频缺失明确报错)与崩溃检测告警 | 部署稳健性 |

---

*已关闭:gstreamer 路径清理(08-14);LO 改动同步远端(08-17,commit 83e0b9c3e);ffplay 多实例并行播放(08-17,经验 37);office_runtime 防御增强与单测加固(08-17,经验 35);**废弃 source/ 旧方案**(08-17,零实例化实证后全平台清理,1.6);writer UpdateFrame 语义修复(08-17,经验 38⑦);**writer 模块全链路闭环**(08-17,经验 38,核心交付);诊断日志清理(08-18,日志体系统一);**IWordManager 抽象落地**(08-18,WordManager/LibreOfficeWriterManager 共同实现,WordCoreExport 持 IWordManager* 分发);代码重构(08-18,link_utils 工具整合/DEFER/UNO_GUARD/异常日志补全/SYS_gettid 可移植)。*
