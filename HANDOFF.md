# NovaLibreOfficePlayer 交接文档

> 新会话起点:先读本文件,再按"三、规划"推进。
> 代码在 `NovaLibreOfficePlayer/`(calc/impress + common/ + office_runtime 含 ffplay)+ 上层 `NovaOfficeCore/`(dlopen links)。
> 经验编号被代码注释引用,**编号只增不改**;每次认知提升更新"二、经验"(带时间+置信度),完成事项移入"一、现状"。

---

## 一、项目现状 [参考] (updated 2026-08-19)

### 1.1 架构

```
NovaLibreOfficePlayer/    (NovaPlayerTools/cmake 单一树子项目; target: OfficeRuntime/
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
  │     windows/win_platform.*  WindowsPlatform (CreateDesktop 独立进程模式; 平台隔离
  │                               新接口 Plan/BootSection 等, calc/impress 共用, 经验 39/44)
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
- **writerlink 底层链路已闭环**(writerlink,经验 38):底层(翻页 20-53ms/页、LRU、缓存命中 0ms、Prev 验证)探针实测全绿。**上层接线已回退(2026-08-18)**:NovaOfficeCore/NovaPlayer/NovaPlayerDemo 的 word LibreOffice 接入改动(IWordManager 抽象/LibreOfficeWriterManager 分发/NP_WORD_PLAY_MODE_ANIMATION_LIBREOFFICE 枚举/Demo Word 模式下拉框)整体还原,功能就绪待后续接入。LibreOfficeWriterManager.cpp/.h 作为样板保留(NovaOfficeCore/word/,不参与构建,去 IWordManager 依赖)
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
- **日志体系(2026-08-18 收尾定稿)**:统一入口 `OfficeLog/Dbg/Warn/Err`(varargs,LogMsg 等历史包装已删);前缀 = target 名 `[OfficeRuntime]/[CalcLink]/[ImpressLink]/[WriterLink]/[KernelHost]`(子场景点分如 `[CalcLink.Scroll]`/`[Common.UIHide]`/`[Common.WinWindow]`/`[Common.WinProfile]`/`[Common.X11]`/`[Common.Boot]`);平台层 Tag() 输出 lowercase `[calc]/[impress]`(区分会话层 `[CalcLink]/[ImpressLink]`);ffplay 组件在 soffice 进程内(office_runtime.so 不在),保留独立 fprintf + `[FFPLAY]`。级别:info=生命周期主线 / debug=诊断细节(窗口扫描/UI 自省/渲染计时) / warn=防御拦截与回退 / error=失败;文件格式 `[时间] [level] [前缀] 消息`,双平台一致(win_office_log 对偶)。开关:ORT_LOG=both(默认)|file|stderr|off(**off 真 silent**——仅跳过初始化时 spdlog 默认 logger 仍打 stdout,已修)、ORT_LOG_LEVEL=debug|info(默认)|warn|error。落位 `office_paths::logs_dir()/office_<pid>.log`(Linux spdlog 5MB×3 轮转;stderr 副本有缓冲差异,排查以文件为准)。**前缀标准化(2026-08-18)**:`[Common]`→`[Common.Boot]`、`[CAPTURE]`→`[Common.WinWindow]`(归入 WinWindow 子域)
- **所有者退出连坐**:共享内核/屏的所有者进程退出,其他进程会话断开;双实例部署需同时使用
- **Windows 平台隔离回归已完成 (2026-08-19)**: 编译零错误 (win_platform 新接口
  Plan/BeginBoot/DiscoverWindow/FormWindow/ApplyNativeFullscreen/OnSessionEnd +
  calc F 反序 + impress 全屏放映 + writer KernelHost); 探针五段全绿 + NovaPlayerDemo
  calc/impress 全量通过 (UI 全隐藏含公式栏, 经验 44); 回归修复: win_platform
  Plan() impress discover AfterReveal→AfterStart + 核心 P8 discover 分支 (协议遗漏)
  + NovaOfficeCore PptCoreExport Windows 分发恢复 (被清理误删) + 模板补 calc 基线
  (69→126 项, 见 3.1 模板部署保障)
- UNO_PATH/URE_BOOTSTRAP 依赖部署位置(office/program),部署路径变化需同步(经验 23/33)
- **旧独立进程方案已清理 (2026-08-17)**:source/ 目录、NovaLibreOfficePlayerDeprecated target、NovaLibreOfficePlayer.vcxproj、PptAnimationManagerLinux/LibreOffice(零实例化, PptCoreExport 全走新链/图片模式)、sln 工程引用、孤儿可执行 全部删除(git 可恢复)。Windows 侧为文本对应清理(CMake/sln/vcxproj),**需 Windows 编译确认**。ShareMemoryReaderLinux/NamePipe* 为 PDF 链/公共设施,保留
- 已知待清理:① ~~calc profile seed 死开销~~(已清, 2026-08-17);② ~~[CALC-T]/[IMP-T] 等诊断日志~~(已清, 2026-08-18 日志体系统一:前缀/级别/单入口,见上条;[CalcLink.Scroll]/[Common.UIHide] 转入 debug 级,ORT_LOG_LEVEL=debug 可见);③ ~~过时探针~~(已清, 2026-08-18: calc 系旧 ABI/uno 系/注入 txt 等 22 文件,探针目录缩至 9 个全登记)
- **writer_cache 总量回收已落地(2026-08-18,简单策略)**:写入后总量超限(ORT_WRITER_CACHE_MB,默认 500MB)按 mtime 最旧删除,排除当前会话文件;实测 1MB 上限 3 文档触发淘汰正常
- 诊断开关:ORT_DUMP_WINDOWS=1(窗口树/重叠/边缘像素);ffplay video_open 打印 renderer 后端
- **writerlink 上层接线已回退(2026-08-18)**:writerlink.so 功能就绪(探针全绿),但 NovaOfficeCore/NovaPlayer/NovaPlayerDemo 的接入改动整体回退(IWordManager 抽象删除、WordCoreExport/WordManager 还原、NP_WORD_PLAY_MODE_ANIMATION_LIBREOFFICE 枚举移除、Demo Word 模式下拉框移除)。LibreOfficeWriterManager.cpp/.h 作为样板保留在 NovaOfficeCore/word/(去 IWordManager 依赖,不参与 CMake/vcxproj 构建),后续接入时恢复继承+override+构建配置即可。NovaLibreOfficePlayer/writer/ 本身不动
- **环境约束(TRAE sandbox, 2026-08-18 实测)**:TRAE sandbox 阻断子进程写 `~/.office-link/`(`SeedKernelProfile` 的 `fs::copy_file` 报 Permission denied → `BootstrapOffice` 抛 DeploymentException)。影响:所有需 LO bootstrap 的探针(impress_nextpage/media_green/impress_multi/writer/pdf_render/word_core/ffplay_inject)在 sandbox 内 bootstrap 阶段失败;office_runtime 单测(用 fake soffice,不引真内核)和 xvfb_stress_probe(纯 Xvfb,无 LO)不受影响。代码逻辑经 shell 手动 `cp` 验证正确,纯属 sandbox 文件策略。真实部署环境无此限制。回归时需在 sandbox 配置放行 `~/.office-link/` 读写,或在无 sandbox 环境跑。

---

## 二、历史经验(勿回退;编号被代码注释引用) [经验·永久] (updated 2026-08-19)

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
| 41 | **Impress 暂停→恢复翻页失效**:pause/resume 不对称 + StartPoller early-return 致 paused_ 不重置。FramePump 接入后同构复现 (泵 Start 幂等早返未重置 paused_), 已由泵契约根治。详见下方 [经验 41 详述](#经验-41-详述) | 08-18 | 高(实测修复) |
| 42 | **FramePoller 共性分析与治理**:三 link poller 六维不一致 + P3-P8 新发现。FramePump 组件统一帧泵, 阶段0-4 全部落地, 三链接入收官。详见下方 [经验 42 详述](#经验-42-详述) | 08-18 | 高(阶段0-4全部完成) |
| 43 | **BootLock 构造即加锁 + 非递归 mutex 自死锁**:包装"构造即获取"型 RAII 资源, 包装层构造函数必须为空; 二次 Lock = 静默永久死锁(无日志/超时不保护)。详见 3.0 验证记录 | 08-18 | 高(源码级+实测修复) |
| 44 | **Calc 公式栏 (fx/Σ 输入行) 隐藏 (2026-08-18 demo 实测)**:公式栏是 **SFX docking window** (UI 布局 inputbar.ui, 窗口类 InputBar), **不是 LayoutManager toolbar 元素** —— hideElement(formulabar)/模板条目/ShowFormulaBar 属性 (SDK IDL 无此名, 猜测无效) 全部不生效; 老 office/user 亦无其持久化条目 (老会话未隐藏过, 搜 formula 仅 2 处计算/sidebar 配置)。**真实控制 = UNO 命令 `.uno:InputLineVisible`** (scalc menubar.xml View 菜单有据可查), dispatch 需 **frame_ provider** (文档级 sc 模块命令; desktop_ queryDispatch 返回 NOT found —— 桌面级命令如 FullScreen 才用 desktop_); 每次会话从模板基线开始公式栏默认显示, toggle 一次即隐藏 (状态确定, 无需查询)。排查陷阱: 公式栏相关的 popupmenu/formulabar.xml 是弹出菜单非主控件; 探针环境 LO 渲染不完整 (画面只画表格首行) —— UI 验证以 demo 为准。**排查纪律 (2026-08-19 复盘)**: UNO_SILENT 异常进 **debug 级日志** (tag+表达式+消息), 默认 info 不可见 —— "静默失败"现象排查时**第一动作开 ORT_LOG_LEVEL=debug** 看 `UNO exception (silent)` 痕迹, 再下"未生效"结论。**[2026-08-19 4.2 实证修正]**: InputLineVisible dispatch 在 Linux 共享内核下破坏 vis=0 初始态导致 UI 复活, 已下沉至 Windows HideUiExtras (Linux 空操作); LO Xvfb 无头环境公式栏默认 vis=0 不显示, 无需 dispatch | 08-18 | 高(实测, 部分认知已修正) |
| 45 | **C ABI 重复 Destroy UAF 防护**: C ABI `ImpressSessionDestroy` / `CalcSessionDestroy` 直接 `delete static_cast<...*>(session)`, 重复调用时悬垂指针 → use-after-free → SIGABRT (确定性必现)。修复: C ABI 层加 `std::unordered_set<void*>` 活跃指针跟踪 + mutex 保护, Create 时 insert, Destroy 时 find+erase, 不在集合中则 no-op (已销毁)。代码: impresslink.cpp / calclink.cpp。失效条件: 改用智能指针管理 session 生命周期时本防护可移除 | 08-20 | 高(确定性必现, 已修复) |

#### 经验 41 详述

**Impress 暂停→恢复翻页失效 (2026-08-18 Demo 实测修复; 2026-08-19 FramePump 接入后同构复现)**

**原发 (2026-08-18, per-session poller 时代)**: 上层 pause/resume 链路不对称——pause 走 `Pause()`(设 `paused_=true`, poller 不停), resume 走 `Start()`(非 `Resume()`)。`Start()` 内 `paused_=false` 原写在 `StartPoller()` 里, 但 `StartPoller()` 对 `poll_running_==true` 做 early-return(pause 不停 poller, 所以恢复时必命中)→ `paused_` 永远不被重置→ poller 跳过抓帧。修复: `Start()` 中显式 `paused_=false`。

**同构复现 (2026-08-19, FramePump 接入后)**: impress 接入 FramePump 后, `Start()` 委托 `pump_->Start()`。FramePump::Start() 幂等早返路径同样未重置 `paused_` (成员上移到泵内), 导致暂停→恢复(走 Start)画面冻结。根因同构: 状态重置依赖幂等早返路径, 但早返跳过了重置。

**根治**: FramePump::Start() 持 `ctrl_mutex_` 内**无条件** `paused_=false` 再判幂等 (frame_pump.cpp)。契约写入: "Start=任何状态→Running 未暂停" (见经验 42 契约表)。新增测试 9 (start_resets_paused_when_running) 闭环。

**教训**: 暂停/恢复走不同入口时, 状态重置必须放在入口函数本身, 不能委托给可能被 early-return 的下游; 幂等路径也必须执行状态重置。

#### 经验 42 详述

**FramePoller 共性分析与治理 (2026-08-18 分析, 2026-08-19 阶段0-4 全部落地)**

三链 (impress/calc/writer) 各自手写一份 poller (`poll_thread_`/`poll_running_`/`paused_`/`force_frame_`/`mu_` 同名同型), 六维不一致演化出 P1-P8 缺陷。本经验为完整治理记录; 设计决策论证/性能预算/测试矩阵见 [五、帧泵专项](#五、帧泵专项)。

**契约 (三链同一份, FramePump 构造性保证):**

| 方法 | 契约 |
|---|---|
| Start | 幂等; 任何状态调用后 = Running 且未暂停 (无条件 `paused_=false` 再判幂等, 构造性消灭 P1/P5; 2026-08-19 回归修复: 幂等早返未重置 paused_ 致 impress 暂停→恢复无法翻页) |
| Stop | 幂等; join 泵线程, 排空在途帧; 之后无任何自动推帧 |
| Pause | 冻结周期推帧 (心跳是否照推 = plan.heartbeat_when_paused); 绝不影响 UpdateFrame; 内容暂停 (impress slideshow pause) 是会话自己的事, 与泵解耦 |
| Resume | 恢复周期推帧 |
| UpdateFrame | 同步立即帧, Running/Paused/Stopped 任何状态有效 (修 P6); 与泵 tick 串行 (修 P3); 返回抓帧成败 |

生命周期不变量: **泵必须先 Stop, 会话才能清 UNO 对象/平台资源** (probe 与 FrameFn 引用的 pane_/view_/platform_ 仅在泵停止后可销毁)。

**缺陷清单 → 机制映射 (全部闭环):**

| 问题 | 描述 | 消灭机制 |
|---|---|---|
| P1 | calc Start() 不重置 paused_ (同经验 41 形态) | 契约 "Start=任何状态→Running 未暂停" (泵内无条件 paused_=false) |
| P1' | impress force_frame_ 无意义 + 置位窗口期并发 (经验 42 原描述"只读不清"已勘误: 实为置位→直推→立即复位, 标志本身无意义) | impress 标志删除 (UpdateFrame 走 frame_mutex_ 直推); writer/calc 保留 force_frame_ 作 ChangeFn 脏位 (有意义) |
| P3 | impress 并发抓帧数据竞争 (UpdateFrame 调用方线程 vs PollThread 泵线程, 共写 cap_bgra_/XShm) | frame_mutex_ 串行所有 FrameFn (构造性) |
| P4 | 帧回调 cb_ 在会话锁 mu_ 内 (calc/writer) | 锁纪律: cb_ 锁外投递; 全局锁序 frame_mutex_→mu_(短), 严禁反向 |
| P5 | calc 双 Start 竞态 → std::terminate (StartPoller check-then-act 非原子, 对 joinable poll_thread_ 赋值) | 泵内部生成互斥 + Start 幂等 |
| P6 | calc 停止后取帧黑屏 (UpdateFrame 仅置标志, Stop 后标志无人消费) | UpdateFrame 全状态有效 (Stopped 就地执行) |
| P7 | 性能三连: calc/writer 5ms 全速循环=200 唤醒/s; calc 每 5ms 一次 UNO 视口查询=200 IPC/s; impress 静止 25fps 全量重推≈208MB/s | tick 合并 (calc 200→50 唤醒/IPC); dedupe 待阶段5 |
| P8 | impress width_/height_ 无同步写读 (泵线程写, GetWidth 读, 形式 UB) | — (迁移期未单独处理, FramePump 路径下宽高写主要在 Create/Start 阶段) |

**勘误 (经验 42 原表述修正):**
- "Calc 心跳暂停也跳" → 应为 "**暂停照推**" (calc 心跳判定无 paused_ 门控, 暂停中仍 10fps; writer 才是暂停冻结)。语义分歧从未被有意决策, 迁移期 plan.heartbeat_when_paused 显式保留现状 (calc=true, writer=false)
- "Impress force_frame_ 泄漏" → 已勘误 (见 P1'): 标志不泄漏, 真实缺陷是无意义+并发

**FramePump 设计 (common/frame_pump.h/.cpp):**
- `FramePumpPlan { tick_ms, heartbeat_ms, heartbeat_when_paused, fail_backoff_ms }` — 每链一份 plan 数据, 差异降维
- `frame_mutex_` 串行所有 FrameFn 执行 (泵 tick + UpdateFrame 调用方就地执行, 否决"单线程委托"方案: 引入唤醒延迟且 Stopped 态仍须回退就地执行)
- `ctrl_mutex_` + condvar tick (Stop 可立即打断等待)
- ChangeFn (可选): 内容是否可能变化 (calc 视口签名 / writer 脏位); impress 无 probe=恒真
- 锁纪律: 调用泵方法不得持 mu_; FrameFn/ChangeFn 内部自取短会话锁; 全局锁序 frame_mutex_→mu_

**三链接入形态 (已完成):**

| 链 | tick | heartbeat | hbp | ChangeFn (probe) | FrameFn 锁 |
|---|---|---|---|---|---|
| impress | 40ms (25fps 不变) | 0 (无) | false | nullptr (恒真) | 不持 mu_ (platform 自锁) |
| writer | 5ms | 100ms | false (Pause 冻结) | `force_frame_.exchange` (脏位) | 持 mu_ (访问 page_cache_) |
| calc | 20ms (放宽原 5ms) | 100ms | true (Pause 照推) | `CheckViewportChanged` 持 mu_ (视口签名 row/col/sheet + force_frame_ 脏位) | 不持 mu_ (platform 自锁) |

**迁移路径 (全部已完成):**
- **阶段0** (2026-08-19): 各一行级修复, 立即消灭 P1/P5/P6, 临时防 P3 (已被阶段2-4 取代)
- **阶段1** (2026-08-19): FramePump 组件 + 单测落地 (common/frame_pump.h/.cpp + frame_pump_test.cpp)
- **阶段2** (2026-08-19): impress 接入 (无 probe 无心跳, 等价原 PollThread); demo 回归通过 (放映帧/1px/暂停恢复)
- **阶段3** (2026-08-19): writer 接入 (脏位 probe, Pause 冻结); demo 回归通过
- **阶段4** (2026-08-19): calc 接入 (视口签名+脏位 probe, Pause 照推, tick 20ms); demo 回归通过
- **阶段5** (可选, 未做): 平台层段内比对 (CaptureFrame 增量 unchanged 参数, 省应用层 8.3MB 拷贝) + dedupe + calc zoom 维度 A/B

**单测**: frame_pump_test.cpp 9 个测试 15 checks (Start 幂等/Stop 排空/Pause 冻结/UpdateFrame 串行/心跳/失败退避/重启/ChangeFn 探测/Start 重置 paused_ 回归), 全绿

**开放问题 (不影响阶段0-4, 可延后):**
- A. 帧新鲜度 TTL: 消费方是否存在"末帧超时视为无帧"? 决定心跳保留(近零成本) or 静止静默(收益最大)。dedupe 两阶段设计使该问题可延后且不返工
- B. calc tick 20ms 滚动延迟接受度 (最坏 +15ms): 默认 20ms, plan 一处可改, 可 A/B 探针实测后定

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
| 16 | **ABI 同步已由依赖图承接**(office_runtime.h 变更重编三件套);历史教训:只重编 office_runtime 导致 undefined symbol | 08-12 前 | 高 |
| — | **组件名陷阱**:gstreamer 生效组件是 libavmediagst.so(libavmedialo.so 是另一组件) | 08-12 | 高 |
| 36 | **相对 LD_LIBRARY_PATH 陷阱(2026-08-17)**:手动跑探针/单测用相对路径→dladdr→UNO_PATH 相对化→BootstrapOffice DeploymentException SIGABRT。产品不受影响(run.sh $CURDIR 绝对)。**探针/单测烧入绝对 RUNPATH,免设直接跑(推荐)**;手动设置必须绝对路径。部署目录=NovaPlayer/bin_<arch>_<sys>(非 NovaPlayerTools/) | 08-17 | 高 |

---

### 2.6 文档渲染(writer, 已落地 2026-08-17)

| # | 经验 | 时间 | 置信度 |
|---|---|---|---|
| 40 | **user 模板机制 + office-link 命名定稿**:UI 控制三层优先级 / 命名 / 模板净化 / 消费语义 / 孤儿文档锁坑 / sidebar+statusbar 存储位置与部署陈旧坑(⑦⑧)。详见下方 [经验 40 详述](#经验-40-详述) | 08-18 | 高(实证) |
| 38 | **writer 渲染方案可行性**:docx→PDF→Draw→XSlideRenderer→BGRA 全 UNO 自治 / 接口细节 / 性能 / 缓存 / 上层接线 / 质量收尾。详见下方 [经验 38 详述](#经验-38-详述) | 08-17 | 高(实测) |
| 39 | **Windows 平台差异定稿**:per-session 独立 soffice + 隐藏桌面 (`SALTMPSUBFRAME`) + `IsFullScreen=true` (LO 自管窗口, 无菜单栏/标题栏) + 双平台日志同款实现 (Linux office_runtime.cpp spdlog / Windows common/win_office_log.cpp) — 与 Linux 共享内核模式正交的设计分支。代码引用: writer_session.h:6 / impress_session.cpp:7 / impress_session.h:24 / link_platform.h:6 / win_platform.cpp:156 / log.h:12 / common/CMakeLists.txt:16 / office_runtime.h:19 | 08-19 | 高(架构定稿) |

#### 关键经验失效条件 (2026-08-20 补充)

> 每条经验都基于特定技术假设。当假设失效时, 经验需重新评估。以下列出关键经验的失效触发条件, 便于维护者判断何时该重新审视。

| 经验 | 失效条件 | 重新评估方向 |
|---|---|---|
| **1** (Xvfb 大屏+slot) | 迁移到 Wayland / X server 改用 compositing | Wayland 下窗口遮挡语义不同, slot 方案可能不需要 |
| **19b** (Xvfb 恒无 GPU) | Xvfb 配置 GPU 加速 / 改用 DRM/KMS | 软件渲染定论失效, 需重新评估 GL 路径 |
| **21** (GL 转场必崩) | SAL_DISABLEGL 不再有效 / LO 改 GL 实现 | 禁 GL 决策需重新验证 |
| **25** (libstdc++ 6.0.30) | SDK 升级带新 libstdc++ / 系统更新 | SONAME 单例陷阱仍存在, 但版本号需更新 |
| **29/30** (ffplay 注入) | LO 源码 mediawindow_impl.cxx 改后端选择逻辑 | ORT_MEDIA_BACKEND 环境变量开关可能失效 |
| **34** (ffplay_embed.patch) | FFmpeg 上游 ffplay.c 大改 / patch 冲突 | 需重新生成 patch, 评估是否仍可行 |
| **37** (多实例并行) | SDL2 改渲染器选择逻辑 / 多线程行为变更 | SDL_FRAMEBUFFER_ACCELERATION=0 + SOFTWARE renderer 可能不再必要 |
| **39** (Windows 平台差异) | Windows LO 内核架构变更 / 改用共享内核 | per-session 独立 soffice + 隐藏桌面方案需重新设计 |
| **42** (FramePump) | 改用其他帧泵机制 / LO 提供 UNO 动画状态接口 | 静止检测两阶段方案需重新评估 |
| **43** (BootLock 死锁) | BootLock 构造函数不再执行 Lock() | 包装层契约需重新验证 |

#### 零引用经验清单 (2026-08-20 审查)

> 以下经验在代码注释中无引用 (grep `经验 N` 命中 0 次), 但仍保留在第二章。原因: 编号锚定不可删 (未来代码可能引用); 经验本身仍承重 (踩坑风险仍在, 只是当前代码未引用)。维护时可优先考虑合并/降级这些经验。

**零引用经验**: 2 / 3 / 4 / 6 / 7 / 8 / 9 / 10 / 11 / 12 / 13 / 14 / 15 / 19c / 19d-g / 20 / 29 / 36 (共 19 条)

**说明**:
- 经验 19 基类 0 引用, 但变体 19b 有 6 处引用、19c/19d-g 完全 0 引用。19 的正文 (弯路勿重走 5 条) 已被 19b/19c/19d-g 完全拆分继承, 可考虑把基类 19 归并进 19b 并删除基类条目。
- 经验 13/14/15 (XShm 抓帧三件套) 虽 0 代码引用, 但实现已落地于 xvfb_platform 抓帧路径, 可改为附在 xvfb_platform.cpp 注释中作为"背景来源"。
- 经验 19d-g 自身注释已标注"已被 29/30 取代", 本身即承认零活引用, 可降级为附录"探索历程备查"。
- 完整代码引用审查报告见 git 历史 commit (2026-08-20 调研)。

#### 经验 40 详述

**user 模板机制 + office-link 命名定稿 (2026-08-18)**

- ① **UI 控制三层优先级(定论)**: UNO API > 平台窗口 API(X11/Win32) > user 模板配置 — 单一层做不到完全控制, 模板是基线兜底不承担运行时控制; Windows 的 per-session fresh copy 正是该层配套防御(运行期写回的 UI 状态不跨 session 存活)
- ② **命名**: Linux `~/.office-link/xvfb/`(内核跑在 Xvfb 上, 名字直指机制; 原 player/ 更名, 运行时数据无迁移负担)、Windows `desktops/<link>/<guid>/`(每 session 独立桌面); office_paths: `xvfb_profile()/desktop_profile()/user_template()`
- ③ **模板 = 仓库 `templates/user/registrymodifications.xcu` 单文件**(126→66→69 item 两轮净化: 第一轮 66 条(保留 3 工具栏 Visible=false+Locked/TabBarVisible=false/SlideSorterBar 按视图/Misc.Start 放映 4 条/Sidebar ContextList 10 条/FirstRun=false/两个 Factory 窗口属性=固定值 `10,1,1920,1080;1;,,,;`(原值机器相关 3725x1992, 模板须跨机器); 剔除: 最近文件/Recovery/绝对路径/时间戳/Linguistic/ooLocale(让环境决定)/默认值写回约 60 条), 第二轮 +3 条补 sidebar/statusbar(见⑦)); 构建 POST_BUILD 随 OfficeRuntime 部署到 office/program/templates/
- ④ **消费语义双平台统一**: 引导/会话创建时 fresh copy(回模板基线), Linux `SeedKernelProfile`(EnsureKernel 引导前; **活内核防护**: cmdline 含 soffice.bin+该 profile 的进程活着时跳过 — 跨进程共享内核复用路径绝不能删正在运行的内核的 profile), Windows 平台层 seed(office/user 退役)
- ⑤ **实证**: 模板三要素(工具栏/TabBar/窗口属性固定值)在运行 profile 中生效且 LO 写回不覆盖; 全链探针 20/20; **2026-08-18 用户 demo 肉眼验收: UI 全部隐藏(工具栏/TabBar/sidebar/statusbar), 编辑视图残留治理闭环**
- ⑥ **孤儿文档锁坑(新)**: 用户 UI soffice 会话退出后 `.~lock.<doc>#` 残留(锁跨 profile 生效!)→ 播放链 Hidden 加载返回空组件("doc loaded FAILED"), 表现为"任何 profile/模板配置下都失败" — 排查先查文档同目录锁文件; 2026-08-18 实测差点误判为模板回归
- ⑦ **sidebar/statusbar 的真实存储位置(2026-08-18 定位, 模板第二轮 +3 条的依据)**:
  - **Sidebar(View>Sidebar, Ctrl+F5)不是 LayoutManager 元素** — SFX 子窗口, SID_SIDEBAR = SID_SVX_START(10000)+336 = **10336**(sfx2/source/sidebar/SidebarChildWindow.cxx `SFX_IMPL_DOCKINGWINDOW_WITHID(SidebarChildWindow, SID_SIDEBAR)`), 持久化在 `/org.openoffice.Office.Views/Windows` 的 **`WindowType['simpress/10336']`** 节点(2 item: UserData + WindowState)。序列化是整条路径进 `oor:path=`, **grep `oor:name="simpress/10336"` 查不到**(审计时易误判缺失)
  - **Statusbar 不在 toolbar 命名节点** — `/org.openoffice.Office.UI.ImpressWindowState/UIElements/States` 直项内嵌 `<node oor:name="private:resource/statusbar/statusbar">` Visible=false; 白名单规则若要求路径含 `resource/toolbar/` 会漏掉它(第一轮净化就这么丢的)
  - **UNO 自省盲区**: LayoutManager.isElementVisible 对 SFX 子窗口(sidebar)报 0 而像素仍在(hideElement 对它不生效/无意义) — UI 自省不能完全反映真实布局, 肉眼是最终裁判(用户定论); 这两个元素的**有效控制层 = 模板**(三层优先级里的第三层在此场景反而是唯一起效的)
  - 模板再净化(用户从 UI 重新配置再提取)时的保留规则: 上述两条的位置匹配必须保留(States 直项路径精确匹配 + `simpress/10336` 内容匹配), 否则重新丢条目
