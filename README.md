# NovaLibreOfficePlayer 交接文档

> 新会话起点:先读本文件,再按"三、项目未来规划"推进。
> 结构:一、项目现状(现在)/ 二、历史经验总结(过去,含提出时间+置信度)/ 三、项目未来规划(未来,含价值排序)。
> 代码在 `NovaLibreOfficePlayer/`(calc/impress 模块 + common/ 平台公共层)+ `office_runtime/`(共享运行时 + ffplay 媒体引擎)+ `NovaOfficeCore/`(上层 Manager)。

---

## 一、项目现状(现在)

### 1.1 架构

```
NovaLibreOfficePlayer/    (构建伞: NovaPlayerTools/cmake 单一树子项目, target 名
  │                        NovaLibreOfficePlayerDeprecated/OfficeRuntime/CalcLink/
  │                        ImpressLink/FFplay; 无独立 build 树, 见经验 31)
  ├── common/              (基础层, 零依赖 office_runtime; 2026-08-14 平台归组, 经验 32)
  │     log.h             OfficeLog 声明 (实现唯一在 office_runtime.so)
  │     link_platform.h   统一平台接口 (原 CalcPlatform/ImpressPlatform 合并)
  │     link_utils.h/.cpp 会话工具 (u2s/s2u/HideUiBlock)
  │     cmake/FindLibreOfficeSDK.cmake  SDK 查找模块 (单份, 缓存自愈)
  │     linux/xvfb_platform.h/.cpp  XvfbSessionPlatform 单类 (规则参数化: 抓帧/slot/落位)
  │     linux/linux_platforms.cpp    工厂 (规则即文档类型, 各 2 行)
  │     windows/win_platform.h/.cpp  WindowsPlatform (CreateDesktop 独立进程模式)
  │     windows/win_platforms.cpp    Calc 工厂; Impress stub
  ├── office_runtime/      OfficeRuntime target → office_runtime.so — 进程级共享运行时 (Linux 专属)
  │     OfficeRuntime: Xvfb 虚拟屏 / LO 共享内核 / Slot 分区 / 跨进程协调
  │     office_runtime_test.cpp — 严酷模式单测 (9 场景 49 检查, 统一树内编译)
  │     ffplay/            FFplay target → ffplay.so — 自治媒体后端 (独立 service 名
  │                        Manager_FFPlay; 嵌入引擎 = 官方定制 ffplay.c 补丁式复用
  │                        (compat/), 真实视频+音频播放已落地, 见经验 30/34)
  ├── calc/                CalcLink target → calclink.so — Calc 文档播放 (C ABI 导出)
  │     calc_session.cpp   会话 (加载/控制/轮询/抓帧)
  ├── impress/             ImpressLink target → impresslink.so — Impress 播放 (C ABI 与 calc 同构)
  │     impress_session.cpp 会话 (加载/窗口化幻灯片/翻页/轮询抓帧)
  ├── source/              旧方案: 独立进程播放 (保留, 待废弃)
  └── writer/              未创建 (目标: writerlink.so, 同构 calc/impress)
```

**上层**:`NovaOfficeCore/ppt/LibreOfficeImpressManager`(dlopen impresslink,CalcManager 同构);`excel/LibreOfficeCalcManager`(dlopen calclink)。分发点在 `PptCoreExport.cpp` 的 `PPT_PLAY_MODE_ANIMATION_LIBREOFFICE` 分支。

**共享内核模式(Linux)**:进程内一个 LO 内核(自研 `BootstrapOffice` 引导,复制官方 cppu::bootstrap 逻辑,支持可选独立 UserInstallation,见经验 23)+ 一个 Xvfb 大屏(静态画布,默认 `max_docs=8 × max_doc_width=3840 × max_doc_height=2160` = **30720x2160**,8 个 2160p 子屏位),多文档窗口动态落位在子屏位内(实际分辨率 ≤ 2160p),互不重叠。调用者只需知道:最大并发文档数 + 每文档最大分辨率。Windows 保持每 session 独立 soffice 进程 + 独立桌面(不参与 office_runtime)。

### 1.2 已验证能力(勿破坏)

- 单/多会话探针:`impress_multi_probe`(2 xlsx + AI时代.pptx 并发,`[n]` 参数控制会话数;含转场 pptx 需 SAL_DISABLEGL,office_runtime 已内置,见经验 21)、`media_green_probe`(媒体后端回归, 帧间差异判据, 经验 30/34)。注:calc_link_test/calc_multi_probe/impress_probe/byte_order_probe 源码已过时(与新 calclink.h 签名不匹配),build_probes.sh 未登记,仅旧二进制可手动跑
- Impress 最小验证:`impress_nextpage_probe`(翻页/卡死定位);impress_probe(窗口化幻灯片 29 页/翻页/效果/翻回一致/slot resize 内容重排)仅旧二进制
- Xvfb 机制探索:`xvfb_stress_probe`(尺寸上限/抓帧性能/XShm/并发)
- office_runtime 单测:`office_runtime_test` 9 场景(bootlock/slots/crossproc/acquire/adopt/dirtyenv/faultinj/linksmoke/gstcheck)49 检查,`--stress N` 压力模式;前提:无其他 office_runtime 使用者
- Demo 实测:2 xlsx + AI时代.pptx 三画面正常、PPT 翻页正常(LibreOffice 动画模式;`PPTPlayerItem.cpp` 的 LUID 默认已改 false,否则 OpenGL 下 CPU 帧断言崩溃)
- 抓帧性能:XShm 后 1080p ~1ms/1440p ~2.5ms/2160p ~5.9ms(原 9/16/27ms)
- 媒体页(含视频)翻页:改 gstplayer.cxx(ximagesink)+ 增量编译替换 libavmediagst.so 后,AI时代.pptx 15 次翻页含媒体页 8/11/12/13 全过(探针 2 轮 + demo 稳定)
- ffplay 软解:Xvfb 上播放 sintel_trailer 正常(窗口/画面/退出码 0)—— ffmpeg 软解 + X11 渲染可靠性背书

### 1.3 构建/部署/验证 (2026-08-13 起: 统一构建树, 见经验 31)

```bash
# 统一构建 (唯一入口, 单一树 debug/; 旧 build_links_linux.sh 已退役)
cd NovaPlayerTools && ./build_in_linux.sh
# 产物: 四件套 .so 直接输出到部署目录 office/program (单副本, per-target
#       LIBRARY_OUTPUT_DIRECTORY = OR_/CALCLINK_/IMPRESSLINK_/FFPLAY_DEPLOY_DIR;
#       POST_BUILD 拷贝保留但幂等)。旧可执行: NovaLibreOfficePlayerDeprecated
#       (输出 bin_x86_64_kylin/, 二进制名不变)。注意: 若 .so 双份存在
#       (bin 根 + office/program), demo 的 LD_LIBRARY_PATH=$CURDIR 优先加载
#       bin 根版 -> dladdr(GetRuntimeDir) 错位 -> UNO_PATH 错 -> bootstrap rc=4
#       (2026-08-14 实测踩过, 见经验 31/33)

# 单 target 定向构建
cd NovaPlayerTools && cmake --build debug --target OfficeRuntime CalcLink ImpressLink FFplay -j$(nproc)

# office_runtime 单测 (统一树产物在 NovaPlayer/bin_x86_64_kylin/; 前提: 无其他使用者)
# 注意: 部署目录是 NovaPlayer/bin_x86_64_kylin (不是 NovaPlayerTools/);
#       测试/探针二进制已烧绝对 RUNPATH, 可免 LD_LIBRARY_PATH 直接跑 (推荐,
#       相对路径 LD_LIBRARY_PATH 会使 UNO_PATH 相对化 → bootstrap 崩, 经验 36)
rm -f /dev/shm/nova_office_slots_v1 /dev/shm/sem.nova_office_boot /tmp/.X9*-lock
NovaPlayer/bin_x86_64_kylin/office_runtime_test [--stress N]

# 探针编译 (项目外调试工具, 在仓库根 /home/hido/NovaPlayerProject/xvfb_calc_demo/,
# 不参与统一树; 登记表见脚本注释, 过时探针不登记)
cd /home/hido/NovaPlayerProject/xvfb_calc_demo && ./build_probes.sh [probe_name ...]

# 并发回归 (2 xlsx + pptx) — 跑前清场 (含 socket, 经验 35)
for p in $(pgrep -x Xvfb); do kill -9 $p; done; rm -f /tmp/.X9*-lock /tmp/.X11-unix/X9*
NovaPlayer/bin_x86_64_kylin/../xvfb_calc_demo/impress_multi_probe \
  "/home/hido/文档/志愿分析.xlsx" "/home/hido/文档/7-8月报销明细表-正式版.xlsx" "/home/hido/文档/AI时代.pptx"

# 媒体后端回归 (media_green_probe: 帧间差异判据, 经验 34; green% 兼容保留)
xvfb_calc_demo/media_green_probe "/home/hido/文档/AI时代.pptx"   # 默认 ffplay: 媒体页 diff 高 = 视频在动
ORT_MEDIA_BACKEND=gstreamer xvfb_calc_demo/media_green_probe \
  "/home/hido/文档/AI时代.pptx"                                   # 显式回退 gstreamer
```

