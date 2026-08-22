# NovaLibreOfficePlayer 交接文档

> 新会话起点:先读本文件,再按"三、规划"推进。
> 代码在 `NovaLibreOfficePlayer/`(calc/impress/writer + base/platform + runtime 含 ffplay)+ 上层 `NovaOfficeCore/`(dlopen links)。
> 经验编号被代码注释引用,**编号只增不改**;每次认知提升更新"二、经验"(带时间+置信度),完成事项移入"一、现状"。
>
> **分层阅读 (2026-08-20 重构)**: 本文件 = Core(每次必读: 现状/沙箱运行策略/经验索引/漏洞/待办);
> 深度内容在同目录伴随文件 (本文件即 doc/HANDOFF.md),按需读取: [experiences.md](experiences.md)(经验详述)/[design-platform-isolation.md](design-platform-isolation.md)(平台隔离)/[design-framepump.md](design-framepump.md)(帧泵)/[design-ffplay.md](design-ffplay.md)(FFplay)。

---

## 0. 快速导航

| 要找什么 | 去哪 |
|---|---|
| 怎么构建/跑探针/单测 | [1.3 构建](#13-构建部署) + [1.4 沙箱运行策略](#14-沙箱运行策略必读) |
| **TRAE sandbox 限制/跑前清场/并行会话互踩** | **[1.4 沙箱运行策略](#14-沙箱运行策略必读)** |
| 项目架构/关键路径/当前状态 | 一、项目现状 |
| 某条经验的具体细节 | 二、经验表格(主索引) → [experiences.md](experiences.md)(38/40/41/42 详述+失效条件+零引用清单) |
| 平台隔离设计(3.3 P0-P10 协议/LinkPlatform)/UI 隐藏根因 | [design-platform-isolation.md](design-platform-isolation.md) |
| FramePump 设计决策/性能预算/测试矩阵 | [design-framepump.md](design-framepump.md) |
| ffplay 尺寸链/多实例根治/静音/日志 | [design-ffplay.md](design-ffplay.md) |
| **当前未修复漏洞 (V3 待修; V1/V2/V4/V5/V6 已修)** | **七、已知漏洞** |
| 隔离回归规则(改哪里要回归什么) | 四、4.2 隔离回归规则速查 |
| soffice loadComponentFromURL 排查实战 (经验 46) | [troubleshooting-soffice-load.md](troubleshooting-soffice-load.md) |

---

## 一、项目现状 [参考] (updated 2026-08-20)

### 1.1 架构

```
NovaLibreOfficePlayer/    (NovaPlayerTools/cmake 单一树子项目; target: OfficeRuntime/
  │                        CalcLink/ImpressLink/FFplay/WriterLink)
  ├── base/                共享基础层 (STATIC, 零项目依赖; 双平台)
  │     link_utils.h/.cpp u2s/s2u/u2w(Windows UTF-8→UTF-16)+ kFrameFormatBGRA/
  │                        kDefaultWidth/Height 常量 + UNO_GUARD/UNO_SILENT 宏 +
  │                        HideUiBlock UI 隐藏三件套 + DumpUiState 自省 + to_path
  │     log.h             OfficeLog 声明 (实现唯一在 office_runtime.so, 勿编第二份)
  │     office_paths.h    .office-link 路径命名空间
  │     abi.h             LINK_API 导出宏 + 统一 C ABI 接口声明 (三 link 全部
  │                        API + 共享回调类型 + ABI 契约; 原 abi/ 并入)
  │     session_registry.h  ABI 入口守卫 (V4: SessionRegistry/Guard/AbiCall)
  │     frame_pump.h/.cpp FramePump 统一帧泵 (经验 42, 三链接入; 原 frame/ 并入)
  ├── platform/            平台抽象 (STATIC, 双平台; 平台差异的家, 3.3 三原则)
  │     link_platform.h   LinkPlatform 统一平台接口 (工厂: CreateCalc/ImpressPlatform)
  │     linux/xvfb_platform.*  XvfbSessionPlatform 单类参数化 (抓帧/slot/落位)
  │     linux/linux_platforms.cpp  工厂 (匹配规则即文档类型差异, 各 2 行)
  │     windows/win_platform.*  WindowsPlatform (CreateDesktop 独立进程模式; 平台隔离
  │                               新接口 Plan/BootSection 等, calc/impress 共用, 经验 39/44)
  │     windows/kernel_host.cpp  KernelHost Windows 实现 (G 缝; 自 base 拆入)
  ├── runtime/             OfficeRuntime → office_runtime.so — 进程级共享运行时 (Linux 专属)
  │     Xvfb 大屏/LO 共享内核/slot shm/跨进程 BootLock/孤儿清场/BootLock/诊断
  │     kernel_host.cpp    KernelHost Linux 实现 (G 缝; 自 base 拆入, writer 引导缝)
  │     runtime_test.cpp — 单测 (9 场景 50 检查, --stress N; 可执行名 office_runtime_test)
  │     ffplay/            FFplay → ffplay.so — 自治媒体后端 (Manager_FFPlay;
  │                         嵌入引擎 = 定制 ffplay.c 补丁式复用, compat/)
  ├── calc/                CalcLink → calclink.so (C ABI 转发 export.cpp + session.*)
  ├── impress/             ImpressLink → impresslink.so (同上)
  └── writer/              WriterLink → writerlink.so (自治 PDF 位图管线, 经验 38: 无平台层, 页表 = Draw XDrawPages)
  third_party/             外部依赖: libreoffice/ (LO SDK UNO 头) + spdlog/ + scope_guard.hpp
  注1: 三 link 接口统一声明在 base/abi.h (调用方 dlopen 动态加载, 无独立接口头)
  注2: 跨模块 include 统一 <模块名/头名> (根 link_include INTERFACE 提供 -I, 零上溯;
       依赖方向: base ← runtime ← platform ← links, 全单向无环)
```

- **共享内核模式 (Linux)**:进程内一个 LO 内核(自研 `BootstrapOffice` 引导,复制官方 cppu::bootstrap 逻辑,独立 profile `~/.office-link/xvfb`,2026-08-18 由 `player/` 更名,见经验 40)+ 一个 Xvfb 大屏(默认 `8×3840×2160 = 30720x2160`,8 个 2160p 子屏位),多文档窗口动态落位互不重叠。调用者只需知道最大并发数 + 每文档最大分辨率。Windows 为每 session 独立 soffice + 独立桌面,不参与本模块。
- **上层**:`NovaOfficeCore/ppt/LibreOfficeImpressManager`(dlopen impresslink)、`excel/LibreOfficeCalcManager`(dlopen calclink);分发点 `PptCoreExport.cpp` 的 `PPT_PLAY_MODE_ANIMATION_LIBREOFFICE`。

### 1.2 已验证能力

- 单测 9 场景 50 检查(bootlock/slots/crossproc/acquire/adopt/dirtyenv/faultinj/linksmoke/gstcheck);加固后连续多轮全绿(经验 35)
- 探针回归(登记 9 个, CMake `-DBUILD_TOOLS=ON`):impress_nextpage/impress_multi(2 xlsx + pptx 并发,slot 0/1/2 无死锁)/media_green 双态(ffplay 默认 + gstreamer 回退,帧间差异判据)/ffplay_inject(注入 SUCCESS)/ffplay_engine(引擎推进/pause/seek/双实例)/xvfb_stress(尺寸上限)/pdf_render(writer 两方案可行性)/writer(翻页/缓存/Prev)/word_core(NovaOfficeCore 分发)
- **同页双视频并行播放**(dual_media.pptx 实证,经验 37);Demo 实测三画面/翻页正常;媒体页真实视频+音频
- **writerlink 底层链路已闭环**(writerlink,经验 38):底层(翻页 20-53ms/页、LRU、缓存命中 0ms、Prev 验证)探针实测全绿。**上层接线已回退(2026-08-18)**:NovaOfficeCore/NovaPlayer/NovaPlayerDemo 的 word LibreOffice 接入改动(IWordManager 抽象/LibreOfficeWriterManager 分发/NP_WORD_PLAY_MODE_ANIMATION_LIBREOFFICE 枚举/Demo Word 模式下拉框)整体还原,功能就绪待后续接入。LibreOfficeWriterManager.cpp/.h 作为样板保留(NovaOfficeCore/word/,不参与构建,去 IWordManager 依赖)
- 抓帧性能:XShm 1080p ~1ms/1440p ~2.5ms/2160p ~5.9ms;Xvfb 30720x2160 RSS ~300MB
- LO 源码两处改动已固化远端:commit `83e0b9c3e`(gstplayer.cxx + mediawindow_impl.cxx),master == origin
- compat/ffplay.c、cmdutils.c/h 与 SDK 上游(FFmpeg4.4.1SDK/source/ffmpeg-4.4/fftools)**diff=0**(2026-08-17 实测);ffplay_embed.patch 重放 == ffplay_embed.c(改 embed.c 必须回填 patch)

### 1.3 构建/部署

```bash
# 统一构建 (唯一入口, 单一树 debug/)
cd NovaPlayerTools && ./build_in_linux.sh
# 定向: cmake --build debug --target OfficeRuntime CalcLink ImpressLink FFplay office_runtime_test -j$(nproc)
# 产物直出部署目录 NovaPlayer/bin_<arch>_<sys>/office/program (单副本)。
# 注意: 部署目录在 NovaPlayer/ 下 (不是 NovaPlayerTools/, 后者同名目录为空)。

# 探针编译 (CMake 子模块, 默认不编译; 源码在 tools/linux/, 素材在 tools/data/)
cmake -S . -B build -DBUILD_TOOLS=ON -DLIBREOFFICE_SDK_ROOT=/path/to/office/sdk
cmake --build build --target probes -j$(nproc)
# 或单探针: cmake --build build --target impress_nextpage_probe

# 并发回归 (2 xlsx + pptx)
build/tools/impress_multi_probe "tools/data/志愿分析.xlsx" "tools/data/7-8月报销明细表-正式版.xlsx" "tools/data/AI时代.pptx"
# 媒体回归 (双态)
build/tools/media_green_probe "tools/data/AI时代.pptx"                      # 默认 ffplay
ORT_MEDIA_BACKEND=gstreamer build/tools/media_green_probe "tools/data/AI时代.pptx"  # 回退 gst
```

### 1.4 沙箱运行策略(必读)

> 每次跑单测/探针/回归前的环境前提与清场纪律。集中自原 1.3 运行注释、经验 35/36/38⑤、原 1.6 sandbox 条目 (2026-08-20 重构)。

#### TRAE sandbox 约束 (2026-08-18 实测)

- TRAE sandbox **阻断子进程写 `~/.office-link/`**(`SeedKernelProfile` 的 `fs::copy_file` 报 Permission denied → `BootstrapOffice` 抛 DeploymentException)
- **影响**: 所有需 LO bootstrap 的探针(impress_nextpage/media_green/impress_multi/writer/pdf_render/word_core/ffplay_inject/attack_*)在 sandbox 内 bootstrap 阶段失败;office_runtime 单测(用 fake soffice,不引真内核)和 xvfb_stress_probe(纯 Xvfb,无 LO)不受影响
- **破解①(推荐, 2026-08-20 实证)**: `ORT_HOME=/tmp/ort_xxx` 跑探针 — office_paths 原生基目录覆盖(设计用途即探针/多实例隔离), profile/logs 全部改道 /tmp (sandbox 可写), 零代码改动全链路 bootstrap 成功; 附带 `LD_LIBRARY_PATH=<deploy>/office/program` (探针 RUNPATH 已烧入, 手动设置时必须绝对路径)
- **破解②**: sandbox 配置放行 `~/.office-link/` 读写,或无 sandbox 环境跑。真实部署环境无此限制。代码逻辑已 shell 手动 `cp` 验证正确,纯属 sandbox 文件策略
- **探针输入用绝对路径** (2026-08-20 实证): 相对路径 `test.xlsx` → `getFileURLFromSystemPath` 产出相对 URL → `loadComponentFromURL` 抛 IllegalArgumentException (UNO 异常**不继承** std::exception, 见 1.4 末尾冷知识)
- **探针用 pptx 素材注意** (2026-08-20 实证): `tools/data/pptx/test.pptx` (gen_test_pptx.py 生成, 9 entries) 在共享内核+Hidden 加载下静默返回 null (`soffice --convert-to` 却能识别); 换 LO 源码树完整 pptx (如 `xmloff/qa/unit/data/Reference-ThemeColors-TextAndFill.pptx`) 加载正常

#### 跑前清场

```bash
# office_runtime 单测前提: 无其他 office_runtime 使用者; 跑前清场
rm -f /dev/shm/nova_office_slots_v1 /dev/shm/sem.nova_office_boot /tmp/.X9*-lock
NovaPlayer/bin_x86_64_kylin/office_runtime_test [--stress N]

# 探针/单测可免 LD_LIBRARY_PATH 直接跑 (烧入绝对 RUNPATH, 经验 36);
# 手动设置时必须绝对路径 (相对路径 → dladdr → UNO_PATH 相对化 → bootstrap SIGABRT)。
# 探针跑前清场: kill Xvfb + rm lock + rm /tmp/.X11-unix/X9*
```

#### 清场纪律 (Xvfb 生命周期, 经验 35)

- **杀 Xvfb 必须等死透** (ProcAlive 轮询; waitpid 对非子进程 ECHILD 无效): 大屏 Xvfb(~300MB)SIGKILL 后垂死窗口内 socket 仍监听, 同号立即重启必 "server already running" 失败; 残留 socket 文件(无活 server)无害
- 标准序列: 杀 → 等死透 → 清 lock + socket (`/tmp/.X<n>-lock` + `/tmp/.X11-unix/X<n>`, 号段 90-99)
- **SafeKill guard**: pid 来自 lock/管道读回, 竞态下可能 0/-1 (kill(0)=杀进程组, 实测全家死); 统一 guard(pid>0 && ≠self), 断言不假设 :90 (display 号感知)

#### 并行会话协作约定 (经验 38⑤)

- 清场命令(kill Xvfb)**只处理自己的 display 号或先互查**——:90 是共享运行时的, 2026-08-17 12:46 实测互踩过一次
- 双实例部署: 共享内核/屏的所有者进程退出, 其他进程会话断连; 需同时使用

#### 冷知识: UNO 异常不继承 std::exception (2026-08-20 实证)

- 本项目 LO 构建的 `css::uno::Exception` 是**裸类** (见 `third_party/libreoffice/com/sun/star/uno/Exception.hdl`), 不继承 std::exception — 与 LO 官方新版 SDK 不同
- **后果**: `catch (const std::exception&)` 抓不住 UNO 异常 (IllegalArgumentException/RuntimeException 等), 全部落入 `catch(...)`; ABI 层的 AbiCall 双 catch 链 (V4 修复) 恰好完整兜底
- **诊断技巧**: 会话层需 UNO 异常细节时须显式 `catch (const css::uno::Exception& e)` 打 `e.Message` (session.cpp loadComponentFromURL 段同款写法)

### 1.5 关键路径

```
~/.office-link/                  项目用户级数据根
  ├─ xvfb/                       共享内核工作 profile (2026-08-18 由 player/ 更名; 引导时从
  │                                部署 templates/user fresh copy 初始化, 见经验 40)
  ├─ desktops/<link>/<guid>/     Windows 每 session 独立桌面工作 profile (同模板初始化)
  ├─ logs/                       OfficeLog 日志 office_<pid>.log + ffplay_<pid>.log (各 5MB×3 轮转)
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

### 1.6 关键文件

- `runtime/runtime.cpp` — 全部运行时逻辑(Xvfb 扫号 90-99/adopt/残留清理、BootstrapOffice、slot shm、BootLock、CleanupOrphanSoffice、窗口诊断、CheckGstDeps)
- `third_party/scope_guard.hpp` — 第三方库(Neargye/scope_guard 0.9.4,MIT),提供 `DEFER` 宏用于 C 资源清理(XCloseDisplay/munmap/close);runtime.cpp 使用
- `runtime/ffplay/compat/` — `ffplay.c`(上游 diff=0)+ `ffplay_embed.c`(= ffplay.c + `ffplay_embed.patch`)+ `ffplay_engine.h` 引擎 C API + 手写 `config.h`
- `platform/linux/xvfb_platform.cpp` — XShm 抓帧 + BGRX 字节序直拷 + 窗口扫描/落位
- `base/frame_pump.h/.cpp` — FramePump 统一帧泵 (经验 42, 三链接入; 原 frame/ 并入 base)
- `calc|impress|writer/session.*` — 会话(加载/控制/轮询;calc 滚动/切表/缩放,impress XPresentation2 窗口化放映 + gotoNextEffect 翻页);`export.cpp` 为 C ABI 转发层 (原 *link.cpp), 接口声明在 `base/abi.h`
- `tools/CMakeLists.txt` — 探针编译 (CMake 子模块, `-DBUILD_TOOLS=ON`; 源码在 `tools/linux/`, 素材在 `tools/data/`)

### 1.7 当前状态与注意事项

- **媒体后端**:默认 ffplay(`ORT_MEDIA_BACKEND`,EnsureKernel setenv 不覆盖宿主);gstreamer 为验证过的回退路径(ximagesink 补丁版 libavmediagst.so 保留;.bak 为补丁前备份)
- **GL 全禁用**:SAL_DISABLEGL=1(转场,经验 21)+ ffplay 的 SDL_FRAMEBUFFER_ACCELERATION=0 + SOFTWARE renderer(经验 37)——Xvfb 恒无 GPU,一切渲染固定软件路径
- **日志体系(2026-08-18 收尾定稿)**:统一入口 `OfficeLog/Dbg/Warn/Err`(varargs,LogMsg 等历史包装已删);前缀 = target 名 `[OfficeRuntime]/[CalcLink]/[ImpressLink]/[WriterLink]/[KernelHost]`(子场景点分如 `[CalcLink.Scroll]`/`[Common.UIHide]`/`[Common.WinWindow]`/`[Common.WinProfile]`/`[Common.X11]`/`[Common.Boot]`);平台层 Tag() 输出 lowercase `[calc]/[impress]`(区分会话层 `[CalcLink]/[ImpressLink]`);ffplay 组件在 soffice 进程内(office_runtime.so 不在),独立 spdlog logger 落盘 ffplay_<pid>.log(见 [design-ffplay.md](design-ffplay.md) §4)。级别:info=生命周期主线 / debug=诊断细节(窗口扫描/UI 自省/渲染计时) / warn=防御拦截与回退 / error=失败;文件格式 `[时间] [level] [前缀] 消息`,双平台一致(win_office_log 对偶)。开关:ORT_LOG=both(默认)|file|stderr|off(**off 真 silent**)、ORT_LOG_LEVEL=debug|info(默认)|warn|error。落位 `office_paths::logs_dir()/office_<pid>.log`(Linux spdlog 5MB×3 轮转;stderr 副本有缓冲差异,排查以文件为准);两侧均 flush_on(info) 保证实时 tail -f
- **Windows 平台隔离回归已完成 (2026-08-19)**: 编译零错误 (win_platform 新接口 Plan/BeginBoot/DiscoverWindow/FormWindow/ApplyNativeFullscreen/OnSessionEnd + calc F 反序 + impress 全屏放映 + writer KernelHost); 探针五段全绿 + NovaPlayerDemo calc/impress 全量通过 (UI 全隐藏含公式栏, 经验 44); 回归修复: win_platform Plan() impress discover AfterReveal→AfterStart + 核心 P8 discover 分支 (协议遗漏) + NovaOfficeCore PptCoreExport Windows 分发恢复 (被清理误删) + 模板补 calc 基线 (69→126 项, 见 3.1 模板部署保障)
- UNO_PATH/URE_BOOTSTRAP 依赖部署位置(office/program),部署路径变化需同步(经验 23/33)
- **旧独立进程方案已清理 (2026-08-17)**:source/ 目录、NovaLibreOfficePlayerDeprecated target、NovaLibreOfficePlayer.vcxproj、PptAnimationManagerLinux/LibreOffice(零实例化, PptCoreExport 全走新链/图片模式)、sln 工程引用、孤儿可执行 全部删除(git 可恢复)。Windows 侧为文本对应清理(CMake/sln/vcxproj),**需 Windows 编译确认**。ShareMemoryReaderLinux/NamePipe* 为 PDF 链/公共设施,保留
- 已知待清理:① ~~calc profile seed 死开销~~(已清, 2026-08-17);② ~~[CALC-T]/[IMP-T] 等诊断日志~~(已清, 2026-08-18 日志体系统一);③ ~~过时探针~~(已清, 2026-08-18: 22 文件, 探针目录缩至 9 个全登记)
- **writer_cache 总量回收已落地(2026-08-18,简单策略)**:写入后总量超限(ORT_WRITER_CACHE_MB,默认 500MB)按 mtime 最旧删除,排除当前会话文件;实测 1MB 上限 3 文档触发淘汰正常
- 诊断开关:ORT_DUMP_WINDOWS=1(窗口树/重叠/边缘像素);ffplay video_open 打印 renderer 后端
- **writerlink 上层接线已回退(2026-08-18)**:writerlink.so 功能就绪(探针全绿),但 NovaOfficeCore/NovaPlayer/NovaPlayerDemo 的接入改动整体回退(IWordManager 抽象删除、WordCoreExport/WordManager 还原、NP_WORD_PLAY_MODE_ANIMATION_LIBREOFFICE 枚举移除、Demo Word 模式下拉框移除)。LibreOfficeWriterManager.cpp/.h 作为样板保留在 NovaOfficeCore/word/(去 IWordManager 依赖,不参与 CMake/vcxproj 构建),后续接入时恢复继承+override+构建配置即可。NovaLibreOfficePlayer/writer/ 本身不动
- **攻击性测试 (2026-08-20)**: 攻击探针 (`attack_uaf_probe` / `attack_resize_probe` / `attack_pagenav_probe` / `attack_mute_teardown_probe` / `attack_lock_inversion` / `attack_cb_join_self`, 登记于 `tools/CMakeLists.txt` 链接组 `add_multi_tools`), 发现 V1-V6 共 6 项可复现崩溃/卡死。V1/V4 已关闭 (AbiCall+SessionRegistry+destroyed_), V2 已修复 (SetWindowSize 去 sleep + SetResolution 50ms 节流), V5 已关闭 (API 锁序约束注释), V6 已修复 (FramePump::Stop 线程 ID 检测防 join self)。V3 待修。攻击报告已归档 (git 历史可查 `attack-report.20260820.achieved.md`)。

---

## 二、历史经验(勿回退;编号被代码注释引用) [经验·永久] (updated 2026-08-19)

> 时间=提出/验证时间;置信度:高=源码级或多次实测,中=单次实测,低=推断。
> **详述**(经验 38/40/41/42)+ **失效条件表** + **零引用清单** 见 [experiences.md](experiences.md)。

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
| 41 | **Impress 暂停→恢复翻页失效**:pause/resume 不对称 + StartPoller early-return 致 paused_ 不重置。FramePump 接入后同构复现 (泵 Start 幂等早返未重置 paused_), 已由泵契约根治。[详述](experiences.md) | 08-18 | 高(实测修复) |
| 42 | **FramePoller 共性分析与治理**:三 link poller 六维不一致 + P3-P8 新发现。FramePump 组件统一帧泵, 阶段0-4 全部落地, 三链接入收官。[详述](experiences.md) | 08-18 | 高(阶段0-4全部完成) |
| 43 | **BootLock 构造即加锁 + 非递归 mutex 自死锁**:包装"构造即获取"型 RAII 资源, 包装层构造函数必须为空; 二次 Lock = 静默永久死锁(无日志/超时不保护)。详见 doc/design-platform-isolation.md Part 1 | 08-18 | 高(源码级+实测修复) |
| 44 | **Calc 公式栏 (fx/Σ 输入行) 隐藏 (2026-08-18 demo 实测)**:公式栏是 **SFX docking window** (UI 布局 inputbar.ui, 窗口类 InputBar), **不是 LayoutManager toolbar 元素** —— hideElement(formulabar)/模板条目/ShowFormulaBar 属性 (SDK IDL 无此名, 猜测无效) 全部不生效; 老 office/user 亦无其持久化条目。**真实控制 = UNO 命令 `.uno:InputLineVisible`** (scalc menubar.xml View 菜单有据可查), dispatch 需 **frame_ provider** (文档级 sc 模块命令; desktop_ queryDispatch 返回 NOT found —— 桌面级命令如 FullScreen 才用 desktop_); 每次会话从模板基线开始公式栏默认显示, toggle 一次即隐藏 (状态确定, 无需查询)。排查陷阱: 公式栏相关的 popupmenu/formulabar.xml 是弹出菜单非主控件; 探针环境 LO 渲染不完整 —— UI 验证以 demo 为准。**排查纪律**: UNO_SILENT 异常进 debug 级日志, "静默失败"排查第一动作开 ORT_LOG_LEVEL=debug 看 `UNO exception (silent)` 痕迹。**[2026-08-19 4.2 实证修正]**: InputLineVisible dispatch 在 Linux 共享内核下破坏 vis=0 初始态导致 UI 复活, 已下沉至 Windows HideUiExtras (Linux 空操作); LO Xvfb 无头环境公式栏默认 vis=0 不显示, 无需 dispatch | 08-18 | 高(实测, 部分认知已修正) |
| 45 | **C ABI 重复 Destroy UAF 防护 (V4 双层防护已补全)**: C ABI `ImpressSessionDestroy` / `CalcSessionDestroy` 直接 `delete static_cast<...*>(session)`, 重复调用时悬垂指针 → use-after-free → SIGABRT (确定性必现)。修复: `SessionRegistry` (recursive_mutex + live 集合), `Register`/`TryRevoke`/`Guard`/`WithGuard` 回调式守卫。**V4 补全 (2026-08-20)**: `Guard` 拦截销毁后所有 API 调用 + `AbiCall` 异常边界 + 会话内 `destroyed_` 标志。**WithGuard 重构 (2026-08-20)**: 消除重复 `Guard g + if(!g)` 模式, 三 link 统一回调式。失效条件: 改用智能指针管理 session 生命周期时本防护可移除 | 08-20 | 高(已修复, V4 实证全绿) |

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
| 31 | **统一构建树**(2026-08-13):NovaLibreOfficePlayer 并入 NovaPlayerTools cmake 单一树,build_links_linux.sh 退役,ABI 同步由依赖图承接(links 链 OfficeRuntime target)。坑:① 伞 target 改名后 -Bsymbolic 需手动应用 ③ ld 对直接 .so 输入按 basename 记 DT_NEEDED ⑤ ffplay 链 ffmpeg 需 --no-as-needed ⑥ 探针不参与统一构建树(CMake 子模块 `tools/`, `-DBUILD_TOOLS=ON` 开启) | 08-13 | 高 |
| 32 | **平台层归组重构**(2026-08-14,消 77% 重复):按环境归组 platform/{linux,windows},规则参数化(窗口匹配规则=文档类型差异,工厂各 2 行);会话层不强提基类;common STATIC 链入各 link,log.h 实现保持 office_runtime 单例(双份=spdlog 双写) | 08-14 | 高 |
| 33 | **.so 路径错位 + CMake 缓存自愈**:① 双份 .so 时 dladdr(GetRuntimeDir)错位→UNO_PATH 错→rc=4;修复=四件套 per-target 输出部署目录单副本 ② FindLibreOfficeSDK 模块移动后旧缓存 FATAL;修复=NOT EXISTS 时 FORCE 重推导 | 08-14 | 高 |
| 35 | **Xvfb 垂死窗口竞态家族(2026-08-17)**:① 大屏 Xvfb(~300MB)SIGKILL 后垂死窗口内 socket 仍监听,同号立即重启必 "server already running" 失败;残留 socket 文件(无活 server)无害。杀后必须等死透(ProcAlive 轮询;**waitpid 对非子进程 ECHILD 无效**)。测试/探针侧 CleanXvfbBattlefield/SafeKill/StopXvfb 均已"杀→等死透→清 lock+socket";office_runtime 侧 StopXvfb 清 lock+socket、StartXvfb fork 前清残留+lost-race 清残局(扫号重试本身已是正确自愈) ② **kill(0)/kill(-1) 灭组**:pid 来自 lock/管道读回,竞态下可能 0/-1(kill(0)=杀进程组,实测测试+tail 全家死,无 core 无日志);SafeKill 统一 guard(pid>0 && ≠self)+ SpawnOrphan 读回校验 + 断言不假设 :90(display 号感知)。**操作速查见 1.4 沙箱运行策略** | 08-17 | 高 |

### 2.3 抓帧与性能

| # | 经验 | 时间 | 置信度 |
|---|---|---|---|
| 13 | **XShm + BGRX 字节序直拷**:Xvfb TrueColor24 视觉=32bpp LSBFirst BGRX → XShmGetImage(0.01ms)+memcpy+alpha(1080p ~1ms vs XGetImage 转换 ~9ms)。非 BGRX 自动回退。links 必须链 X11::Xext | 08-12 前 | 高 |
| 14 | **屏高 ≥ 最大文档分辨率**(2160p 窗口在 1080 屏 BadMatch);StartXvfb 按 max_doc_height 定高 | 08-12 | 高 |
| 15 | **屏尺寸 16 位坐标上限 32767 内无阻碍;30720x2160 RSS ~300MB 可起** | 08-12 | 高 |

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
| 16 | **ABI 同步已由依赖图承接**(runtime.h 变更重编三件套);历史教训:只重编 office_runtime 导致 undefined symbol | 08-12 前 | 高 |
| — | **组件名陷阱**:gstreamer 生效组件是 libavmediagst.so(libavmedialo.so 是另一组件) | 08-12 | 高 |
| 36 | **相对 LD_LIBRARY_PATH 陷阱(2026-08-17)**:手动跑探针/单测用相对路径→dladdr→UNO_PATH 相对化→BootstrapOffice DeploymentException SIGABRT。产品不受影响(run.sh $CURDIR 绝对)。**探针/单测烧入绝对 RUNPATH,免设直接跑(推荐)**;手动设置必须绝对路径。部署目录=NovaPlayer/bin_<arch>_<sys>(非 NovaPlayerTools/)。**操作速查见 1.4** | 08-17 | 高 |
| 46 | **相对路径→loadComponentFromURL 失败(2026-08-20)**:`osl::FileBase::getFileURLFromSystemPath` 对相对路径不报错但生成非法 URL(`./test.xlsx` 而非 `file:///...`),soffice.bin 拒绝(calc 抛 Unsupported URL / impress 静默 null)。产品不受影响(调用方传绝对路径)。**探针必须传绝对路径**。排查全流程(含 ffplay/URP 排除项)见 [troubleshooting-soffice-load.md](troubleshooting-soffice-load.md) | 08-20 | 高(实证) |

### 2.6 文档渲染(writer, 已落地 2026-08-17)

| # | 经验 | 时间 | 置信度 |
|---|---|---|---|
| 40 | **user 模板机制 + office-link 命名定稿**:UI 控制三层优先级 / 命名 / 模板净化 / 消费语义 / 孤儿文档锁坑 / sidebar+statusbar 存储位置与部署陈旧坑(⑦⑧)。详见 [experiences.md](experiences.md) 经验 40 详述 | 08-18 | 高(实证) |
| 38 | **writer 渲染方案可行性**:docx→PDF→Draw→XSlideRenderer→BGRA 全 UNO 自治 / 接口细节 / 性能 / 缓存 / 上层接线 / 质量收尾。详见 [experiences.md](experiences.md) 经验 38 详述 | 08-17 | 高(实测) |
| 39 | **Windows 平台差异定稿**:per-session 独立 soffice + 隐藏桌面 (`SALTMPSUBFRAME`) + `IsFullScreen=true` (LO 自管窗口, 无菜单栏/标题栏) + 双平台日志同款实现 (Linux runtime.cpp spdlog / Windows platform/win_office_log.cpp) — 与 Linux 共享内核模式正交的设计分支。代码引用: session.h:6 / session.cpp:7 / session.h:24 / link_platform.h:6 / win_platform.cpp:156 / log.h:12 / platform/CMakeLists.txt:16 / runtime.h:19 | 08-19 | 高(架构定稿) |

> **失效条件表** (关键经验何时需重新评估) 与 **零引用经验清单** (19 条) 见 [experiences.md](experiences.md)。

---

## 三、设计/待办 [设计+待办] (updated 2026-08-20)

### 3.0 平台隔离设计验证 — 已闭环 (2026-08-18)

> 全文(目的达成评估 6 项核验)见 [design-platform-isolation.md](design-platform-isolation.md) Part 1。
> 关键沉淀: 经验 43 (BootLock 死锁根因) / 设计规格 (同文件 Part 2) / Windows 回归 (commit 5832a507)。
> 摘要: 会话层零逻辑 `#ifdef` ✅ / 平台差异全落平台层 ✅ / 变体点 SessionPlan 可枚举 ✅ / Windows 编译+探针+demo 全过 ✅; 诚实边界: 无 Windows CI, FramePump 待 Windows 侧确认。

### 3.1 待办/讨论

> ★★★=立即;★★=中期;★=远期。

| 排序 | 事项 | 说明 |
|---|---|---|
| ★★ | **V3 漏洞修复** (七、已知漏洞): 快速 teardown-recreate 卡死 (impress settle_ms=2500 × 200 轮累加超时)。V1/V4 已关闭, V2 已修复 (SetWindowSize 去 sleep + 节流), V5 已关闭 (API 约束), V6 已修复 (Stop 防 join self) | 当前中心 |
| ★★ | **user 模板部署保障 (2026-08-18, 两平台)**: 模板 = 仓库 `templates/user/registrymodifications.xcu` (净化, 经验 40) → 部署 `office/program/templates/`。**Linux**: office_runtime POST_BUILD 自动拷贝 ✓; **Windows**: 不构建 office_runtime, **NovaPlayer 打包脚本 (CopyFile.bat 等) 需加 templates/ 拷贝项** —— 缺失时 WindowsPlatform::PrepareEnvironment seed 失败 → LO 默认 UI (2026-08-18 探针实测, 已手动部署当前环境) | 打包流程 |
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

### 3.3 平台隔离设计(意图/机制分离)— 已实施 2026-08-18 (J1-J4 全量)

> **全文** (三原则/两类差异判定/P0-P10 会话协议/LinkPlatform 接口定稿/变体点总账/calc Win 反序用例/writer 引导缝/知识安居铁律/三层保证/J 迁移路径/反模式) 见 [design-platform-isolation.md](design-platform-isolation.md) Part 2。
> 一句话: 会话层零 `#ifdef`, 平台差异收进 SessionPlan 数据 + LinkPlatform 接口 (P0-P10 里程碑协议), 分歧不可消除但可以安放。

---

## 四、平台隔离专项 [设计+经验] (updated 2026-08-19)

> 深度内容 (UI 隐藏隔离根因 4 组对照实验/HideUiExtras 下沉/6 次探针实证/模板隔离评估) 见 [design-platform-isolation.md](design-platform-isolation.md) Part 3。

### 4.1 隔离边界总账

| 层 | 隔离状态 | 范式 | 说明 |
|----|---------|------|------|
| 窗口发现/定型 (DiscoverWindow/FormWindow) | ✅ 已隔离 | SessionPlan 数据驱动 | 3.3 J1-J3 |
| 终止策略 (terminate_on_destroy) | ✅ 已隔离 | SessionPlan 数据驱动 | 3.3 J3 |
| 引导段 (BeginBoot/Release) | ✅ 已隔离 | BootSection RAII | 3.3 J1-J2 |
| 全屏浮窗 (HideUiFloats) | ✅ 已隔离 | LinkPlatform 虚函数 | Linux 空 / Win 原生 API |
| **UI 修补 (HideUiExtras)** | ✅ 已隔离 (2026-08-19) | LinkPlatform 虚函数 | 4.2 → doc Part 3 §3.1 |
| 模板 (registrymodifications.xcu) | ⚠️ 共享 | 真相源单一份 | 待评估 (doc Part 3 §3.2) |

### 4.2 隔离回归规则速查

| 改动位置 | 回归范围 | 示例 |
|---------|---------|------|
| 平台层 (platform/linux/, platform/windows/) | 单平台 | xvfb_platform.cpp / win_platform.cpp |
| 共享层 (session.cpp, link_utils.cpp, session.cpp) | **双平台** | 任何会话逻辑改动 |
| 共享模板 (templates/user/) | **双平台** | registrymodifications.xcu |
| 平台接口 (link_platform.h) | **双平台** | 新增/修改虚函数 |

---

## 五、帧泵专项 [设计] (updated 2026-08-19)

> 对应经验 42 ([详述](experiences.md))。**设计决策论证 (frame_mutex_ 串行化否决单线程委托/锁纪律/静止检测两阶段/tick 循环)、性能预算、测试矩阵、开放问题 A/B** 见 [design-framepump.md](design-framepump.md)。
> 摘要: FramePump 统一帧泵 (common, 与平台隔离正交), 三链 (impress tick=40/writer 5/calc 20) 全部接入收官; P1-P8 缺陷由契约构造性消灭; 单测 15/15; 阶段5 (dedupe/增量比对) 可选未做。

---

## 六、FFplay 嵌入专项 [经验+设计] (updated 2026-08-20)

> ffplay 嵌入引擎 (runtime/ffplay, 补丁式复用 FFmpeg ffplay.c) 专项治理。**尺寸链修复/多实例根治 (三个 bug + render_mutex)/静音专项 (C1 UNO 远程调用全链路)/日志专项 (ffplay_<pid>.log + av_log callback)** 见 [design-ffplay.md](design-ffplay.md)。
> 摘要: ① video_open 尺寸修复 (SDL_GetWindowSize 替代 640x480 硬编码); ② 多实例根治 (audio_dev 下沉 VideoState + render_mutex + per-instance 销毁); ③ 静音 C1 简化版 (XFastPropertySet handle 0 = MUTE_ALL, UNO pipe 跨进程); ④ 日志专项 (spdlog 独立 logger, [FFmpeg/<module>] 前缀, flush_on(info))。
> 关联经验: 34 (补丁纪律: 改 embed.c 必须回填 patch) / 37 (SDL 软件渲染) / 19b (Xvfb 无 GPU) / 28-30 (注入+后端开关)。

---

## 七、已知漏洞 [1 待修/5 已修] (updated 2026-08-21)

> 攻击性测试发现的漏洞。已修复的标注"已修复"并保留在此供查阅。
> 复现探针: `tools/CMakeLists.txt` 链接组 `add_multi_tools` (attack_uaf/attack_resize/attack_pagenav/attack_mute_teardown/attack_lock_inversion/attack_cb_join_self)。

| 编号 | 状态 | 一句话 |
|------|------|--------|
| V1 | ✅ 已关闭 | Destroy 与帧泵竞态 (AbiCall+destroyed_+Guard 三层修复) |
| V2 | ✅ 已修复 | 高频 resize 风暴 (去 sleep+节流) |
| V3 | ❌ **待修** | 快速 teardown-recreate 卡死 |
| V4 | ✅ 已关闭 | 销毁后 API 调用崩溃 (SessionRegistry::Guard 全入口守卫) |
| V5 | ✅ 已关闭 | 锁序反转 (API 契约约束) |
| V6 | ✅ 已修复 | 回调中 Destroy 导致泵线程 join 自己 (线程 ID 检测) |

---

## X、构建 [构建·经验归集] (updated 2026-08-21)


> 构建系统专项: 单一树哲学 / 依赖发现上提 / 部署单副本 / 跨平台参数边界 / 模拟环境限制。
> 编号经验 (31/33/23/25/36) 原文在主索引 (二章表格) 与详述 (experiences.md), 此处归集其构建维度要点 + 本次重构结论, **编号只增不改**。

### X.1 单一构建树与依赖发现 (经验 31)

- **单一树**: `NovaLibreOfficePlayer` 并入 `NovaPlayerTools` cmake 单一树; 伞 `CMakeLists.txt` 只做 `add_subdirectory`, 依赖发现 (LibreOfficeSDK/Threads/X11) **统一在根完成**, 子树只声明 target (ABI 同步由依赖图承接: links 链 OfficeRuntime/Common target, 经验 31)。
- **重构 (2026-08-21)**: 删除 6 个子目录的 `project()` + 重复的 `cmake_minimum_required`/`find_package`/`set(CMAKE_CXX_STANDARD)` 样板 (约 42 行), 上提到根。子 project() 在单一树场景下冗余 (target 名硬编码、无独立 install/export; 仅 Windows VS 下有微弱 IDE 分组收益); 编译器/标准检测根已做, 子树继承。根 `cmake_minimum_required` 由 3.13 升到 3.16 统一版本。
- **等价性**: 重构未碰 target 名 / MSVC 参数 (`if(MSVC)` 的 `/utf-8` `/EHsc`) / 链接与部署逻辑 / 编译宏定义 → 各 target 最终编译命令集合不变 → 回归无忧 (Linux 真编 100% + 单测全绿实证)。

### X.2 部署单副本 (经验 33 / 31)

- **四件套 per-target 输出部署目录单副本**: 各 target `LIBRARY_OUTPUT_DIRECTORY` 直指部署目录 `office/program` (非顶层 bin 根), `POST_BUILD` 拷贝。双份 .so → dladdr(GetRuntimeDir) 错位 → UNO_PATH 错 → bootstrap rc=4。
- **CMake 缓存自愈 (经验 33)**: `FindLibreOfficeSDK` 模块移动后旧缓存 FATAL; 修复 = `NOT EXISTS` 时 `FORCE` 重推导。默认路径 `cmake/` 上溯 3 级到 `NovaPlayer` 在当前机器失效 (真实路径 `NovaPlayerProject/NovaPlayer`), **须显式 `-DLIBREOFFICE_SDK_ROOT=xxx` 覆盖** (注释已标注)。

### X.3 引导与运行时参数 (经验 23 / 25 / 36)

- **BootstrapOffice (经验 23)**: 复制 `cppu::bootstrap`, 零 LO 源码改动; `-env:UserInstallation` 支持独立内核。四坑: ① 先 `set URE_BOOTSTRAP` ② 客户端需 `UNO_PATH` ③ 连接串 `StarOffice.ComponentContext` ④ `osl_executeProcess` 原样。引导 ~504ms。
- **libstdc++ SONAME 单例 (经验 25)**: 宿主预加载系统 6.0.28 后 dlopen link 缺 GLIBCXX 符号失败。修复 = links 加 `-Wl,-rpath,<deploy>` + 显式链部署目录 libstdc++ 6.0.30 (组件自包含, 免 `LD_LIBRARY_PATH`)。`env -i` 缺 `LANG` 时 LO 报 type detection failed, 勿误判为库问题。
- **相对 LD_LIBRARY_PATH 陷阱 (经验 36)**: 手动跑探针/单测用相对路径 → dladdr → UNO_PATH 相对化 → bootstrap SIGABRT。探针/单测烧入绝对 RUNPATH 免设直接跑; 手动设置必须绝对路径。部署目录 = `NovaPlayer/bin_<arch>_<sys>` (非 `NovaPlayerTools/`)。

### X.4 跨平台参数边界

- **MSVC 专属参数** (`/utf-8` 防 GBK 注释吞换行 C4819 / `/EHsc` UNO 异常展开 / `_CRT_SECURE_NO_WARNINGS`): 仅真 MSVC 编译生效, 由根 `if(MSVC)` 块设置, **重构不碰**。Windows 分支当前未做独立编译验证 (双平台推进暂缓), MSVC 参数正确性待 Windows 侧确认。
- **平台差异**: `office_runtime`/X11/ffplay 为 Linux 专属, 收进根 `if(NOT WIN32)` 块; Windows 走每 session 独立 soffice, 不编 office_runtime (经验 39)。