- ⑧ **模板部署陈旧坑(2026-08-18 实测踩过)**: 仓库模板更新后 office/program/templates/ 部署副本仍是旧的(上次构建早于模板编辑) → Linux seed 用部署副本, 运行时 profile 一直缺条目, 症状="模板明明加了配置但不生效"。**规则: 改 templates/user 后必须重建 OfficeRuntime(POST_BUILD 拷贝)或手动 cp, 并 diff 确认部署副本一致**; 排查 UI 不生效先核对三方(仓库模板/部署副本/运行时 profile)条目数
- ⑨ **活内核 seed 跳过的验证姿势**: SeedKernelProfile 的活内核防护(cmdline 匹配跳过)意味着**改模板后若内核还活着, 新配置不生效** — 需确保 soffice 重启(探针退出会 atexit 停内核; demo 常驻进程需重启)

#### 经验 38 详述

**writer 渲染方案可行性 (2026-08-17 探针实测)**

**方案 A(自治 PDF, 采用)**: docx → PDF → PDF 导入 Draw → 逐页 XSlideRenderer::createPreview → XBitmap::getDIB → BGRA。全链路 UNO 公开接口, LO 自治零第三方(mupdf/poppler 均不需要); 不需要 Xvfb/窗口/抓帧(纯离屏渲染) —— 契合"不用截屏和虚拟屏"与"内核稳定优先"。