### 1.4 关键文件地图

- `office_runtime/office_runtime.h/.cpp` — 运行时(生命周期/内核/Xvfb 共享/adopt/slot shm/BootLock/清场/僵尸判定);`BootstrapOffice`(自研内核引导,支持独立 profile,经验 23);`EnsureKernel` 内置 `SAL_DISABLEGL` + `ORT_MEDIA_BACKEND`(经验 21/30)
- `office_runtime/office_runtime_test.cpp` — 严酷模式单测(9 场景 49 检查;统一树 target `office_runtime_test`)
- `office_runtime/ffplay/` — ffplay 自治媒体后端(manager/player/window + rdb + CMakeLists + **compat/**(ffplay.c/cmdutils/config.h/ffplay_embed.patch/ffplay_engine.h 引擎));经验 29/30/34
- `xvfb_calc_demo/build_probes.sh` — 探针编译脚本(项目根 xvfb_calc_demo/ 不在 NovaLibreOfficePlayer 下!)
- `common/link_platform.h` — 统一平台接口 (原 CalcPlatform/ImpressPlatform 合并, 经验 32)
- `common/linux/xvfb_platform.h/.cpp` — XvfbSessionPlatform 单类: Xvfb 连接/slot 定位/窗口查找/落位/**XShm 抓帧+字节序直拷**(规则参数化, MatchWindow 逃生口虚函数)
- `common/linux/linux_platforms.cpp` — 工厂: CalcRule/ImpressRule (窗口匹配规则即文档类型差异)
- `common/windows/win_platform.h/.cpp` — WindowsPlatform (CreateDesktop 独立进程模式, calc 实现迁移; impress stub)
- `common/link_utils.h/.cpp` — u2s/s2u + HideUiBlock (UI 隐藏三件套)
- `common/log.h` — OfficeLog 声明 (实现唯一在 office_runtime.so, 勿在 common 编译第二份)
- `common/cmake/FindLibreOfficeSDK.cmake` — SDK 查找模块 (单份, 缓存自愈, 经验 33)
- `calc/calc_session.cpp` — Calc 会话(boot 段用共享 BootLock、Hidden 加载、滚动/切表/缩放)
- `impress/impress_session.cpp` — Impress 会话(窗口化幻灯片 XPresentation2、XSlideShowController 翻页)
- `NovaOfficeCore/ppt/LibreOfficeImpressManager.cpp` — 上层 Manager(dlopen impresslink)
- `NovaOfficeCore/ppt/PptCoreExport.cpp` — NPpt 分发(ANIMATION_LIBREOFFICE → LibreOfficeImpressManager)
- `NovaPlayerDemo/player/PPTPlayerItem.cpp` — Demo PPT 播放器(LUID 默认 false 修复)
- `xvfb_calc_demo/` — 探针集(impress_probe/impress_multi_probe/impress_nextpage_probe/xvfb_stress_probe/byte_order_probe/bmp_analyze.py + `lo_build/` 编译命令备份)

### 1.5 当前运行状态与待清理

- **媒体后端选择开关已落地 (方案 A, 2026-08-13, 经验 30)**:LO `mediawindow_impl.cxx`(libavmedialo.so)读 `ORT_MEDIA_BACKEND`;office_runtime `EnsureKernel` 默认 **`ffplay`**(2026-08-14 切换, gstreamer 不满足需求; 宿主 export 可回退 gstreamer)。媒体页**真实视频+音频播放**(ffplay 嵌入引擎, 经验 34; 帧间差异判据)
- **媒体页问题已修复**(gstplayer ximagesink)并部署:部署侧 `office/program/libavmediagst.so` 为修复版;`libavmedialo.so.bak`/`libavmediagst.so.bak` 为备份(libavmedialo.so.bak 为 ORT_MEDIA_BACKEND 补丁前版本)
- **LO 源码两处改动已同步远端 (2026-08-17 确认)**:同在 commit `83e0b9c3e [feat][NA]:增加ffplay媒体播放后端`(gstplayer.cxx ximagesink + mediawindow_impl.cxx 后端选择),master 已推 origin(git.novatools.vip/caolei/libreoffice.git),工作树干净 —— 3.1 的 ★★★ 同步项完成
- **单测/探针加固 (2026-08-17, 经验 35/36/37)**:office_runtime_test 竞态修复(CleanXvfbBattlefield 等死透+清 socket/SafeKill 灭组防御/SpawnOrphan 读回校验/断言 display 号感知),修复后 8/8 轮稳定全绿(修复前偶发 SIGKILL 全家死);xvfb_stress_probe 判空+socket 清理(脏环境从段错误降级为可诊断退出);ffplay_engine_probe duration 查询时机修正。office_runtime 产品代码**零改动**(其 lost-race 换号自愈经确认正确)
- **GL 转场已禁用**(office_runtime 内置 `SAL_DISABLEGL=1`,经验 21):带转场 pptx(morph/fade)在 Xvfb 下不再崩;转场退化为 CPU 渲染
- 日志已统一:`[ORT]` 前缀 + 本地时间戳 + 英文(2026-08-13);防御分支可见:重复 FreeSlot(owner 不匹配)/多余 Release(ref_count 0)会打印 warning,探针销毁路径存在重复释放(无害,防御拦截)
- **LO 源码本地已改两处**(远端构建需同步;已入本地 LO 仓库 staged,待 commit):① `avmedia/source/gstreamer/gstplayer.cxx`(ximagesink 优先,经验 18)② `avmedia/source/viewer/mediawindow_impl.cxx`(ORT_MEDIA_BACKEND 后端选择,经验 30)。**build_libreoffice.sh 可正常跑**(无 set -e,tar 失败不中断):`git.tar.gz` 只是构建前临时恢复官方 git 元数据供 autogen 查询源码版本/依赖网址(README.txt 明说构建完成后本来就会删 .git)——**工作树补丁改动不丢**(在文件系统,不在 .git);唯一损失是 .git 历史/staged 记录(且 git.tar.gz 在时跑完也会删,属设计行为)。在意 git 历史就提前 commit/stash 或备份 .git。增量编译仍推荐模块级 make(快): `cd build/libreoffice_core/avmedia && export LD_LIBRARY_PATH=build/gcc/lib:build/gcc/lib64 && make -j8`(产物 instdir/program/,手工替换部署)
- 诊断日志:`[CALC-T]`/`[IMP-T]`/`[ORT]`/FindCalcWindow scan/GrabBgra 诊断(fprintf stderr)定位完成,建议清理;`[SCROLL]`/`[CALC-INST]` 有线上诊断价值,保留
- **所有者退出连坐**:共享内核/共享屏的所有者进程退出时,其他进程的会话随之断开(内核随管道退出)—— 对单进程部署无影响,双实例只建议同时使用
- Windows 回归:EnsureKernel 空实现 + impress stub,需 Windows 编译确认
- office_runtime 的 UNO_PATH/URE_BOOTSTRAP 依赖部署位置(office/program,EnsureKernel/BootstrapOffice 内),部署路径变化需同步
- **引导方式认知对齐 (2026-08-13, 换 BootstrapOffice 后实测确认)**:soffice.bin 启动参数/环境与官方 cppu::bootstrap 完全一致(osl_executeProcess env 继承同款)——① run.sh 的 LD_LIBRARY_PATH/libstdc++(program 自带 6.0.30 > 系统 6.0.28,gcc12 符号需求)**与引导方式无关**,是 LO 库在宿主/内核进程加载的必然;② GST 环境变量传递机制不变(经验 19① 的"无效"是 gst 1.16.3 内部机制,与引导无关);③ ffplay 注入(unorc/UNO_SERVICES)路径不变(soffice.bin 启动参数一致);④ SAL_DISABLEGL 新路径生效(实测内核 env 含);⑤ **新要求**:不经 office_runtime 的 UNO 客户端进程须 `UNO_PATH` + `URE_BOOTSTRAP`(经验 23 两个坑);⑥ 引导耗时 1004→504ms
- 注释错位已修 (2026-08-13): office_runtime.h 生命周期(末个 Release 不销毁, 进程级存活 + atexit 清理)、Instance 注释的 PDEATHSIG 残留表述(已弃用, 经验 3)、媒体"LD_PRELOAD 劫持"注释(实际是 gstplayer.cxx 改码 + libavmediagst.so 替换部署, 经验 18)

---

## 二、历史经验总结(过去)

> **时间**:提出/验证时间。**置信度**:高 = 源码级确认或多次实测稳定;中 = 单次实测或行为有波动;低 = 推断或未完全验证。
> 每条都踩过坑,勿回退。

### 2.1 运行时架构与生命周期

| # | 经验 | 时间 | 置信度 |
|---|---|---|---|
| 1 | **Xvfb 窗口存储缺陷**:`XGetImage(被遮挡窗口)` 返回背景;root 区域抓帧在窗口重叠时串画面。解法:大屏 + slot 分区,窗口天然不重叠,直接 `XGetImage(窗口)`。(另:遮挡场景 XShm 同样返回背景,raise 亦不可靠 —— 2026-08-12 补充实测) | 2026-08-12 前 | 高 |
| 2 | **OfficeRuntime 必须是共享实例**:`OfficeRuntime::Instance()`(so 内函数静态)—— "so 单例"机制 | 2026-08-12 前 | 高 |
| 3 | **PDEATHSIG 多线程误杀 Xvfb**:改用 `std::atexit` 清理(崩溃遗留孤儿 Xvfb,靠下次启动清理) | 2026-08-12 前 | 高 |
| 4 | **进程退出时不能析构 LO 内核**:静态析构阶段 UNO 调用段错误。`Instance()` 故意泄漏,OS 回收 | 2026-08-12 前 | 高 |
| 7 | **Hidden 加载**:slot 方案下 Hidden 加载正常;可见加载不再需要 | 2026-08-12 前 | 高 |
| 9 | **退出时 X IO Error 噪音**:atexit 杀 Xvfb 时 LO 连接断开打印,无影响 | 2026-08-12 前 | 高 |
| 19c | **会话重建不设**:对确定性故障(如媒体页崩溃)重建到同一页仍崩,语义不清晰;改为"崩溃检测 + 明确告警",聚焦根因修复 | 2026-08-12 | 高(讨论定案) |

### 2.2 跨进程协调(共享屏/内核/slot)

| # | 经验 | 时间 | 置信度 |
|---|---|---|---|
| 5 | **并发 Create 必须串行化引导+加载**:`OfficeRuntime::BootLock` —— 进程内 so 单例 mutex + **跨进程命名信号量**(sem_open "/nova_office_boot",60s 兜底强制恢复)。锁必须由 office_runtime 提供,各 link 私有锁无法跨 link/跨进程互斥(踩过:file-static 锁导致并发引导卡死) | 2026-08-12 前 | 高 |
| 6 | **Xvfb 锁文件**:SIGKILL 退出留残留锁,启动时检查锁内 PID **是否存活且非僵尸**(`kill(pid,0)` 对僵尸仍成功,须查 /proc stat 的 Z;踩过:僵尸被当活服务器 → adopt 分支 WaitForX 失败 → 显示号漂移) | 2026-08-12 | 高 |
| 10 | **共享虚拟屏是机器级的**:发现活 Xvfb(锁内 PID 可服务)直接 **adopt**(`[ORT] adopted shared Xvfb`),不再另起 —— 否则新进程的会话(按默认 profile 复用已运行内核)渲染在别人的屏上,自己抓帧全空(踩过:双实例全黑) | 2026-08-12 | 高 |
| 11 | **slot 位图必须跨进程共享**:POSIX shm(`/dev/shm/nova_office_slots_v1`,flock 保护,owner 记 PID,崩溃/僵尸自动回收)—— 各进程私有位图会重复分配同一 slot,窗口重叠(踩过) | 2026-08-12 | 高 |
| 12 | **启动清场**:`Acquire` 首调用时 `CleanupOrphanSoffice()` —— 清理孤儿 soffice.bin(判定:cmdline 含 office/program/soffice.bin + ppid==1 或 DISPLAY 的 Xvfb 已死;活内核绝不误杀) | 2026-08-12 | 高 |
| 22 | **同 UserInstallation 必复用(转发机制,独立于锁检查)**:soffice 启动时 `RequestHandler::Enable()`(officeipcthread.cxx:740)按 `SingleOfficeIPC_<md5(user_install)>` 命名 pipe 判断:创建成功=第一实例;失败则连接已有实例 → **转发命令行并自我终止,等待第一实例执行**(app.cxx:524 IPC_STATUS_2ND_OFFICE)。`--nolockcheck`/`--headless` 均拦不住。实测:播放内核占住 office/user 时,默认 profile 的 `soffice --convert-to` 等待 9-27s(排队)且零本地 CPU,内核退出则请求丢失(PDF 不生成)。**推论:任何不带 -env:UserInstallation 的 soffice 调用都会污染共享内核;转换必须显式独立 profile** | 2026-08-13 | 高(源码+实测) |
| 23 | **多内核/独立 profile 的 bootstrap(已落地 office_runtime)**:复制 cppu::bootstrap 逻辑(cppuhelper/source/bootstrap.cxx:84-229)到自研 `BootstrapOffice`,在 args 加 `-env:UserInstallation=file://<独立profile>` 即可引导独立内核,零 LO 源码改动。**两个坑**:① 必须先 `rtl::Bootstrap::set("URE_BOOTSTRAP", encode(<program>/fundamentalrc))` 再建本地上下文(宏展开器依赖它解析 unorc 的 `${ORIGIN}`;缺失时 UNO_TYPES 解析不全 → binaryurp `Marshal::writeType` 段错误,栈:libbinaryurplo.so);② 客户端进程须 `setenv UNO_PATH=<program>`(注意: UNO_PATH 必须指向含 soffice 的 program 目录, 见经验 33 的 GetRuntimeDir 错位坑)。启动用 osl_executeProcess(官方原样)。连接串 `uno:pipe,name=xxx;urp;StarOffice.ComponentContext` + `UNO_QUERY_THROW`(ServiceManager/Object 均踩过坑)。**落地**:`OfficeRuntime::EnsureKernel(user_installation="")` 已替换官方 cppu::bootstrap;回归全过(单测 47→现 49 + 压力 107 + calc/impress 探针 + 转换探针),引导耗时 504ms(旧路径 ~1004ms)。独立 profile 转换验证:office/user 不被写 | 2026-08-13 | 高(源码+实测) |
| 24 | **环境变量"必须写 run.sh"的认知修正(双实验验证)**:soffice.bin 的 env = 启动时快照(osl_executeProcess 继承宿主)。① 传递层:office_runtime 引导前 setenv 的 `GST_REGISTRY` 实测出现在 soffice.bin 的 /proc/pid/environ;② 读取层:同一变量被 soffice.bin 内 gst 真正读取(进媒体页后写 registry 文件,1MB)。**结论:LO/gst 内部 getenv 读取的变量(SAL_*/GST_*/UNO_* 等)只需在 EnsureKernel 引导前 setenv 即可,不必写 run.sh**——SAL_DISABLEGL 即此机制活例。**例外(必须进程启动前/run.sh)**:LD_LIBRARY_PATH/libstdc++(program 自带 6.0.30 > 系统 6.0.28,gcc12 符号需求)与 LD_PRELOAD 属动态链接器,启动时决定,任何 setenv 无效。经验 19① 的"gst 变量无效"澄清:变量传递与读取均正常(本次实测),无效的是 gst 1.16.3 对 rank 等变量的响应机制本身 | 2026-08-13 | 高(双实验) |
| 25 | **run.sh 的 office/program 路径不能删(实测, 结论反转)**:原因是 **libstdc++ SONAME 单例** —— 宿主(Qt)启动即加载系统 libstdc++ 6.0.28,后续 dlopen calclink 时 `libstdc++.so.6` 已加载则复用系统版, libuno_cppu 缺 `GLIBCXX_3.4.29` 符号 → dlopen 失败("version not found")。**教训(假阳性)**:纯 C dlopen 测试进程不预加载 libstdc++, 掩盖了单例问题; 必须用"预加载系统 libstdc++ 再 dlopen"的宿主等价测试(rt_test 复现与 demo 一字不差)。**可行替代(构建侧, 已落地 2026-08-14)**: links 已加 `-Wl,-rpath,${DEPLOY_DIR}` + 显式链接 `${DEPLOY_DIR}/libstdc++.so.6`(rt_test 验证 dlopen 成功; 机制见经验 31⑤)—— 机制是主程序 RUNPATH 参与间接依赖解析, 整个进程统一用 program 的 libstdc++ 6.0.30(与 LD_LIBRARY_PATH 效果等价, 无精度提升; 6.0.30 向后兼容, 系统编译器构建的宿主无兼容性问题, run.sh 长期运行即实证)。**保留**:$CURDIR(ffmpeg 宿主库)必须; soffice.bin 侧靠 soffice 脚本自设 LD_LIBRARY_PATH(soffice:154)不依赖宿主。**陷阱**:env -i 缺 LANG 时 LO 报 "type detection failed"(loadenv.cxx:189, 2ms 失败)—— 是 locale 缺失, 与库路径无关, 二分时勿误判 | 2026-08-13 | 高(实测) |
| 31 | **统一构建树整合(2026-08-13)**:NovaLibreOfficePlayer 整体并入 NovaPlayerTools cmake 单一树(`build_in_linux.sh` → debug/), 无独立 build 树; `build_links_linux.sh` 退役, ABI 同步由依赖图承接(CalcLink/ImpressLink target 链接 OfficeRuntime)。**结构**: NovaLibreOfficePlayer/CMakeLists.txt 为构建伞 — 旧可执行 target 改名 `NovaLibreOfficePlayerDeprecated`(OUTPUT_NAME 不变, configure 期 WARNING)+ `add_subdirectory(office_runtime|calc|impress|office_runtime/ffplay)`; office_runtime 内还编译 `office_runtime_test` target。**坑**:① `NovaPlayerToolsBase.cmake` 的 `target_link_options(${PROJECT_NAME})` 依赖 target 名==project 名, target 改名后需手动等价应用(-Bsymbolic), 且须在 add_executable 之后; ② 顶层 `CMAKE_*_OUTPUT_DIRECTORY_DEBUG` = bin_x86_64_kylin: 产物直接输出到 bin 目录(**已被经验 33 修正**: 四件套 per-target 输出目录=部署目录单副本, 与 1.3 一致); ③ links 链 **OfficeRuntime target** 得 `DT_NEEDED=office_runtime.so`(ld 对直接 .so 输入按 basename 记 NEEDED, 无 SONAME 时), 运行时经 RUNPATH(部署目录)解析, 语义与旧"链部署文件"完全一致且构建顺序自动; ④ cmake 会附加 build 树 RUNPATH 条目(sdk/lib 等), 无害; ⑤ ffplay CMakeLists 链 ffmpeg 库需 `-Wl,--no-as-needed`(绿壳无符号引用会被剥离 NEEDED), RUNPATH `$ORIGIN:$ORIGIN/..:$ORIGIN/../..`——ffmpeg 库在 bin 根(部署目录上**两级**), $ORIGIN/..(=office/) 解析不到, 实际还靠 soffice 脚本自设 LD_LIBRARY_PATH 含 bin 根双保险 (2026-08-14 审查修正); ⑥ 探针(xvfb_calc_demo/, **项目根不在 NovaLibreOfficePlayer 下**)不参与统一树, 用 `build_probes.sh` 编译(登记表: media_green/ffplay_inject/xvfb_stress/impress_multi/impress_nextpage/ffplay_engine(引擎探针需 compat 源, gcc 编 C 源); calc_multi_probe 等旧探针源码已与新 calclink.h 签名不匹配, 未登记); ⑦ **构建目录**: 探针/单测产物在 bin_x86_64_kylin/; ⑧ **Debug 树产物带符号**(.so 8MB 级), Release 部署时用 Release 配置 | 2026-08-13 | 高(实测) |
| 32 | **平台层归组重构(2026-08-14, 消除 77% 重复)**:平台实现按"环境"归组到 `common/`(不再按文档类型目录): `calc/linux`、`calc/windows`、`impress/linux`、`impress/windows` 与 `calc_platform.h`/`impress_platform.h` 删除。**结构**: `common/link_platform.h`(统一接口, 原两接口 90% 同构合并, FindWindow 统一命名)、`common/link_utils`(u2s/s2u/HideUiBlock UI 隐藏三件套, 原会话各一份)、`common/log.h`(OfficeLog 声明下沉, **实现保持 office_runtime.so 单例**——STATIC 复制会导致 calclink/impresslink 各自初始化 spdlog 双写日志, 勿在 common 编译第二份)、`common/linux/xvfb_platform`(XvfbSessionPlatform **单类参数化**: WindowMatchRule(关键词)+tag+profile 子目录为构造参数, 无子类; MatchWindow 保留 virtual 作逃生口)、`common/linux/linux_platforms.cpp`(工厂: 规则即文档类型, 各 2 行)、`common/windows/win_platform`(calc Windows 实现迁移, CreateDesktop 模式, profile 子目录参数化)。**会话层不强提基类**(calc 滚动/表格 vs impress slideshow 差异本质)。**依赖方向**: common(基础, 零依赖 office_runtime)← office_runtime(运行时)← links(客户端); ffplay 归属 office_runtime 子模块(伞形不再跨层级 add_subdirectory)。**CMake**: common STATIC 库, CalcLink/ImpressLink 链之; FindLibreOfficeSDK.cmake 移入 common/cmake(原 calc/impress 双份 md5 相同)。**验证**: 重构后全量回归(单测 49/并发/全绿双态)与重构前一致; Windows 侧为机械改名, 需 Windows 编译确认 | 2026-08-14 | 高(实测) |
| 33 | **.so 加载路径错位 + CMake 缓存失效(2026-08-14, 两条实战坑)**:① **GetRuntimeDir 错位**: office_runtime 用 dladdr 自定位(GetRuntimeDir), 若 .so 双份存在(bin 根 + office/program), demo 的 `LD_LIBRARY_PATH=$CURDIR` 优先加载 bin 根版 → UNO_PATH 指向 bin 根(无 soffice) → osl_executeProcess rc=4 全部会话失败(探针单路径 LD_LIBRARY_PATH 不暴露)。**修复**: 四件套 per-target `LIBRARY_OUTPUT_DIRECTORY` = 部署目录(单副本); 遗留 bin 根旧 .so 需手动清理。② **FindLibreOfficeSDK 缓存失效**: 模块移动(calc/cmake → common/cmake)后 CMakeCache 残留旧路径, `if(NOT LIBREOFFICE_SDK_ROOT)` 遇非空缓存跳过重推导 → 路径失效 FATAL。**修复**: 模块内加缓存自愈(`NOT EXISTS` 时 FORCE 重推导, LIBREOFFICE_UNO_INCLUDE 同款)。**另**: ffmpeg 库在 bin 根(部署目录上两级), ffplay 的 RUNPATH 应为 `$ORIGIN:$ORIGIN/..:$ORIGIN/../..`, $ORIGIN/..(=office/) 解析不到; 实际运行还靠 soffice 脚本自设 LD_LIBRARY_PATH 含 bin 根双保险 | 2026-08-14 | 高(实测) |
| 35 | **Xvfb SIGKILL 垂死窗口竞态家族(2026-08-17 定性+修复, 全部在测试/探针侧)**:① **垂死窗口**: 大屏 Xvfb(30720x2160, ~300MB 映射)被 SIGKILL 后死透需时间, 窗口内 socket 仍监听 → 同号立即重启必 "Cannot establish any listening sockets / server already running" 失败(实测两次: adopt/dirtyenv 孤儿 Xvfb 起不来 → 断言连锁)。**残留 socket 文件**(无活 server)无害, X server 启动会自清; 真凶是垂死窗口。**修复**: 测试 CleanXvfbBattlefield/SafeKill、探针 StopXvfb 均改为"杀 → ProcAlive 轮询等死透(≤2s) → 清 lock+socket"; waitpid 对非子进程(双 fork 孤儿)直接 ECHILD 等不到死, 必须轮询。② **kill(0)/kill(-1) 灭组事故**: pid 来自 lock 文件或管道读回, 竞态下可能是 0/-1 —— `kill(0)` 杀整个进程组、`kill(-1)` 杀全部可杀进程(实测: 测试进程+同组 tail 全家 SIGKILL, 无 core 无日志)。**修复**: SafeKill 统一 guard(pid>0 && pid!=getpid()) + SpawnOrphan 管道读回校验 + faultinj 不假设 :90(display 号感知, lost-race 换号是 office_runtime 的正确自愈)。**office_runtime 产品侧零改动** —— 其 StartXvfb 的 lost-race 换号重试(扫 90-99)在污染环境下行为正确 | 2026-08-17 | 高(两次实测复现+8/8 验证) |