**接口细节(落地直接复用)**:

- ① 转 PDF 用 `XStorable::storeToURL(url, {FilterName="writer_pdf_Export"})`(**XModel 无 storeToURL**; 同内核内转换, 不需要外部 --convert-to 进程, 独立 profile 隔离仍适用)
- ② PDF 导入 `loadComponentFromURL(pdf, FilterName="draw_pdf_Import")`(Hidden)
- ③ `XSlideRenderer` 服务名 `com.sun.star.drawing.SlideRenderer`(实现 com.sun.star.comp.Draw.SlideRenderer, sd/source/ui/presenter/SlideRenderer.cxx), `createPreview(XDrawPage, awt::Size(宽,高), superSample)` → `awt::XBitmap` —— 输出尺寸按页面比例适配(竖版 A4 @1080 高 → 763x1080, 完整页面)
- ④ `XBitmap::getDIB()` 返回 **BMP 文件格式**(非裸 DIB!): 'BM'(0-1) + 像素偏移(10-13=0x36=54) + BITMAPINFOHEADER(biWidth@18, biHeight@22, biBitCount@28=24bpp) + 行对齐 4 字节 —— 解析陷阱, 按 offset 10 的像素偏移取值, 勿假设 40 字节头

**性能实测**(pdf_render_probe, build_probes.sh 已登记):

- 戴奥良-简历.docx(1页): 转换 102-113ms + 导入 217-264ms + 首渲染 81ms(总 ~0.5s)
- NovaPlayer概要设计说明书.doc(90页): 转换 ~2.9s + 导入 ~6.1s + 逐页渲染 19-72ms/页(总首开 ~9s, 一次性); 翻页 20-70ms/页(翻页语义足够)

**方案 B(直接渲染 XRenderable)排除**: Writer 文档 `XRenderable::getRendererCount=0` —— XRenderable 是导出器基础设施(PDF 导出内部用, filter/source/pdf/pdfexport.cxx), UNO 公开层 render 的 xOptions 是导出选项, 无位图输出路径。

**探针坑**: 中文路径必须 `OStringToOUString(UTF8)`(createFromAscii 损坏→mojibake→type detection failed); LO type detection 依赖 LANG(经验 25 陷阱, 探针 setenv 兜底)。

**writer link 设计**: C ABI 同构 calc/impress, 复用 office_runtime 内核/BootLock; **无平台层**(不需要 LinkPlatform); 页表 = Draw 文档 XDrawPages, 翻页 = createPreview 当前页; 大文档首开 9s 的优化方向: 转换缓存/后台预转。

**落地决策(2026-08-17 讨论定稿, 二轮修订)**:

- ① 不做懒转换(保留优化空间)
- ② 内存 = 按需渲染 + 当前页±2 LRU 缓存(渲染 19-72ms/页, 按需足够)
- ③ **PDF 缓存键 = 源文件 MD5**(修订: 原 SHA-1, 为与 /tmp/NPOfficeCache 统一——一次计算双向兼容): 转换前先查 `/tmp/NPOfficeCache/<md5>.pdf`(Nova 缩略图链产物, GlobalDataSet::DoConvertDocumentW, 外部 soffice+独立 profile convertuser/<md5> 用后清), **命中总是拷贝**到 `~/.office-link/writer_cache/<md5>.pdf`(/tmp 易失+免疫外部清理; 总量上限最旧回收, 大文件阈值等优化空间保留); 未命中才自转(同内核 storeToURL), 写 writer_cache(`<md5>.pdf.<pid>.tmp` → rename 原子, 并发同播无冲突); 命中/自转后播放链直接 draw_pdf_Import(**跳过 docx 加载+转换**, 90 页场景 9s→~6.2s; draw_pdf_Import 为进程内对象, 跨会话不可缓存 = 命中后成本下限); `_N.pdf` 后缀是缩略图页版(Windows PageRange; **Linux 分支无滤镜实际全量**, 实测与主文件同字节)——writerlink 只认无后缀全量版。**反向协同不做(Nova 缩略图链不查 writer_cache)——依赖方向纪律: writerlink 定位为 NovaOfficeCore 插件, 依赖必须单向(上层→下层), 上层感知下层缓存即反向耦合**
- ④ 架构 = 无平台层定案, Windows 侧 bootstrap 落 calc_session 的 `#ifdef _WIN32` 同款模式
- ⑤ 并行会话协作约定: 清场命令(kill Xvfb)只处理自己的 display 号或先互查(:90 是共享运行时的, 12:46 实测互踩过一次)

**上层接线(2026-08-17 二轮定稿)**:

- ⑥ NovaOfficeCore/word 新增 LibreOfficeWriterManager(dlopen writerlink, 同构 LibreOfficeImpressManager); WordCoreExport 的 **WORD_PLAY_MODE 参数已存在但当前被忽略**——启用为正式分发(加枚举值, 定义在 NovaPlayer 侧头 NP_WORD_PLAY_MODE, 加值需跨仓库同步); 实际落地为**正式 mode 分发**(比原计划更进一步): WORD_PLAY_MODE_ANIMATION_LIBREOFFICE=3 走新链, 其余走 WordManager(PDF 链, 不动); 上层 Manager 方案(IWordManager/LibreOfficeWriterManager)2026-08-18 回退: writerlink 功能就绪但上层接线暂不接入; IWordManager.h 删除, WordCoreExport/WordManager 还原(回 shared_ptr<WordManager> 直接分发), LibreOfficeWriterManager.cpp/.h 作为样板保留(去 IWordManager 依赖, 不参与构建); NWordExportThumbnail 缩略图接口不动(自带缓存链, 与播放链互不干扰)

**质量收尾(2026-08-17 三轮)**:

- ⑦ **UpdateFrame 已修**: 原实现只置标志等轮询, Stop 后轮询线程已停→标志无人消费→"停止后取一帧"黑屏(writer_probe 复现 frames=0); 现锁内直接 PushFrame(语义对齐 impress)
- ⑧ **LO 统一尺寸认知(draw_pdf_Import)**: 混合页面尺寸 PDF(横竖混排实测 612x792/842x595/595x842)导入 Draw 后**所有页统一为第一页尺寸**, createPreview 全部同尺寸输出(834x1080)——"缓存命中不刷新 width_/height_ 的错配前提不存在"(per-page 尺寸处理不需要); writer_probe 的 WRITER_MIXED 段留作回归锚点(LO 行若变会 FAIL 提醒)
- ⑨ writerlink 纳入 linksmoke(ABI 一致性同机制, 单测 49→50 检查)
- ⑩ calc_session 精简 include 后 syscall 需显式 <unistd.h>(传递包含被移除暴露); 2026-08-18 改进: 加 <sys/syscall.h> 用 SYS_gettid 宏替代硬编码 186(x86_64=186, aarch64 不同, 可移植)

## 三、设计/待办 [设计+待办] (updated 2026-08-20)

### 3.0 平台隔离设计验证 — 已闭环 (2026-08-18)

> 历史验证段已归档; 关键沉淀已分别落入 **经验 43** (BootLock 死锁根因)、**3.3** (设计规格)、**1.6** (当前状态)、**已闭环事项** (commit 5832a507 Windows 回归)。

**目的达成评估** (2026-08-19 Windows 回归 + demo 通过后核验):
- ✅ 会话层零 `#ifdef` (逻辑分支): calc/impress/writer 剩余 `#ifdef` 均为编译机制类 (windows.h/FindWindow 宏 include, 3.3 E 表"可留")
- ✅ 平台差异安放: Windows 回归 4 个问题无一在会话层平台分支
- ✅ 变体点可枚举: SessionPlan 一眼看清两平台差异 (discover/form/fullscreen/settle_ms/ui_hide_needed/terminate_on_destroy)
- ✅ 构造性保证 (构建期): Windows 编译零错误
- ✅ 行为期保证: Windows conformance 探针五段全绿 (CALC/WRITER/IMPRESS/CORE-WORD/CORE-PPT, rc=0) + NovaPlayerDemo 全量回归
- ⚠️ 诚实边界: 无 Windows CI; FramePump 待 Windows 侧回归确认; 模板部署保障 (CopyFile.bat) 待打包流程加项 (3.1)

---

### 3.1 待办/讨论

> ★★★=立即;★★=中期;★=远期。

| 排序 | 事项 | 说明 |
|---|---|---|
| ★★ | **user 模板部署保障 (2026-08-18, 两平台)**: 模板 = 仓库 `templates/user/registrymodifications.xcu` (净化, 经验 40) → 部署 `office/program/templates/`。**Linux**: office_runtime POST_BUILD 自动拷贝 (构建时) ✓ 无需脚本; **Windows**: 不构建 office_runtime, **NovaPlayer 打包脚本 (CopyFile.bat 等) 需加 templates/ 拷贝项** —— 缺失时 WindowsPlatform::PrepareEnvironment seed 失败 → LO 默认 UI (2026-08-18 探针实测, 已手动部署当前环境) | 打包流程 |
| ★★ | **word 上层接入**(writerlink 底层就绪, 经验 38):NovaOfficeCore(LibreOfficeWriterManager 样板已保留, 恢复继承+override+构建配置)+ NovaPlayer(NP_WORD_PLAY_MODE_ANIMATION_LIBREOFFICE 枚举 + WordInstance 映射)+ Demo(Word 模式下拉框) | 功能就绪待接入 |
| ★ | **经验 42 阶段5 (可选)**: 平台层段内比对 (CaptureFrame 增量 unchanged 参数) + dedupe + calc zoom 维度 A/B; 开放问题 A (帧新鲜度 TTL) / B (calc tick 20ms 延迟接受度) 待 NovaPlayer 侧验证 | 远期优化 |
| ★★ | ffplay 能力增强(按需):XFrameGrabber 帧抓取/硬解/媒体信息 | 引擎底座就绪 |
| ★ | ffplay 引擎并发创建竞态(错开即好,LO 天然满足;紧邻创建场景需引擎内串行化) | 按需 |

### 3.2 功能/平台

| 排序 | 事项 | 说明 |
|---|---|---|
| ★★ | 2160p 混合分辨率落位产品化验证(默认配置已支持) | 配置验证 |
| ★★ | slot 管理策略(超限语义/动态轮替/最大并发数) | 策略决策 |
| ★ | win_platform seed 的窄字符 fs 调用 u2w 化(中文用户名路径风险, 与 md5 原问题同源, 2026-08-18 检视发现属遗留非新引入) | 平台补全 |
| ★ | 环境自检(字体/音频缺失明确报错)与崩溃检测告警 | 部署稳健性 |

---

### 3.3 平台隔离设计(意图/机制分离)— 已实施 2026-08-18 (J1-J4 全量)

> 背景: 双平台并行开发负担重。UNO 层大体一致(实证: writerlink 零平台层双平台可用),
> 桌面/窗口层本质分歧(共享内核+Xvfb+slot vs 独立进程+独立桌面)。**分歧不可消除,
> 但可以安放**。现状诊断(2026-08-18 统计): 平台分支倒挂——本应承载差异的平台层几乎
> 干净(xvfb_platform 0 处/win_platform 2 处), 本应平台无关的会话层躺着 28 处 `#ifdef`
> (calc 9 / impress 8 / writer 11)。目标: **会话层零 `#ifdef`**, 单平台开发者的
> 变更面物理上碰不到对端平台, 微妙细节各有唯一且被编译器守护的家。

#### A. 三原则

1. **隔离意图, 不隔离机制**: 接口抽象的是 what/when(协议与时序), 不抽象 how
   (XMoveWindow/SetWindowPos/CreateDesktopA/XShm)。统一"窗口 API"是伪泛型——
   最小公约数会强迫放弃各平台的 workaround, 那才是毁细节的方式。
2. **不变量构造性执行**: 跨平台禁令不靠注释记性, 靠代码结构让违规不可能
   (例: 核心层不持有窗口句柄 → "放映中不得 UNO setPosSize"物理上无处发生)。
3. **变体点可枚举**: 平台间自由度全部收进 plan 数据结构, review 时一眼看清
   两平台到底差在哪几维, 而不是在 28 处 `#ifdef` 里考古。

#### B. 关键澄清: 两类差异, 只隔离其中一类

- **calc vs impress 的差异 = 文档类型差异** → 允许留在各自会话文件(有无放映段)。
- **Windows vs Linux 的差异 = 平台差异** → 必须出会话文件, 进平台模块或 plan。
- 判据: 代码里出现平台名(`_WIN32`/`__linux__`)即是违例; 出现文档类型名是正常。

#### C. 会话协议规格(核心独占, 双平台同一份代码)

```
里程碑序列 (核心按此顺序执行, 平台工作绑定点由 plan 声明):

  P0  平台工厂 + PrepareEnvironment      (Linux: Acquire/Xvfb/slot; Win: DPI/桌面/profile seed)
  P1  BeginBoot (BootSection RAII)       — 意图: 并发 Create 引导+加载须串行(经验 5)
  P2  EnsureKernel → 空 ctx 则 BootstrapSession (calc/impress 均已此形态; writer 走 KernelHost)
  P3  SnapshotWindows + Hidden 加载          — 意图: 引导+加载须串行(经验 5)
  (BootSection::Release 不在 P3: 见 P5 后注*; 提前释放会 reintroduce 经验 5)
  *Release 绑定点 = 当前 calc_session.cpp Create() 内 setVisible(P5) 之后 (boot_section->Release(), 经验 5)。setVisible(P5)
   触碰共享内核须串行, Release 须在 P5 之后、首个 discover 之前调用; 提前到
   P3/SnapshotWindows 之后释放 = 并发 Create 卡死(经验 5)。core 显式调用此点。
  P4  [W@BeforeReveal]                   ← plan 绑定点 (calc/Win 在此发现+定型, 见 F 用例)
  P5  setVisible 显露                     — VCL 窗口在此时按最终形态创建
  P6  [W@AfterReveal]                    ← plan 绑定点 (calc/Linux + impress/Linux 发现)
  P7  (仅 impress) 放映属性(IsFullScreen=plan) + start + 等 settle_ms + controller+pause
  P8  [W@AfterStart]                     ← plan 绑定点 (impress/Linux slot 落位; impress/Win 发现放映窗口)
  P9  HideUiBlock (plan.ui_hide_needed 门控)
  P10 UpdateFrame 首帧
```