### 2.3 抓帧与性能

| # | 经验 | 时间 | 置信度 |
|---|---|---|---|
| 13 | **XShm 抓帧 + 字节序直拷**:Xvfb TrueColor 24bpp 视觉是 32bpp LSBFirst BGRX 布局 → `XShmGetImage`(零拷贝,0.01ms)+ memcpy+alpha 填充(1080p 共 ~1ms vs 原 XGetImage+mask 转换 ~9ms)。非 BGRX 布局或 XShm 不可用自动回退。link 的 CMakeLists 必须链 X11::Xext(XShmQueryExtension,踩过 undefined symbol) | 2026-08-12 | 高 |
| 14 | **屏高必须 ≥ 最大文档分辨率**:2160p 窗口在 1080 高屏上 BadMatch。分辨率定位 1080p/1440p/2160p ⇒ 共享屏 7680x2160(已验证可起)。`StartXvfb` 已按 `config_.max_doc_height` 定高(默认 2160) | 2026-08-12 | 高 |
| 15 | **屏尺寸上限**:16 位坐标上限 32767 前无阻碍;30720x2160(8 docs × 2160p)RSS ~300MB 已验证可起 | 2026-08-12 | 高 |

### 2.4 媒体播放(最大战线的经验区)

| # | 经验 | 时间 | 置信度 |
|---|---|---|---|
| 17 | **LO 媒体页播放(含视频的 PPT)**:LO avmedia 后端选择走 UNO service 名(`com.sun.star.comp.avmedia.Manager_GStreamer`,mediawindow_impl.cxx:196);gstreamer 后端的 video-sink 在非 GTK 平台回退 `autovideosink`(gstplayer.cxx:888,gen 平台 CreateGStreamerSink 返回 null)。**Xvfb 无 GPU 环境下 autovideosink 选中 glimagesink → EGL→swrast 段错误(soffice.bin 崩 → bridge disposed → 会话全毁);且媒体页渲染失败导致 slideshow 事件链阻塞(gotoNextEffect 挂起)** | 2026-08-12 | 高(源码级) |
| 18 | **媒体修复(已验证,改 LO 源码)**:`gstplayer.cxx` 回退分支优先 `ximagesink`(支持 video-overlay、无 GL;失败回退 autovideosink)—— 崩溃与挂起同时消除,探针/demo 稳定。**生效组件是 `libavmediagst.so`(不是 libavmedialo.so!)**;改码 + 增量编译(build_libreoffice.sh --build-gcc=no)+ 替换部署 | 2026-08-12 | 高(demo 稳定) |
| 19b | **Xvfb 恒无 GPU,媒体 sink 固定 ximagesink(不做动态决策)** —— "有 GPU 用 GL / 无 GPU 用软件"的决策是伪需求,部署环境固定。修复已在 LO 源码层固定 | 2026-08-12 | 高 |
| 19 | **媒体方案弯路(勿重走)**:① gst 环境变量禁用 GL(`GST_PLUGIN_FEATURE_RANK`/`GST_GL_PLATFORM`/`LIBGL_*`/`EGL_PLATFORM`)在此 gst 1.16.3 全部无效(rank 实测不变;gstgl 1.16 无平台 env);② 卸载 gstreamer1.0-gl → 媒体页挂起重现(挂起与 GL 无关,是渲染失败);③ LD_PRELOAD 拦截 `gst_element_factory_make` 不可靠(符号解析随机,拦截不是 100% 生效);④ VirtualGL/Xvfb+GLX/EGL 离屏均不可行;⑤ ffplay 软解验证通过(ffmpeg 软解 + X11 渲染在 Xvfb 可靠) | 2026-08-12 | 高(全部实测) |
| 19d | **ffplay.so 组件替换(已被经验 29/30 取代, 勿重走)**:实现 `com.sun.star.comp.avmedia.Manager_GStreamer` 同名 service,office_runtime 在 bootstrap 前设 `UNO_SERVICES=ffplay.rdb <原services>`(我们的 rdb 排前,servicemanager 同名取 vector[0],机制已源码级确认)—— **LO 源码零改动,无感知替换**;收益:与 gstreamer 生态彻底解耦(软解 + XPutImage,依赖仅项目自带 ffmpeg),并可增强(帧访问/硬解/媒体信息) | 2026-08-12 | 高(机制源码级) |
| 19e | **注入可靠性已实测(最终未采用此机制, 落地=独立 profile 注册 + LO 源码开关, 见 28/30)**:UNO_SERVICES 环境变量被 soffice.bin bootstrap 读取(坏值即 `cannot open <rdb>` 报错)—— 注入链路两环节(变量读取 ✓ 实测 + 同名取 vector[0] ✓ 源码)全部验证 | 2026-08-12 | 高 |
| 19f | **ffplay 最小壳已就绪(office_runtime/ffplay/)**:ffplay_manager/player/window.cxx + ffplay.rdb(XML uno-components)—— 编译卡点与备选方案见 19g | 2026-08-12 | 高(代码就绪) |
| 19g | **ffplay 编译已走通(bear 方案)**:bear 2.4.3 用法为 `bear make`(不支持 `--`);LO 构建树拷到新机器后 make 失败需补 `sources.ver`(echo 'lo_sources_ver=24.2' > sources.ver,无 .git 走 tarball 分支);bear 的 compile_commands.json 提取 gstplayer 编译参数(LO 完整标志:gcc12+config_host+include+comprehensive/udkapi 双 include),改造后成功编译 ffplay.so(接口坑:GreenWindow 须实现 XPlayerWindow 而非 XWindow;XPlayerWindow 继承 XWindow+XComponent+update/setZoomLevel/getZoomLevel/setPointerType;ZoomLevel 枚举在 css::media 顶层非枚举类内)。**注入运行卡点**:UNO_SERVICES 覆盖后 soffice 启动崩;改 unorc 追加 ffplay.rdb → 启动成功但 slideshow 阶段 abort。**已被经验 29/30 取代**(SDK 外部编译 + 独立 service 名 + 环境变量开关, 勿重走 unorc 注入路径) | 2026-08-12 | 中(编译高,运行期未通) |
| 20 | **幻灯片属性对齐旧方案**:`IsEndless=true`(循环,窗口保活)+ AllowAnimations/IsAlwaysOnTop/StartWithNavigator/UsePen —— 与 source/Communicator.cpp 一致 | 2026-08-12 | 高(语义对齐) |
| 21 | **GL 转场崩溃(Xvfb 无 GPU 的第二个 GL 点,媒体之外的独立必崩路径)**:slideshow 无条件加载 `com.sun.star.presentation.TransitionFactory`(ogltrans 组件 = libOGLTranslo.so,slideshowimpl.cxx:611);`hasTransition()` 首查 `OpenGLHelper::supportsOpenGL()`(TransitionerImpl.cxx:1193),而 X11 generic 平台 **无条件** `m_bSupportsOpenGL = true`(vcl/unx/generic/app/salinst.cxx:77,不看 Xvfb 有无 GLX)→ 带 morph/fade 转场的 pptx(AI时代.pptx 实测必崩,单探针/三文档并发均复现)转场委托 OGL 实现 → EGL→swrast 崩 soffice.bin → bridge disposed 连坐共享内核全部会话。**修复(已验证)**:`supportsOpenGL()` 首行读 `SAL_DISABLEGL` 环境变量(OpenGLHelper.cxx:749),`office_runtime::EnsureKernel` 已在 bootstrap 前 `setenv("SAL_DISABLEGL","1")`(不覆盖宿主已有值;转场退化为 CPU 渲染,效果保留;无外部设置时并发回归验证通过)。部署侧 run.sh 亦建议设置(覆盖不经过 office_runtime 的启动路径)。**勿回退此 setenv** | 2026-08-13 | 高(源码级 + 探针验证) |
| 26 | **窗口黑边/全屏瞬态/串流的三层机制(2026-08-13 探针+日志实测)**:① **全屏瞬态**:impress 窗口创建时铺满屏(30720x2160)——根源是 user 配置 `ooSetupFactoryWindowAttributes`(PresentationDocument)记录的"上次窗口状态 30720x2160";**结构修复**:该配置改为 `10,1,1920,1080;1;,,,;`(与 Calc 同款)后 LO 创建即 1920x1080,全屏瞬态/OVERLAP 瞬态/串流从源头消失。② **23px 黑边(旧 user 配置产物)**:并发 Create 时 impress 内容高 1057(23px 黑边),串行时 1077;新 user/无 user 时也 1077——23px 来自旧 user 的 UI 状态残留。**结构修复(UNO 动态隐藏 UI)**:`setMenuBar(null)` + `XLayoutManager::hideElement(menubar)` + `.uno:FullScreen` dispatch(calc_session 同款,已移植 impress_session)——旧 user 下也消除 23px(1077),不依赖 user 配置。**UNO 窗口几何不可用**:slideshow 运行中 `XWindow::setPosSize`/`setVisible` toggle 实测渲染中断(窗口全黑),勿用。③ **3px 边框(已知问题,接受)**:内容 bbox 1917x1077 vs 窗口 1920x1080,LO 窗口右/下固有边框,UNO 不可控。**诊断能力**:`DumpWindowEdges()`(边缘像素 4 边+分层+bbox)与 `CheckWindowOverlap()` 已内置 office_runtime(ORT_DUMP_WINDOWS=1 触发) | 2026-08-13 | 高(实测) |
| 27 | **播放内核独立 profile(~/.office-link/player, 已落地)**:`EnsureKernel()` 无参默认用 `~/.office-link/player` 作为 UserInstallation(BootstrapOffice 的 profile 参数)——**不再维护部署 office/user**(LO 会重建/写回运行时配置,UNO 动态隐藏 UI 后 user 只用于 LO 自身初始化);**防御**:与任何默认 profile 的 soffice 调用(外部转换/其他进程)彻底隔离(不同 UserInstallation → 不复用、不共享配置, 经验 22)。实测:内核在 ~/.office-link/player 下生成 user,office/user 不再被写,4 会话功能正常。**部署认知**:引导模式(BootstrapOffice)后,自定义 user 目录即可隔离,无需顾虑其他组件(如 NovaOfficeCore.so 转换进程)复用/干扰我们的内核 | 2026-08-13 | 高(实测) |
| 28 | **gstreamer 依赖检测 + ffplay 定位(2026-08-13; 2026-08-14 起 ffplay 为默认后端, 本条 gst 依赖清单仅回退路径需要)**:含音视频 pptx 媒体页依赖 gst,缺失时 LO 媒体页卡死/黑屏(静默失败)。**依赖清单**(部署机需 apt 安装,昨装记录):核心 `libgstreamer1.0-dev/libgstreamer1.0-0` + 插件 `gstreamer1.0-plugins-base/good/bad/ugly`(base-apps/rtp 等可选),**gstreamer1.0-gl 必须移除**(经验 19 实测崩)。**检测已内化**:`OfficeRuntime::CheckGstDeps(plugin_dir_override, detail)`(核心库 dlopen + 插件目录 + 关键插件 coreelements/playback/autodetect/typefind/videoconvert/videoscale 文件检查,参数化可测),EnsureKernel 引导时自动检测并告警(`gstreamer deps OK` / `WARNING ... may hang/black`);单测场景 `gstcheck`(49 检查含模拟缺失)。**ffplay.so 的正确定位**:系统无 gstreamer 或部署机无 sudo 权限时的**自治媒体后端**(正规同名 service 组件替换,非 hack gstreamerplayer)——检测结果驱动:gst 缺失时提示/启用 ffplay 兜底;注入路径用独立 profile 的 unopkg/registry 注册(标准方式,经验 19g 的 unorc 注入 VCL abort 已规避)。**可移除性实测(2026-08-13)**:完整卸载昨天额外安装的试验包(gl/espeak/gtk3/nice/opencv/pipewire/qt5/pocketsphinx/rtp/base-apps + 各 -dbg)后,媒体页翻页探针完全正常(20+ 页含媒体页全过),CheckGstDeps OK —— 这些包对媒体播放非必需。**媒体必需最小集**:libgstreamer1.0-0 + gstreamer1.0-plugins-base/good/bad/ugly + gstreamer1.0-x(ximagesink);gl 可移除(补丁后仍非必需,移除更安全,避免经验 19 风险)。**补丁确认**:部署的 libavmediagst.so 含 ximagesink 字符串(补丁版, md5 56f7c58d...),.bak 为未补丁版(无 ximagesink)—— 媒体正常的核心是 gstplayer.cxx 补丁(经验 18),试验包与媒体功能无关。**无插件/无 gst 行为实验(2026-08-13)**:GST_PLUGIN_PATH=/nonexistent + GST_PLUGIN_SYSTEM_PATHS= 屏蔽后,媒体页翻页仍正常(不卡死) —— 但 registry 950KB 说明空值屏蔽可能未完全生效(gst 1.16 空值回退默认路径),需改名系统 gst 库(需 sudo)才能定论"无 gst 库是否卡死"。另发现:媒体页首次初始化 gst 时 registry 重建 ~40s(950KB 扫描, 后续缓存后快)—— 独立于卡死的首次慢现象。**无 gst 库实测(改名 /lib libgstreamer-1.0.so.0, 2026-08-13)**:媒体页翻页完全正常(941 帧不卡死),CheckGstDeps 正确告警(level=1)—— **"无 gst 卡死"假设不成立**:gst 库缺失时 LO avmedia 创建后端失败但优雅降级(媒体不播,事件链不阻塞)。卡死真实机制 = gst 存在 + sink 渲染失败(GL 崩,补丁已修)。**ffplay 自治的价值修正**:不是为了防卡死(无 gst 本就不卡),而是无 gst 时提供媒体播放能力(否则媒体页空白/无媒体) | 2026-08-13 | 高(实测) |
| 29 | **ffplay 正规注入打通(SDK 模式, 独立 service 名, 2026-08-13)**:ffplay.so 实现 `com.sun.star.comp.avmedia.Manager_FFPlay`(独立名, 非 hack GStreamer), 注入验证 SUCCESS(createInstance + createPlayer 被调用, 绿色视频判据就绪)。**SDK 外部编译模式(无需 LO 内部编译/无 C++20/无内部头)** 的坑:① `cppu::WeakImplHelper` 仅 `LIBO_INTERNAL_ONLY` 定义, 外部工程用公开的 `cppu::WeakImplHelper1<Ifc>`(implbase1.hxx);② 组件库需标准导出 `component_getImplementationEnvironment`(odk/examples/cpp/counter 同款);③ rdb 的 `environment="@CPPU_ENV@"` 手写未展开(LO 构建产物才展开)会致 "cannot get environments"(shlib.cxx:193), 改为 `environment="gcc3"`;④ ~~createPlayerWindow 的媒体子窗口句柄参数是 LO 内部结构~~ **认知修正见经验 30**:aArgs[0] 是公开整数类型 (sal_IntPtr 窗口句柄), SDK 模式可解析。**编译**:系统 g++ C++17 + LO 公开头 + SDK 库。**注**:LO 的 avmedia 后端编译期写死 GStreamer(mediamisc.hxx 宏), 选择逻辑改动见经验 30 | 2026-08-13 | 高(实测) |
| 30 | **媒体后端选择开关(方案 A, 最小侵入, 2026-08-13 全链路实测)**:LO `mediawindow_impl.cxx`(libavmedialo.so 组件, mediawindow_impl.cxx:190 处)读 `ORT_MEDIA_BACKEND` 环境变量: `ffplay` → `Manager_FFPlay`, 其他/空 → 编译期默认 GStreamer。office_runtime `EnsureKernel` 在 SAL_DISABLEGL 同位置 `setenv("ORT_MEDIA_BACKEND","ffplay",0)`(默认 ffplay, 2026-08-14 切换: gstreamer 不满足需求 — Xvfb 无音频设备时静默无声; 不覆盖宿主显式设置, export gstreamer 可回退)。**全绿里程碑**(AI时代.pptx 实测): `ORT_MEDIA_BACKEND=ffplay` 时媒体页 7-17 全部命中 FFPlay 真实播放路径, 绿色占比 3%-41% 与媒体 rect 精确吻合(446x251→3%, 1080x780→41%); 非媒体页 0% 无干扰; 不设变量走 GStreamer 29 页 0% 全过。**GreenWindow 句柄解析(修正经验 29④)**: createPlayerWindow 的 sequence<any> 参数 (mediawindow_impl.cxx:436-442): [0]=sal_IntPtr 媒体子窗口 X 句柄(UNX 下 GetParentWindowHandle = 窗口自身 frame 句柄), [1]=awt::Rectangle(公开 IDL), 均 SDK 模式可解析; [2]/[3]=LO 内部指针跳过。渲染用**服务端背景绿色子窗口**(XCreateSimpleWindow + background_pixel=0x00FF00, Expose/重绘仍绿, 比客户端 XFillRectangle 可靠), 父窗口未映射时建窗也安全(随父 map)。**验证探针**: `media_green_probe`(判据 2026-08-14 起为帧间差异, 见经验 34; 全绿里程碑为早期历史)。**LO 增量编译**: 模块级 make 即可 (`cd libreoffice/build/libreoffice_core/avmedia && export LD_LIBRARY_PATH=<build>/gcc/lib:<build>/gcc/lib64 && make -j8`), 产物 instdir/program/libavmedialo.so 备份后替换部署 | 2026-08-13 | 高(实测) |
| 34 | **ffplay 嵌入引擎全链路(补丁式复用, 2026-08-14 实测)**:官方定制 ffplay.c 以补丁方式嵌入 UNO 组件, 真实视频+音频播放。**结构**: `office_runtime/ffplay/compat/` — `ffplay.c`+`cmdutils.c/h`(与 FFmpeg4.4.1SDK/source/ffmpeg-4.4 上游 **diff=0**)+ 手写最小 `config.h`(已验证; 关键不是 config.h 而是 ffplay.c 源码版本——官方 release/4.4 的 ffplay.c 与 Nova 定制库不兼容会 find_stream_info 卡死, 定制版仅 44 行差异: RTP 丢包 skip_frame/AVDISCARD_ANYWAY + 音频同步日志)+ `ffplay_embed.patch`(259 行: 198+新增/2-删除; `#ifdef FFPLAY_EMBED` 包住, 默认关=官方行为零变化)。**补丁点**: ① VideoState 加 per-instance window/renderer/pump_tid 成员 ② main 嵌入 stub(全局默认值初始化移入引擎 create) ③ do_exit 嵌入不 exit/SDL_Quit(引擎末实例统一) ④ video_open 嵌入 SDL_CreateWindowFrom(外部 LO 媒体子窗口句柄) ⑤ video_display 开头同步全局 window/renderer(多实例渲染线程内原子, 各实例互不干扰——**不要用宏映射 window→is->window, `event.window` 等 SDL 字段会被误展开**) ⑥ 泵线程复刻定制版 refresh_loop_wait_event 轮询语义(定制版**无** FF_REFRESH_EVENT) ⑦ 引擎 C API(create/destroy/play/pause/seek/get_time/get_duration/set_loop/set_volume, ffplay_engine.h)。**编译**: 组件链接需 SDL2/avfilter/avdevice/postproc 全部(--no-as-needed 段, 缺一个即 soffice dlopen undefined symbol → 连锁 X BadWindow); kylin 的 `<sys/time.h>` 不含 struct tm → `-include /usr/include/time.h`(config.h 加 HAVE_SYS_RESOURCE_H 1); main 嵌入改名宏(与探针 main 冲突)。**验证链(可复现三层)**: 官方靶子(compat 原样编译, Input #0/退出码 0) → 引擎探针(ffplay_engine_probe: 播放时间推进/pause 冻结/seek 生效) → LO 真实路径(media_green_probe 判据升级: 全绿→**帧间差异**——媒体页 diff 23-64%, 页 11/12 静止时 diff 3-4% = 视频真实渲染)。**效果实证**: demo 媒体页真实视频 + **声音**(gstreamer 路径在 Xvfb 无音频设备静默失败无声音——ffplay 补齐)。**性能**: 媒体页播放时 calc 抓帧 avg 5ms→10-16ms(解码竞争, 可接受) | 2026-08-14 | 高(实测) |
| 37 | **ffplay duration 语义 + 多实例隔离现状(2026-08-17 检视定性)**:① **duration 接口已实现非桩**(`is->ic->duration`, ffplay_embed.c get_duration), 引擎 create 后**立即**查询返回 0 是 read_thread 尚未完成流探测(avformat_find_stream_info)——播放 1s 后查询正常(media1.mp4 实测 39.63s)。探针已把 duration 查询挪到播放 1s 后; LO 真实路径播放中查询不受影响。② **get_media_time nan 防御已落地 (2026-08-17)**:`get_master_clock` 对未初始化时钟(新实例/流未启动)返回 nan, `ffplay_engine_get_media_time` 现归零返回(实测实例[1] t=nans → t=0.00s; ffplay.so 已重编部署, media_green_probe 回归全绿)。③ **多实例隔离缺口(讨论项)**: ffplay_engine_probe 2 实例实测, 实例[0] 完全正常(推进/pause/seek/duration), **实例[1] 流未启动**(t 恒 0/duration=0, playing=1)——ffplay.c 官方即单实例设计, compat 里 30+ 进程级全局变量(窗口/parent_window/SDL 事件队列为进程级单例, 两个 pump 线程抢同一事件队列), 渲染隔离(补丁点⑤)之外疑似 read_thread/音频设备路径仍有全局耦合, 待查。**LO 真实路径暂不受影响**: media_green_probe 多媒体页(11-17)全绿(LO 侧媒体页切换为串行创建/销毁, 未触发并行双实例)。多媒体页同屏并行播放前需先修引擎多实例 | 2026-08-17 | 中(单次实测, 引擎侧待查) |

### 2.5 构建与部署

| # | 经验 | 时间 | 置信度 |
|---|---|---|---|
| 8 | **run.sh 依赖**:`LD_LIBRARY_PATH=$CURDIR:$CURDIR/office/program`(单副本部署后 $CURDIR 段仅剩 ffmpeg 库需求, office/program 段仍必需 (UNO 库); libstdc++ 单例机制见经验 25, 加载路径错位见经验 33) | 2026-08-12 前 | 高 |
| 16 | **ABI 同步**:office_runtime.h 变更必须同步重编三件套 —— 已由统一构建树的 target 链接机制承接(经验 31), `build_links_linux.sh` 退役;历史教训仍有效: 只重编 office_runtime 导致 dlopen undefined symbol; OR_DEPLOY_DIR 缓存旧值致新构建未部署(统一树内该变量已改普通变量, 由 LIBREOFFICE_SDK_ROOT 统一推导) | 2026-08-12 | 高 |
| — | **部署组件名陷阱**:gstreamer 后端生效组件是 `libavmediagst.so`,`libavmedialo.so` 是另一组件(勿混淆替换) | 2026-08-12 | 高(实测) |
| 36 | **相对 LD_LIBRARY_PATH 陷阱 + 探针免设方案(2026-08-17 实测)**:手动跑探针/单测时若 `LD_LIBRARY_PATH` 用**相对路径**(如 `bin_x86_64_kylin/office/program`), 经 dladdr→GetRuntimeDir 传导 UNO_PATH 也变相对 → `BootstrapOffice` 抛 DeploymentException → SIGABRT 全部会话失败(经验 33 的新变体)。**两个要点**:① **产品不受影响** —— run.sh 的 `$CURDIR` 展开为绝对路径(真实部署形态);② **探针/单测推荐免设** —— build_probes.sh 与统一树均烧入绝对 RUNPATH(部署目录), `office_runtime_test`/各探针直接裸跑即可(实测 nextpage 探针免设全绿, UNO_PATH 正确绝对化)。手动非要设时必须绝对路径。**部署目录注意**: 实际部署在 `NovaPlayer/bin_x86_64_kylin/`(不是 NovaPlayerTools/, 其下同名目录为空; build_probes.sh 以 `../NovaPlayer` 定位) | 2026-08-17 | 高(实测) |

---

## 三、项目未来规划(未来)

> **价值排序**:★★★ = 立即/高价值;★★ = 中期;★ = 远期/低优先。

### 3.1 待解决问题(先修问题)

| 排序 | 事项 | 说明 | 方向 |
|---|---|---|---|
| ~~★~~ | **gstreamer 路径清理(已完成 2026-08-14)**:默认已切 ffplay;回退路径已验证(export ORT_MEDIA_BACKEND=gstreamer 媒体画面正常);CheckGstDeps 仅显式回退时告警;libavmediagst.so 补丁保留作回退 | 已关闭 |
| ~~★★★~~ | **LO 源码改动同步远端(已完成 2026-08-17)**:两处改动同在 commit `83e0b9c3e`(gstplayer.cxx + mediawindow_impl.cxx), master 已推 origin, 工作树干净 | 已关闭 |
| ★★ | **ffplay 多实例隔离(讨论项, 经验 37③)**:引擎第 2 实例流不启动(t 恒 0/duration=0);LO 真实路径串行创建/销毁暂不受影响, 多媒体页同屏并行播放前需先修;ffplay.c 官方单实例设计, compat 内 30+ 进程级全局变量(窗口/parent_window/SDL 事件队列单例);get_media_time nan 归零防御已落地(经验 37②) | 引擎侧, 讨论后改 |
| ★★ | **ffplay 能力增强**:XFrameGrabber(帧抓取, 缩略图用)/硬解(vaapi/cuvid 已编译进 ffmpeg, 真机可用)/媒体信息接口补全 | 经验 34 引擎底座 | 按需 |


### 3.2 功能规划

| 排序 | 事项 | 说明 |
|---|---|---|
| ★★★ | **writer 模块**:照 impress 模式(writerlink.so;XPageCursor 页导航,直接渲染不转 PDF;用户已确认"动画模式"= 直接复用 soffice 引擎渲染) | 核心交付 |
| ★★ | **2160p 输出支持**:共享屏配置 `max_doc_width/height=3840x2160`(默认已是),多文档混合分辨率落位待产品化验证 | 配置验证 |
| ★★ | **阶段 2:slot 管理策略**:超限语义(AllocSlot -1 上层决策)、动态轮替、最大并发数 | 策略决策 |
| ★★ | **废弃旧方案**:source/ + PptAnimationManagerLinux 功能对齐后废弃(target 已标记 `NovaLibreOfficePlayerDeprecated`) | 收尾 |
| ★ | **Windows impress 平台实现**(当前 stub) | 平台补全 |

### 3.3 远期演进

| 排序 | 事项 | 说明 |
|---|---|---|
| ★ | **ffplay 能力增强(按需/弱需求)**:帧访问(XFrameGrabber)/硬解(vaapi/cuvid 已编译进 ffmpeg, **真机可用性待验证, 稳定性风险 — PPT 场景内核稳定优先, 按需启用**)/媒体信息 | 见 3.1 同项, 引擎底座已就绪(经验 34) |
| ★★ | **环境自检与启动校验**:gst 依赖检测已完成(CheckGstDeps, 经验 28);剩余字体/音频缺失检测,缺失时明确报错而非静默失败 | 部署稳健性 |
| ★ | **崩溃检测 + 明确告警**:会话失效(soffice.bin 崩溃/bridge disposed)时上层可感知,不假装自愈 | 可观测性 |

---

*文档维护:每次认知提升后更新"二、历史经验总结"(新增条目加时间与置信度);完成事项从"三、规划"移入"一、现状"。*