- 里程碑本身是**共享意图**(顺序即踩坑结论); 每个绑定点是**平台机制**的自由。
- 失败语义显式: DiscoverWindow/FormWindow 返回 bool, 失败即 Create 失败, 无歧义。

#### D. LinkPlatform 接口定稿形态

```cpp
// 窗口工作绑定点 (相对核心里程碑; 平台声明白己的窗口工作发生处)
enum class WindowPoint { None, BeforeReveal, AfterReveal, AfterStart };

// 平台策略声明 (数据, 非代码): 启动时一次性取, 核心原样消费并打日志
struct SessionPlan {
    WindowPoint discover;        // 窗口发现绑定点
    WindowPoint form;            // 窗口定型(落位/样式)绑定点; None = LO 自管(全屏)
    bool  fullscreen;            // 放映 IsFullScreen (Linux false=窗口化+slot, 经验 1)
    int   settle_ms;             // start 后形态稳定等待 (Win 实测 1200 不够须 2500)
    bool  ui_hide_needed;        // 需 HideUiBlock (setMenuBar 消除1px + hideElement 冗余兜底)
    bool  terminate_on_destroy;  // 每 session 独立进程才 true
};

// 引导段 RAII: 构造 = 进入串行区, Release() = 核心在 setVisible(P5) 之后显式调用
// (早释点本身是协议: "窗口查找可并行", 经验 5; 须等于 Create() 内 setVisible(P5) 之后,
//  不得提前到 P3/SnapshotWindows 之后 —— 否则 reintroduce 经验 5 并发崩溃)
class BootSection { virtual void Release() = 0; ... };  // Linux 真锁 / Win 空实现

class LinkPlatform {
    // 既有: PrepareEnvironment/EnsureKernel/SnapshotWindows/SetWindowSize/
    //       CaptureFrame/HideUiFloats/Cleanup (不动)
    virtual SessionPlan Plan() = 0;
    virtual std::unique_ptr<BootSection> BeginBoot() = 0;
    // 契约样例 (接口注释写时机与不变量, 平台实现者读合同不读对端代码):
    // DiscoverWindow: 在 plan.discover 绑定点被调; 须已 SnapshotWindows。
    // FormWindow: 在 plan.form 绑定点被调; **放映运行中的窗口几何操作只允许
    //   发生在此实现内** (UNO setPosSize 运行中黑屏, 经验 26); 允许在隐藏态执行
    //   (Win 改 style+SetWindowPos 于 setVisible 前定型)。
    virtual bool DiscoverWindow() = 0;
    virtual bool FormWindow(int w, int h) = 0;   // 吸收 SizeWindowToSlot
    virtual void ApplyNativeFullscreen() = 0;    // 能力钩子, 默认空 (Win 快捷键注入)
    virtual void OnSessionEnd() = 0;             // 能力钩子 (Win terminate; Linux 空)
};
```

- 核心代码形态: `if (plan.discover == WindowPoint::AfterReveal) DiscoverWindow();`
  —— 分支条件是 plan 数据, 不是平台名; 两平台读同一条代码路径。
- 备选方案(全量里程碑回调 `OnMilestone(m)`, 核心零分支)被否: 契约含糊、
  失败语义弱(FindWindow 失败须中止 Create), 可枚举 plan 的显式性更值钱。

#### E. 变体点总账(现存每处 `#ifdef` 的归宿)

| 现存位置 | 内容 | 归宿 |
|---|---|---|
| impress.cpp:39 / calc.cpp:58 / writer.cpp:17 | include office_runtime | 平台实现文件内(机制) |
| impress.cpp:77 / calc.cpp:366 / writer.cpp:592 | destroy 时 terminate | `plan.terminate_on_destroy` + OnSessionEnd |
| impress.cpp:117 / calc.cpp:221 / writer.cpp:117+190 | BootLock+Unlock | BeginBoot RAII + Release(释放点=协议 P3) |
| impress.cpp:166-175 | 解锁+start 前找窗 | `plan.discover=AfterReveal`(impress/Linux) |
| impress.cpp:188-211 | IsFullScreen 平台分支 | `plan.fullscreen` |
| impress.cpp:219 | settle 2500ms | `plan.settle_ms` |
| impress.cpp:277-289 | start 后 slot 落位 | `plan.form=AfterStart` + FormWindow |
| impress.cpp:321-333 | Win 窗口化 A/B 兜底 | `plan.ui_hide_needed`;ORT_IMPRESS_FULLSCREEN 逃生门留在 Win 平台内 |
| calc.cpp:302-309(Win) | reveal 前+找窗+落位+快捷键 | `discover=form=BeforeReveal` + FormWindow 内含快捷键(见 F) |
| calc.cpp:317-327(Linux) | reveal 后找窗+落位 | `discover=form=AfterReveal` |
| writer.cpp:35-82 | u2w/to_path/进程 ID | link_utils 机制层(**to_path 应上收 link_utils 三链共用**) |
| writer.cpp:117-134+606 | Acquire/EnsureKernel/Release | 见 G(writer 引导缝) |
| calc.cpp:33-45 | windows.h/FindWindow 宏 | 编译机制, 可留(或 os 头收拢) |
| link_utils.cpp:18/38/75 | GetLinkDir/BootstrapSession/u2w 双实现 | 本职(它就是机制的家), 不动 |

#### F. 最微妙用例: calc 的 Windows 反序(设计容纳力的试金石)

Windows calc **先找窗+定型再 setVisible**(VCL 在 setVisible 时按最终形态创建窗口,
反序则 menubar 隐藏失效,demo 实测);Linux **先 setVisible 再找窗+落位**。设计下:

- Win plan: `discover=form=BeforeReveal`;Linux plan: `discover=form=AfterReveal`。
- 核心只在 P4/P5/P6 按各自 plan 调 DiscoverWindow/FormWindow, **同一条代码**。
- "定型必须在显露前"这条 Win 局部知识, 写在 win_platform 的 Plan() 返回处与
  FormWindow 实现注释里, 连同 demo 实测记录——Linux 开发者永远不需要知道它。

#### G. writer 的引导缝(可选, 最后做)

writer 无平台层是定案(经验 38④), 其 Linux 分支(Acquire/BootLock/EnsureKernel/
Release)是同一"引导+串行+生命周期"缝。两个选项:
- **G1(推荐)**: 抽 `link_utils::KernelHost` 三函数(BeginBoot/ObtainCtx/EndSession),
  双平台各一个编译单元文件;writer 会话零 `#ifdef`, 不引入 LinkPlatform。
- G2: 维持现状 4 处分支(少而稳定, 承认不完美)。
- 不选: 给 writer 强加 LinkPlatform(违反无平台层定案)/HeadlessPlatform(过度设计)。

#### H. 知识安居铁律 + 映射

每条踩坑结论必须落在且只落在两处之一, 不允许第三处(现状的会话 `#ifdef` 是第三处):

| 经验 | 家 |
|---|---|
| 1(全屏盖大屏→窗口化+slot)/13(XShm 直拷)/14(屏高 BadMatch)/15(坐标上限) | xvfb_platform.cpp 内部 |
| 5(引导串行+窗口查找可并行) | BeginBoot/Release 契约 + 核心在 setVisible(P5) 之后调用 Release |
| 22/23/27(bootstrap/profile 隔离) | EnsureKernel/BootstrapSession 契约(意图)+平台实现(机制) |
| 26(放映中 UNO 几何黑屏) | FormWindow 契约 + 核心不持窗口句柄(构造性) |
| 38④(writer 无平台层) | G 缝选择 |
| 41(paused_ 重置在入口函数) | FramePump::Start 契约 (泵内无条件 paused_=false, 经验 42 承接) |
| Win settle 2500ms / 反序定型 / 1.5s 形态稳定 | win_platform.cpp 内部 + plan 数据 |

#### I. 保证机制(三层)与诚实边界

- **构建期(构造性)**: 核心零 `#ifdef` → 改核心语法上不可能破坏平台代码;改接口则
  未适配平台**编译响亮失败**——失败点即唯一耦合点。
- **行为期**: 双平台同脚本 conformance 探针(create→start→帧→翻页→destroy);
  Linux 已有(impress_nextpage/media_green 等), Windows impress 落地时补等价物。
- **知识期**: 经验编号锚定平台文件注释(上表), 细节搬家注释随行。
- **边界**: 协议本身变更(新增里程碑)仍是双平台共同决策——以接口变更形态出现在
  review, 响亮可见;无 Windows CI 前, "保证"上限 = 构造性防护 + 纪律。
  C ABI 与 UNO 语义是共同资产, 动它们仍需对端编译确认。

#### J. 迁移路径(每步可独立验证, 任意步后可停)

1. **impress 先行**(收益最大: Windows 实现还是 stub, 先定缝后落地, 零返工):
   LinkPlatform +SessionPlan/BeginBoot/DiscoverWindow/FormWindow/OnSessionEnd,
   impress 会话清 `#ifdef`, Linux 全链探针回归。
2. Windows impress 平台按新接口落地(契约即规格书), conformance 探针补齐。
3. calc 跟进(含 F 反序用例), 回归 impress_multi/media_green。
4. writer 可选: to_path 上收 link_utils(独立小步, 随时可做);G 缝按 G1/G2 决策。

#### K. 反模式清单(明确不做)

- 统一 X11/Win32 "窗口 API"(伪泛型, 最小公约数毁 workaround)
- 按平台拆仓库(单树+目录隔离足够)
- 为 writer 强加平台层 / 模板基类魔法(FramePump 是经验 42 的事, 不混入本设计)

---

### 已关闭事项

**2026-08-19:**
- **UI 隐藏专项**(4.2): InputLineVisible dispatch 下沉至 HideUiExtras (平台隔离); 6 次探针实验闭合验证 setMenuBar 消除 impress 1px 底边框; HANDOFF.md 认知错位修正 (FullScreen 从未生效等)
- **FramePoller 阶段0**(经验 42): calc Start 补持锁+重置 paused_ (修 P1/P5); calc UpdateFrame 改锁内直推 (修 P6); impress UpdateFrame 加 mu_ 防并发 (临时防 P3); 单测 50/50 全绿
- **FramePoller 阶段1**(经验 42): FramePump 组件 + 单测落地 (common/frame_pump.h/.cpp + frame_pump_test.cpp); 单测 14/14 全绿
- **FramePoller 阶段2**(经验 42): impress 接入 FramePump (tick=40/heartbeat=0/backoff=200, 等价原 PollThread); 清理遗留 StartPoller/StopPoller/PollThread + NextPage 日志残留; impress 补齐 HideUiExtras 调用 (平台隔离两层契约, 之前漏调); demo 回归: pptx 放映帧正常 + 1px 依旧消失
- **FramePump Start 契约回归修复**(经验 41/42 P1): impress demo "暂停→恢复无法翻页"复现经验 41 路径(resume 走 Start)。根因: FramePump::Start() 幂等早返未重置 paused_, 违反契约"Start=任何状态→Running 未暂停"(见经验 42 详述契约表)。修复: Start() 持 ctrl_mutex_ 内无条件 `paused_=false` 再判幂等; 新增测试 9 (start_resets_paused_when_running) 闭环; 单测 15/15 全绿
- **FramePoller 阶段3**(经验 42): writer 接入 FramePump。ChangeFn=force_frame_.exchange(脏位 probe), FrameFn=PushFrame 持 mu_ 访问 page_cache_ (frame_mutex_→mu_ 锁序, 无反向); tick=5/heartbeat=100/hbp=false (Pause 冻结, 与原 PollThread `!paused_&&heartbeat_due` 一致)/backoff=200。删除 PollThread/StartPoller/StopPoller。38⑦ 语义(停止后取帧黑屏)由泵全状态 UpdateFrame 承接
- **FramePoller 阶段4**(经验 42): calc 接入 FramePump。ChangeFn=CheckViewportChanged(持 mu_, 视口签名 row/col/sheet + force_frame_ 脏位, 原 PollThread 内联逻辑提取为方法); tick=20(放宽原 5ms full-speed, 性能预算 50 唤醒/s)/heartbeat=100/hbp=true(Pause 照推, 与原 PollThread 心跳无 paused_ 门控一致)/backoff=200。Create 末尾加首帧 UpdateFrame。P1(Start 不重置 paused_)/P5(双 Start 竞态)/P6(停止后取帧黑屏) 均由 FramePump 契约承接。zoom 维度仍缺(靠心跳兜底, 待 A/B)
- **三链 FramePump 接入收官**(经验 42 阶段2-4): impress/writer/calc 均已接入统一帧泵, 删除所有 per-session poll_thread_/paused_/force_frame_ 重复实现; 全量构建通过, 单测 15/15 全绿

**2026-08-18:**
- 诊断日志清理(日志体系统一: 前缀/级别/单入口)
- 代码重构(link_utils 工具整合/DEFER/UNO_GUARD/异常日志补全/SYS_gettid 可移植)
- **Impress 暂停→恢复翻页失效**(经验 41, 实测修复)
- **FramePoller 共性分析**(经验 42, 当日待实施; 2026-08-19 阶段0-4 全部落地)
- **UI 隐藏收官**(经验 40⑦-⑨): sidebar/statusbar 模板条目补齐(66→69)+ 部署副本同步(踩部署陈旧坑), demo 肉眼验收全部隐藏; 重构检视+全量重建+单测 50/50+探针回归全绿
- **平台隔离骨架落地(impress)+ BootLock 死锁修复**(3.0/3.3/经验 43): 会话层 `#ifdef` 清零, P0-P10 协议化, 探针复绿
- **平台隔离设计全量实施**(3.3 J1-J4): J2 Windows impress 新接口落地(Plan/BeginBoot/DiscoverWindow/FormWindow/ApplyNativeFullscreen/OnSessionEnd, calc/impress 策略按 profile_subdir 数据化); J3 calc_session 重构(P0-P10 协议化, 8 处 `#ifdef` → plan 数据驱动, F 反序定型用例, terminate 按 plan_.terminate_on_destroy 门控); J4 writer G 缝(link_utils::KernelHost 引导缝封装 + to_path 上收, writer 会话引导缝 `#ifdef` 清零); **Linux demo 回归通过**(修复 xvfb_platform Plan() 写死 impress 策略 bug: calc form=AfterReveal/impress form=AfterStart, 2 xlsx 黑屏消失); 日志前缀标准化([Common]→[Common.Boot], [CAPTURE]→[Common.WinWindow]); **Windows 侧回归完成 (2026-08-19, 见 1.6)**

**2026-08-17:**
- LO 改动同步远端(commit 83e0b9c3e)
- ffplay 多实例并行播放(经验 37)
- office_runtime 防御增强与单测加固(经验 35)
- **废弃 source/ 旧方案**(零实例化实证后全平台清理, 1.6)
- writer UpdateFrame 语义修复(经验 38⑦)
- **writerlink 底层链路闭环**(经验 38)

**2026-08-14:**
- gstreamer 路径清理

---

## 四、平台隔离专项 [设计+经验] (updated 2026-08-19)

> 目标: 消除双平台开发的串扰风险, 让任何平台的调优/回归不影响其他平台。
> 3.3 是设计 (意图/机制分离), 本章节是专项治理记录 (盲区发现 + 修补落地)。

### 4.1 隔离边界总账

| 层 | 隔离状态 | 范式 | 说明 |
|----|---------|------|------|
| 窗口发现/定型 (DiscoverWindow/FormWindow) | ✅ 已隔离 | SessionPlan 数据驱动 | 3.3 J1-J3 |
| 终止策略 (terminate_on_destroy) | ✅ 已隔离 | SessionPlan 数据驱动 | 3.3 J3 |
| 引导段 (BeginBoot/Release) | ✅ 已隔离 | BootSection RAII | 3.3 J1-J2 |
| 全屏浮窗 (HideUiFloats) | ✅ 已隔离 | LinkPlatform 虚函数 | Linux 空 / Win 原生 API |
| **UI 修补 (HideUiExtras)** | ✅ 已隔离 (2026-08-19) | LinkPlatform 虚函数 | 见 4.2 |
| 模板 (registrymodifications.xcu) | ⚠️ 共享 | 真相源单一份 | 见 4.3 (待评估) |

### 4.2 子项1: UI 隐藏隔离 (2026-08-19 已落地)

#### 问题

Windows 回归 (85aae31f..HEAD) 在 calc_session.cpp 共享层新增 `InputLineVisible` dispatch
(公式栏隐藏), Linux demo 出现 menubar + 公式栏显示 (此前已通过)。

#### 根因 (4 组对照实验闭合)

| 状态 | 模板 | 代码 | menubar | toolbar | 公式栏 |
|------|------|------|---------|---------|--------|
| 85aae31f (回归通过) | 69 item | 旧 (无 InputLineVisible) | 隐藏 | 隐藏 | 隐藏 |
| HEAD baseline | 126 item | 新 (含 InputLineVisible) | 显示 | 隐藏 | 显示 |
| 实验1 (模板回退) | 69 item | 新 | 显示 | 显示 | 显示 |
| 实验2 (代码回退) | 126 item | 旧 | 隐藏 | 隐藏 | 隐藏 |

结论:
- **InputLineVisible dispatch 是唯一破坏源** (实验1 vs 实验2, 模板无关)
- **[置信度: 高, debug 日志实证]** LO 在 Xvfb 窗口化模式下 UI 元素默认 vis=0
  (不显示), HideUiBlock 的 hideElement 是对已隐藏元素的冗余兜底 (非主要机制)。
  **setMenuBar(null) 非冗余**——消除 impress 1px 底边框 (见下文"UI 隐藏机制实证")
- **[置信度: 高, debug 日志实证]** HideUiBlock 内的 `.uno:FullScreen` dispatch
  在 calc/Linux 下 dispatcher NOT found, 从未生效。此前"公式栏靠 FullScreen
  全屏态自管隐藏"的认知错误
- **[置信度: 中, 推断]** InputLineVisible dispatch 在 HideUiBlock 之后执行,
  触发 LO UI 重建, 破坏了 vis=0 初始态, 导致 menubar/公式栏重新显示。
  模板的 CalcWindowState 条目 (Visible=false) 是"破坏后的兜底", 仅在新代码
  下起作用 (实验1 vs HEAD baseline 的 toolbar 差异证实)

#### 根本缺陷

平台相关的 UI 修补 (InputLineVisible dispatch) 被放在共享层 (calc_session.cpp),
其副作用平台相关 (Linux 共享内核破坏 vis=0 初始态, Windows 独立进程不破坏)。
隔离设计只覆盖"平台机制"层, UI 隐藏逻辑被误当作平台无关。

#### 解决方案: HideUiExtras 下沉

遵循 HideUiFloats 已建立的范式, 新增 `LinkPlatform::HideUiExtras(frame, factory, ctx)`:

| 平台 | 实现 | 行为 |
|------|------|------|
| Linux (XvfbSessionPlatform) | 空操作 | LO Xvfb 无头环境 UI 默认 vis=0, 无需平台修补 |
| Windows (WindowsPlatform) | InputLineVisible dispatch | 搬迁自 calc_session.cpp, 行为不变 |

会话层改动:
```cpp
// calc_session.cpp P9 后
platform_->HideUiExtras(frame_, factory, ctx_);  // 平台自决
```

#### 改动清单

- `common/link_platform.h`: 新增 HideUiExtras 虚函数 + UNO include
- `common/linux/xvfb_platform.h`: HideUiExtras 空实现 override
- `common/windows/win_platform.h`: HideUiExtras 声明 override
- `common/windows/win_platform.cpp`: HideUiExtras 实现 (InputLineVisible dispatch 搬入)
- `calc/calc_session.cpp`: InputLineVisible dispatch 块 → platform_->HideUiExtras()

#### 验证

- **Linux**: 2 xlsx + 1 pptx demo, UI 全部干净 (menubar/toolbar/公式栏 隐藏) ✓
- **Linux impress**: 2 pptx + 1 xlsx demo (ORT_LOG_LEVEL=debug), UI 全部干净 ✓
- **Windows**: 行为不变 (InputLineVisible dispatch 逻辑原样搬迁, 仅日志前缀改) — 待 Windows 侧回归确认
- **隔离保证**: Linux 改 HideUiExtras 实现 (空) 不影响 Windows; Windows 改 HideUiExtras 实现不影响 Linux

#### UI 隐藏机制实证 (2026-08-19 debug 日志, 置信度: 高)

`ORT_LOG_LEVEL=debug` 跑 calc (2 xlsx) + impress (2 pptx) 验证 HideUiBlock 内部行为:

**[置信度: 高, debug 日志实证]** LO 在 Xvfb 窗口化模式下 UI 元素默认 vis=0 (不显示):
- calc/impress 的 state[before] 全部 vis=0, state[after] 全部 vis=0
- menubar/toolbar_std/toolbar_fmt/toolbar_draw/statusbar/sidebar/sidebar_props 均如此
- HideUiBlock 的 hideElement 是对已隐藏元素的冗余兜底 (非主要机制)

**[置信度: 高, debug 日志实证]** HideUiBlock 内 `.uno:FullScreen` dispatch 在 calc/Linux
和 impress/Linux 下均 `dispatcher NOT found`, 从未生效。此前"FullScreen 全屏态自管隐藏 UI"
的认知错误。FullScreen dispatch 在 Xvfb 无头环境下是死代码 (desktop_ provider 找不到此 dispatch)。

**[置信度: 高, 探针实证]** HideUiBlock 内 setMenuBar(null) 非冗余——消除 impress 1px 底边框:
- 6 次探针实验 (ui_1px_probe + 像素分析) 闭合验证:
  - V1 (无 HideUiBlock): impress row[-1] = (0,0,0) 纯黑, 1px 边框出现
  - 步骤1 (完整 HideUiBlock): 1px 消失
  - 复现 (无 HideUiBlock): 1px 再次出现
  - V2 (无 HideUiBlock + 2500ms 独立 sleep): 1px 仍出现 → sleep 时序无关
  - V3a (只 setMenuBar, 无 hideElement): 1px 消失 → setMenuBar 是消除 1px 的子动作
- 机制: setMenuBar(null) 移除 LO 窗口的 menubar 容器, 触发窗口重绘/布局调整,
  消除初始化过渡期的 1px 底边框
- hideElement 和 sleep 对 1px 无效 (V2 + V3a 间接证明)

**[置信度: 高, debug 日志实证]** 多文档并发无竞态:
- 2 calc + 2 impress 交错执行 HideUiBlock, 各 frame 的 setMenuBar(null)/hideElement 互不影响
- frame 隔离: 各会话 container pos/size 不同 (不同 slot), setMenuBar 对各自 frame 操作
- frame_active 状态可能不同 (后创建的 frame 被激活), 但 UI 元素 vis 一致 (全 0)

**[置信度: 中, 推断]** LO Xvfb 无头环境 UI 默认 vis=0 的原因:
- 推测 LO 在无头/无桌面环境下不构建 UI 元素 (VCL 后端不渲染)
- HideUiBlock 的 hideElement 是为有头环境 (Windows 独立桌面) 准备的防御性代码
- setMenuBar 消除 1px 的间接效果可能在 Windows 下也有效 (待 Windows 侧验证)
- 此推断无法在 Linux 侧验证 (Linux 只用 Xvfb), 需 Windows 侧 debug 日志确认

### 4.3 待评估子项

#### 4.3.1 模板隔离

当前模板 (registrymodifications.xcu) 是共享真相源 (NovaPlayerTools/templates/user/),
双平台共用一份。实验2 证明模板在旧代码 (无 InputLineVisible) 下无影响 (LO Xvfb 默认
vis=0, 模板条目被覆盖), 但在新代码 (含 InputLineVisible, 触发 UI 重建) 下起兜底作用。

评估点:
- 是否需要平台分叉 (templates/linux/ vs templates/windows/)?
- 或保持共享但明确"模板是共享的, 改动需双平台验证"?
- 当前结论: 保持共享, 纳入"共享层改动需双平台回归"规则 (见 4.4)

#### 4.3.2 隔离契约文档化

3.3 已有 LinkPlatform 接口定稿形态 (D), 需补全:
- HideUiExtras 接口契约
- 隔离边界总账 (4.1 表格) 的维护规则
- "哪些改动是平台安全的 (只改平台实现), 哪些是双平台共享的 (改共享层需双平台验证)"

### 4.4 隔离回归规则

| 改动位置 | 回归范围 | 示例 |
|---------|---------|------|
| 平台层 (common/linux/, common/windows/) | 单平台 | xvfb_platform.cpp / win_platform.cpp |
| 共享层 (calc_session.cpp, link_utils.cpp, impress_session.cpp) | **双平台** | 任何会话逻辑改动 |
| 共享模板 (templates/user/) | **双平台** | registrymodifications.xcu |
| 平台接口 (link_platform.h) | **双平台** | 新增/修改虚函数 |

### 4.5 反模式 (不做)

- 在共享层调用平台专属 dispatch (如 InputLineVisible 是 calc/Windows 专属, 不应在 calc_session.cpp)
- 假设"平台机制隔离了 = UI 隔离了" (UI 隐藏副作用是平台相关的)
- 为统一而统一 (Linux 不需要 InputLineVisible, 不应为了"对齐"而在 Linux 也调用)

---

## 五、帧泵专项 [设计] (updated 2026-08-19)

> 对应经验 42。契约/缺陷映射/三链形态/迁移路径见 [经验 42 详述](#经验-42-详述);
> 本章收录设计决策论证、性能预算、测试矩阵等深度内容 (源自原始设计稿, 已落地)。
> FramePump 是会话基础设施 (common, 与 link_utils 平级), 与 3.3 平台隔离设计正交。

### 5.1 设计决策

#### 决策一: frame_mutex_ 串行化, 否决"单线程委托"

泵内一把 `frame_mutex_`: 所有 FrameFn 执行 (泵 tick 与 UpdateFrame 调用方) 都在这把锁内, 调用方线程就地执行, 不向泵线程投递。

- **否决的替代方案**: UpdateFrame 发请求→唤醒泵线程执行→等待完成。否决理由: (a) 引入唤醒延迟与请求/应答机制; (b) Stopped 态仍须回退为调用方就地执行——最终还是两个执行上下文, 规则反而变复杂; (c) mutex 模型对所有状态只有一条规则。
- **效果**: P3 构造性消灭 (两处执行不可能并发); XShm/cap_bgra_ 单线程语义保持。
- **成本**: UpdateFrame 与 tick 短暂互斥, 最坏等待一帧抓取时间 (~1-2ms 1080p), 可忽略。

#### 决策二: 锁纪律与锁序 (修 P4, 防新死锁)

1. 调用泵方法 (含 UpdateFrame) 时**不得持有会话锁 mu_**。会话的 UpdateFrame 实现退化为 `return pump_->UpdateFrame();`。
2. FrameFn/ChangeFn 内部**自取所需的短会话锁** (writer 读页缓存时), 抓帧与 cb_ 投递**绝不在 mu_ 内**。
3. 全局锁序: `frame_mutex_ → mu_(短)`, 严禁反向。probe 的 UNO 调用不持 mu_ (pane_/view_ 引用在泵运行期稳定, Destroy 先 Stop 保证)。

#### 决策三: 静止检测两阶段 (无损优先, 阶段5 可选)

**原则: 无损耗优先于省电**——只允许跳过"字节级完全相同"的帧, 任何基于状态预判的方案都可能在三处盲区漏帧, 一律否决:
- impress 内部动画无 UNO 状态接口
- ffplay 媒体画面在 LO 渲染管线之外
- CPU 转场逐帧变化

否决的具体方案: XDamage (ffplay 独立顶层窗口不触发 LO 窗口的 damage → 视频漏帧)、采样签名 (小变化落抽样间隙 = 假阴性 = 漏帧)、UNO 动画状态查询 (无接口)。

- **阶段一 (纯会话层, 零平台改动)**: FrameFn 抓帧 (memcpy 后) 对上一已推帧做全量 memcmp (早退): 相同 → 只更新"末帧时间"不计推帧, 跳过 cb_ 及下游; 不同 → 拷贝/投递/缓冲互换。软件渲染确定性保证同输入同输出, 无假阳性抖动。成本: 动画/视频期 memcmp 首差异行早退 ≈ 0; 静止期全量扫描 8.3MB ≈ 0.3-1ms。收益: 静止期下游管线完全静默; 上游拷贝照付。
- **阶段二 (可选优化, 触碰平台接口)**: LinkPlatform 增量扩展 `CaptureFrame(..., bool& unchanged)` —— GrabBgra 在 XShm 段内与上帧先比对后拷贝 (XShmGetImage 服务端拷贝 ~0.01ms 级, 见经验 13, 保留; 省掉的是 8.3MB 应用层拷贝+alpha 填充)。属 3.3 平台隔离设计的接口增量, 走其"改接口=双平台共同决策"流程。
- **契约联动 (开放问题 A)**: 若消费方存在"帧新鲜度 TTL"(超时视为无帧), 心跳必须保留 (推相同帧, 但阶段一已把内部拷贝省掉, 心跳近零成本); 若消费方是末帧语义, 静止期可完全静默, 208MB/s → 趋近 0。两阶段设计使该问题可延后决策且无论答案都不返工。

#### 决策四: tick 循环合并探测与心跳

每 tick (condvar 计时唤醒, Stop 可立即打断等待):

```
tick:
  if (!running) continue
  paused 且 !heartbeat_when_paused → continue
  lock frame_mutex_:
    changed = changed_fn ? changed_fn() : true     // impress 无 probe = 恒真
    due = (now - last_push >= heartbeat_ms)        // heartbeat_ms=0 则恒 false
    if (changed || due):
        if (frame_fn()) { last_push = now } else { 退避 fail_backoff_ms }
```

calc 的 UNO 视口查询从"每 5ms 无条件"变为"每 tick 一次、心跳到期也复用同一帧", 唤醒 200→50 次/秒, 空转 IPC 200→50 次/秒 (tick=20ms; 开放问题 B 可调 10ms)。

### 5.2 性能预算 (1080p BGRA ≈ 8.3MB/帧)

| 场景 | 现状 (per-session poller) | FramePump 后 |
|---|---|---|
| impress 静止页 | 25fps 全量: 拷贝+alpha+回调+下游 ≈208MB/s | 阶段一: 服务端拷贝+memcmp(~0.5ms/tick), 下游 0; 阶段二: 趋近 0 |
| impress 动画/视频 | 25fps 全量 (必要) | 不变 (memcmp 早退 ≈0) |
| calc 空转 | 200 唤醒/s + 200 UNO IPC/s + 83MB/s 心跳 | 50 唤醒/s + 50 IPC/s + 心跳近零成本 (dedupe) |
| writer 空转 | 200 唤醒/s + 83MB/s 心跳 | 200 唤醒/s (tick=5 未放宽) + 心跳近零成本 (dedupe) |

注: writer tick 落地为 5ms (保守, 与原 PollThread 一致), 未按设计稿 20ms 放宽; dedupe (阶段一) 尚未实施, "心跳近零成本"为阶段一落地后的预期。

### 5.3 测试矩阵

- **单元测试** (`common/frame_pump_test.cpp`, 树内编译不部署, 沿 office_runtime_test 模式): 9 个测试 15 checks — Start 幂等/重置、Stop 排空、Pause 冻结与 UpdateFrame 可用、UpdateFrame 与 tick 串行 (序列断言)、心跳间隔 (1ms tick 加速)、失败退避、Stop/Start 重启、ChangeFn 探测、Start 重置 paused_ 回归 (经验 41 同构)。
- **探针回归**: impress (impress_nextpage / media_green 双态 / impress_multi 并发)、writer (writer_probe 翻页/停止后取帧段)、calc (滚动/切表/缩放 + impress_multi 混跑观察 IPC 竞争)。
- **A/B 性能探针** (开放 B 用): calc 滚动在 tick 5/10/20ms 三档视觉对比。

### 5.4 与平台隔离设计 (3.3) 的关系

正交互补: FramePump 是会话基础设施 (common, 与 link_utils 平级), 不触碰 LinkPlatform 语义; 唯一交点 = 阶段二的 `CaptureFrame(+unchanged)` 增量, 走 3.3 的接口变更流程 (改接口=双平台共同决策)。落地顺序无依赖, 可并行推进。

### 5.5 开放问题 (不影响阶段0-4, 可延后)

- **A. 帧新鲜度 TTL**: 消费方 (取帧链) 是否存在"末帧超时视为无帧"? 决定心跳保留 (近零成本) or 静止静默 (收益最大); 以及 hbp 三链统一值。验证手段: NovaPlayer PlayerItem.getVideoFrame 侧读帧逻辑确认。无论答案如何, 阶段0-4 不需要该答案 (dedupe 无内容风险, 心跳保留现状)。
- **B. calc tick 20ms 滚动延迟接受度** (最坏 +15ms): 接受 / 改 10ms (+5ms) / A/B 探针实测后定。默认 20ms, plan 一处可改。

---

## 六、FFplay 嵌入专项 [经验+设计] (updated 2026-08-20)

> ffplay 嵌入引擎 (office_runtime/ffplay, 补丁式复用 FFmpeg ffplay.c) 的尺寸链治理。
> 关联经验 34 (补丁式复用)、19b (软件渲染/Xvfb 无 GPU)、37 (并发创建竞态)。
> 探针: xvfb_calc_demo/ffplay_window_size_probe.cpp (engine 组, 直接验证引擎)。

### 6.1 video_open 尺寸修复 — 已闭环 (2026-08-19)

> 修复后无回归。详细尺寸链/探针验证见 git 历史 commit + ffplay_window_size_probe.cpp。

**问题**: ffplay_embed.c `video_open()` 用 `default_width/height` (640x480) 设置 `is->width/is->height`, 嵌入模式下与外部 X11 窗口实际尺寸无关, 视频位置/缩放错误。

**尺寸链** (LO → ffplay): mediawindow_impl.cxx Resize → createPlayerWindow aArgs[0]=parent / aArgs[1]=rect → ffplay_player.cxx 解析 (rect 被忽略) → ffplay_engine_create → SDL_CreateWindowFrom → video_open `is->width=default_width=640` ← **bug**。

**修复 (方案A)**: video_open renderer 创建后加 `SDL_GetWindowSize(is->window, &w, &h)` 替代 640x480 硬编码。技术依据: SDL X11 驱动 `X11_CreateWindowFrom → SetupWindowData → XGetWindowAttributes` 设置 window->w/h (SDL2 源码实证)。修改文件: ffplay_embed.c (video_open) + ffplay_embed.patch 回填 (经验 34)。

**验证**: ffplay_window_size_probe (engine 组, 三方对比 期望/X11/ffplay, 500x300 故意 ≠ 640x480)。修复后 PASS: 三方一致。Demo 含视频 pptx 视频尺寸正确。

**开放问题 (升级路径)**: 方案A 仅首次创建, 运行时 X11 resize 不感知 (放映期尺寸固定为常见场景)。升级路径: 方案B (`ffplay_engine_set_window_size` API + `PlayerWindowShell::setPosSize` 调用) / 方案C (SDL_WINDOWEVENT_RESIZED 事件监听)。

### 6.5 关联索引 (六、FFplay 嵌入专项)

**关联经验**: 17/18/19 (gst → ffplay 替代链) / 28/29/30 (ffplay 注入 SDK 模式 + ORT_MEDIA_BACKEND) / 34 (补丁式复用回填纪律) / 37 (多实例并行播放)
**关联待办**: 3.1 ffplay 能力增强 (XFrameGrabber/硬解/媒体信息) / ffplay 引擎并发创建竞态
**已闭环**: 2026-08-19 video_open 尺寸修复 (6.1) / 2026-08-19 多实例根治 (6.6) / 2026-08-19 静音专项 (6.7) / 2026-08-20 日志专项 (6.8)
**探针**: ffplay_window_size_probe / ffplay_engine_probe / ffplay_inject_probe / media_green_probe
**项目级上下文**: 1.6 媒体后端 / 1.6 GL 全禁用 / 1.6 诊断开关

### 6.6 多实例根治 (2026-08-19 落地)

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

### 6.7 静音专项 (2026-08-19 落地, C1 简化版)

> PptX 单 slide 多媒体 shape 各起一 XPlayer 实例, LO UNO 无 setMuteAll 接口,
> 需上层 (LibreOfficeImpressManager::SetMute override) 跨进程触发 soffice 子进程内
> ffplay.so 全局静音。本节记录方案演进与最终落地的 C1 简化版。

#### 6.7.1 方案演进

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

#### 6.7.2 触发链路 (C1 简化版)

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

#### 6.7.3 关键设计点

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

#### 6.7.4 探针可行性验证 (ffplay_inject_probe 扩展)

**早期 6 层探针路径** (源自 ppt-mute.md 整合, 2026-08-19; Phase 2 落地前的诊断 SOP):

| 层 | 探针/方法 | 结论 |
|---|---|---|
| 1. ffplay 引擎层 | ffplay_mute_probe (engine 组) — set_volume/get_volume 直调 | ✅ 引擎层 set_volume(0) 设 audio_volume=0,SDL 混音器输出静音;静音后 media_time 仍推进,解码正常 |
| 2. FfplayPlayer 映射 | 代码审查 ffplay_player.cxx setMute/setVolumeDB/isMute | ✅ setMute/setVolumeDB 映射正确;❌ isMute 硬编码 false (Phase 2 已修) |
| 3. LO UNO XPlayer 接口 | ffplay_mute_uno_probe (office 组) — createPlayer+setMute/isMute | ⚠️ createPlayer 后 duration=0, start() 后 mediaTime=0 — 因 createPlayerWindow 未被调 (引擎在 createPlayerWindow 创建, parent=0 时不创建); LO 真实流程会调,正常播放时引擎存在 |
| 4. LibreOfficeImpressManager | 代码审查 LibreOfficeImpressManager.cpp | ❌ 未 override SetMute/GetMuteStatus,走基类 return false (Phase 2 已修) |
| 5. impresslink C ABI | ffplay_mute_abi_probe (dlopen 组) — nm + dlsym | ❌ ImpressSessionSetMute 缺失 (Phase 2 已加) |
| 6. 完整链路验证 | Phase 2 落地后 media_green_probe 端到端 | ✅ engines=1 SetMuteAll 命中 (见 6.7.5) |

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

#### 6.7.5 端到端回归 — 已闭环 (2026-08-19)

PASS: video-loop.pptx + ORT_MEDIA_BACKEND=ffplay, engines=1 SetMuteAll(true/false) 命中 (UNO marshalling 跨进程生效)。详细日志见 git 历史。

#### 6.7.6 ABI 不变性 (上层透明)

- impresslink C ABI: `ImpressSessionSetMute(void* session, int mute)` 签名不变
- LibreOfficeImpressManager::SetMute override 实现不变 (D 层, 已在 libNovaOfficeCore.so)
- libNovaOfficeCore.so 不需要重新构建 (D 层调用方式未变, 只是内部 ImpressSessionSetMute 实现改了)

#### 6.7.9 教训

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

### 6.8 ffplay 日志专项 (2026-08-20 落地)

> ffplay.so (soffice.bin 子进程内被 dlopen) 自己初始化一份 spdlog logger,
> 落盘 ~/.office-link/logs/ffplay_<pid>.log。复用 ORT_LOG/ORT_LOG_LEVEL 控制,
> 与主进程 office_<pid>.log 同目录同格式, 跨进程时序对照友好。
> ffmpeg 库内 av_log 经 callback 接入, 输出 [FFmpeg/<module>] 前缀。

#### 6.8.1 设计

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

#### 6.8.2 实施

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

#### 6.8.3 配对方式

**主进程 office log** 内含提示:
```
[OfficeRuntime] soffice child started initial_pid=N (soffice script;
  final soffice.bin pid differs due to 2-stage fork;
  ffplay log: see latest ~/.office-link/logs/ffplay_*.log by mtime)
```

**注意 pid 不匹配**: osl_executeProcess 启动 soffice 脚本 (initial_pid), 脚本 exec
oosplash → fork soffice.bin (final pid), ffplay.so 在最终 soffice.bin 内执行 getpid()。
配对方式: `ls -t ~/.office-link/logs/ffplay_*.log | head -1` (按 mtime 排序最新)。

#### 6.8.4 回归验证 — 已闭环 (2026-08-20)

PASS: ffplay log 落盘 (4357 字节, flush_on 修复 0 字节问题) + 日志全英文 + av_log 接入 (前缀 [FFmpeg/ffmpeg] [FFPLAY]) + engines=1 SetMuteAll 命中 (静音不回归) + stderr 已无裸 fprintf + flush_on 保证实时 tail -f。详细日志见 git 历史。

#### 6.8.5 不动清单 (patch 纪律)

- **ffplay_embed.c L1818**: `if (show_status == 1 && AV_LOG_INFO > av_log_get_level()) fprintf(stderr, "%s", buf.str);` — 上游 ffplay.c fallback 路径, 仅 show_status==1 时触发, 嵌入模式不走
- **cmdutils.c / ffplay.c**: 上游 ffplay.c 代码, 经验 34 patch 纪律不动
- **ffplay_embed.c L1818 fprintf**: 保留 (上游残留)

**已知小问题**: [FFmpeg/ffmpeg] 后偶尔空消息 (ffmpeg 退出路径 av_log(NULL, AV_LOG_QUIET, "") 触发, 不影响功能)

---

## 七、已知漏洞 [待修] (updated 2026-08-20)

> 攻击性测试发现的未修复漏洞。已修复的漏洞沉淀为"二、历史经验" (如经验 45)。
> 本章节的漏洞待修复后, 对应条目移入经验并标注"已修复"。

### V1: Destroy 与帧泵竞态 (竞态崩溃)

- **严重度**: ★★ (竞态依赖时序, 组合运行确凿崩溃)
- **现象**: 8 线程并发 `Create→Start→sleep(3~8ms)→Destroy` 时, `EXIT=134` (SIGABRT), 报错 `IllegalArgumentException` + 核心转储
- **根因分析**: `Destroy()` 在 `mu_` 之外 `reset platform_` / `pump_`, 而 `FramePump::PollThread` 持 `frame_mutex_` 运行 `frame_fn_` (即 `CaptureFrame`, 触及 `platform_`)。两把锁 (`mu_` vs `frame_mutex_`) 不重合, 存在窗口期。但 `FramePump::Stop()` 是同步 join 的, 根因可能更深 — 共享内核模式下多 session UNO 对象生命周期交错 (非简单锁问题)
- **关联经验**: 42 (FramePump 契约, P3 锁纪律 `frame_mutex_→mu_`) / 43 (BootLock 同源串行化弱点)
- **修复方向**: Destroy 内确保 UNO clear 在 `pump_->Stop()` join 后执行; `frame_mutex_` 保护 `platform_` 最后使用点; 或深入分析共享内核 UNO 生命周期

### V2: 高频 resize 风暴卡死 (确定性卡死)

- **严重度**: ★ (压力测试场景, 非正常使用)
- **现象**: 5000 次无停顿 `ImpressSessionSetResolution` 调用 → `EXIT=124` (超时)
- **根因**: `SetWindowSize` 的 XShm 段反复申请/释放/重建, 在 Xvfb 无 GPU + 软件渲染路径下串行阻塞加重。对比单次非法尺寸 (0/负/32768/INT_MAX) **均未崩溃**, 说明崩溃点集中在"无停顿高频 resize" 而非参数校验
- **修复方向**: `SetWindowSize` 节流/去抖/最小间隔; 或串行于帧泵

### V3: 快速 teardown-recreate 卡死 (确定性卡死)

- **严重度**: ★ (压力测试场景, 非正常使用)
- **现象**: 200 轮 `Create→Start→Destroy` (即 `delete this`) 后立即再次 `Create` → `EXIT=124` (超时)
- **根因**: 堆分配器极可能复用同一地址, `Destroy` 触发的 `BootLock`/profile seed 在快速 teardown-recreate 节奏下发生级联阻塞。根因部分与经验 43 (BootLock) 同源, 需进一步证实
- **修复方向**: Destroy 内确保 BootLock/profile seed 完全释放后再返回; 或限制 Create 频率

