# 平台隔离设计 + 专项治理（HANDOFF.md 三章3.0/3.3 + 四章伴随文件）

> 本文件承载 HANDOFF.md "三、3.0/3.3 平台隔离设计" 与 "四、平台隔离专项" 全部深度内容。
> 摘要级信息（隔离边界总账/回归规则速查）仍在 HANDOFF.md 对应章节, 阅读路径:
> 日常回归看 HANDOFF.md 四章速查表; 设计决策/根因分析/迁移路径在本文件。

---

## 0. 代码锚点归位（单点全源）

> 平台隔离知识**本文件唯一承重**；代码注释一律只作锚点（`[platform-isolation]`）。
> 散落注释知识已按其归属收进本文件上文各节，代码侧仅保留指引。概念 → 代码落点：

| 概念 | 代码落点 | 本文件节 |
|---|---|---|
| LinkPlatform 接口契约（Discover/Form/Plan/BeginBoot/HideUiFloats/HideUiExtras） | `platform/link_platform.h` | D + Part3 §3.1 |
| SessionPlan 数据驱动（`plan_.discover/form/terminate_on_destroy`） | `calc/session.*` · `impress/session.*`（`plan_` 成员；Create 取、Destroy 消费） | C/D/E |
| P0 平台工厂 + PrepareEnvironment | `calc/session.cpp` `CreateCalcPlatform()` · `impress/session.cpp` `CreateImpressPlatform()` | C(P0) |
| UI 修补 HideUiExtras 下沉 | P9 后 `platform_->HideUiExtras(...)`；`win_platform.cpp` 实现 / `xvfb_platform.*` 空实现 | Part3 §3.1 |
| writer 引导缝 G（KernelHost, 零 #ifdef） | `base/link_utils.h` · `writer/session.cpp` | G |
| to_path/u2w 机制上收（双平台） | `base/link_utils.h` | E |
| 平台机制实现（差异的家） | `platform/linux/xvfb_platform.*` · `platform/windows/win_platform.*` | A/B/E |

---

## 平台隔离设计(意图/机制分离)— 已实施 2026-08-18 (J1-J4 全量)

> 背景: 双平台并行开发负担重。UNO 层大体一致(实证: writerlink 零平台层双平台可用),
> 桌面/窗口层本质分歧(共享内核+Xvfb+slot vs 独立进程+独立桌面)。**分歧不可消除,
> 但可以安放**。现状诊断(2026-08-18 统计): 平台分支倒挂——本应承载差异的平台层几乎
> 干净(xvfb_platform 0 处/win_platform 2 处), 本应平台无关的会话层躺着 28 处 `#ifdef`
> (calc 9 / impress 8 / writer 11)。目标: **会话层零 `#ifdef`**, 单平台开发者的
> 变更面物理上碰不到对端平台, 微妙细节各有唯一且被编译器守护的家。

### A. 三原则

1. **隔离意图, 不隔离机制**: 接口抽象的是 what/when(协议与时序), 不抽象 how
   (XMoveWindow/SetWindowPos/CreateDesktopA/XShm)。统一"窗口 API"是伪泛型——
   最小公约数会强迫放弃各平台的 workaround, 那才是毁细节的方式。
2. **不变量构造性执行**: 跨平台禁令不靠注释记性, 靠代码结构让违规不可能
   (例: 核心层不持有窗口句柄 → "放映中不得 UNO setPosSize"物理上无处发生)。
3. **变体点可枚举**: 平台间自由度全部收进 plan 数据结构, review 时一眼看清
   两平台到底差在哪几维, 而不是在 28 处 `#ifdef` 里考古。

### B. 关键澄清: 两类差异, 只隔离其中一类

- **calc vs impress 的差异 = 文档类型差异** → 允许留在各自会话文件(有无放映段)。
- **Windows vs Linux 的差异 = 平台差异** → 必须出会话文件, 进平台模块或 plan。
- 判据: 代码里出现平台名(`_WIN32`/`__linux__`)即是违例; 出现文档类型名是正常。

### C. 会话协议规格(核心独占, 双平台同一份代码)

```
里程碑序列 (核心按此顺序执行, 平台工作绑定点由 plan 声明):

  P0  平台工厂 + PrepareEnvironment      (Linux: Acquire/Xvfb/slot; Win: DPI/桌面/profile seed)
  P1  BeginBoot (BootSection RAII)       — 意图: 并发 Create 引导+加载须串行(经验 5)
  P2  EnsureKernel → 空 ctx 则 BootstrapSession (calc/impress 均已此形态; writer 走 KernelHost)
  P3  SnapshotWindows + Hidden 加载          — 意图: 引导+加载须串行(经验 5)
  (BootSection::Release 不在 P3: 见 P5 后注*; 提前释放会 reintroduce 经验 5)
  *Release 绑定点 = 当前 session.cpp Create() 内 setVisible(P5) 之后 (boot_section->Release(), 经验 5)。setVisible(P5)
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

### D. LinkPlatform 接口定稿形态

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

### E. 变体点总账(现存每处 `#ifdef` 的归宿)

| 现存位置 | 内容 | 归宿 |
|---|---|---|
| impress/export.cpp:39 / calc/export.cpp:58 / writer/export.cpp:17 | include office_runtime | 平台实现文件内(机制) |
| impress/export.cpp:77 / calc/export.cpp:366 / writer/export.cpp:592 | destroy 时 terminate | `plan.terminate_on_destroy` + OnSessionEnd |
| impress/export.cpp:117 / calc/export.cpp:221 / writer/export.cpp:117+190 | BootLock+Unlock | BeginBoot RAII + Release(释放点=协议 P3) |
| impress/export.cpp:166-175 | 解锁+start 前找窗 | `plan.discover=AfterReveal`(impress/Linux) |
| impress/export.cpp:188-211 | IsFullScreen 平台分支 | `plan.fullscreen` |
| impress/export.cpp:219 | settle 2500ms | `plan.settle_ms` |
| impress/export.cpp:277-289 | start 后 slot 落位 | `plan.form=AfterStart` + FormWindow |
| impress/export.cpp:321-333 | Win 窗口化 A/B 兜底 | `plan.ui_hide_needed`;ORT_IMPRESS_FULLSCREEN 逃生门留在 Win 平台内 |
| calc/export.cpp:302-309(Win) | reveal 前+找窗+落位+快捷键 | `discover=form=BeforeReveal` + FormWindow 内含快捷键(见 F) |
| calc/export.cpp:317-327(Linux) | reveal 后找窗+落位 | `discover=form=AfterReveal` |
| writer/export.cpp:35-82 | u2w/to_path/进程 ID | link_utils 机制层(**to_path 应上收 link_utils 三链共用**) |
| writer/export.cpp:117-134+606 | Acquire/EnsureKernel/Release | 见 G(writer 引导缝) |
| calc/export.cpp:33-45 | windows.h/FindWindow 宏 | 编译机制, 可留(或 os 头收拢) |
| base/link_utils.cpp:18/38/75 | GetLinkDir/BootstrapSession/u2w 双实现 | 本职(它就是机制的家), 不动 |

### F. 最微妙用例: calc 的 Windows 反序(设计容纳力的试金石)

Windows calc **先找窗+定型再 setVisible**(VCL 在 setVisible 时按最终形态创建窗口,
反序则 menubar 隐藏失效,demo 实测);Linux **先 setVisible 再找窗+落位**。设计下:

- Win plan: `discover=form=BeforeReveal`;Linux plan: `discover=form=AfterReveal`。
- 核心只在 P4/P5/P6 按各自 plan 调 DiscoverWindow/FormWindow, **同一条代码**。
- "定型必须在显露前"这条 Win 局部知识, 写在 win_platform 的 Plan() 返回处与
  FormWindow 实现注释里, 连同 demo 实测记录——Linux 开发者永远不需要知道它。

### G. writer 的引导缝(可选, 最后做)

writer 无平台层是定案(经验 38④), 其 Linux 分支(Acquire/BootLock/EnsureKernel/
Release)是同一"引导+串行+生命周期"缝。两个选项:
- **G1(推荐)**: 抽 `link_utils::KernelHost` 三函数(BeginBoot/ObtainCtx/EndSession),
  双平台各一个编译单元文件;writer 会话零 `#ifdef`, 不引入 LinkPlatform。
- G2: 维持现状 4 处分支(少而稳定, 承认不完美)。
- 不选: 给 writer 强加 LinkPlatform(违反无平台层定案)/HeadlessPlatform(过度设计)。

### H. 知识安居铁律 + 映射

每条踩坑结论必须落在且只落在两处之一, 不允许第三处(现状的会话 `#ifdef` 是第三处):

| 经验 | 家 |
|---|---|
| 1(全屏盖大屏→窗口化+slot)/13(XShm 直拷)/14(屏高 BadMatch)/15(坐标上限) | xvfb_platform.cpp 内部 |
| 5(引导串行+窗口查找可并行) | BeginBoot/Release 契约 + 核心在 setVisible(P5) 之后调用 Release |
| 22/23/27(bootstrap/profile 隔离) | EnsureKernel/BootstrapSession 契约(意图)+平台实现(机制) |
| 26(放映中 UNO 几何黑屏) | FormWindow 契约 + 核心不持窗口句柄(构造性) |
| 38④(writer 无平台层) | G 缝选择 |
| Calc 外部写锁预检 (Win32 CreateFileW, 经验 48) | link_utils::SourceWriteLocked (基础层函数内部 `#ifdef`, u2w/to_path 同款; 预检/编码类平台差异落基础层函数为 G1 同族裁决, 会话层保持零逻辑 `#ifdef`) |
| 41(paused_ 重置在入口函数) | FramePump::Start 契约 (泵内无条件 paused_=false, 经验 42 承接) |
| Win settle 2500ms / 反序定型 / 1.5s 形态稳定 | win_platform.cpp 内部 + plan 数据 |

### I. 保证机制(三层)与诚实边界

- **构建期(构造性)**: 核心零 `#ifdef` → 改核心语法上不可能破坏平台代码;改接口则
  未适配平台**编译响亮失败**——失败点即唯一耦合点。
- **行为期**: 双平台同脚本 conformance 探针(create→start→帧→翻页→destroy);
  Linux 已有(impress_nextpage/media_green 等), Windows impress 落地时补等价物。
- **知识期**: 经验编号锚定平台文件注释(上表), 细节搬家注释随行。
- **边界**: 协议本身变更(新增里程碑)仍是双平台共同决策——以接口变更形态出现在
  review, 响亮可见;无 Windows CI 前, "保证"上限 = 构造性防护 + 纪律。
  C ABI 与 UNO 语义是共同资产, 动它们仍需对端编译确认。

### K. 反模式清单(明确不做)

- 统一 X11/Win32 "窗口 API"(伪泛型, 最小公约数毁 workaround)
- 按平台拆仓库(单树+目录隔离足够)
- 为 writer 强加平台层 / 模板基类魔法(FramePump 是经验 42 的事, 不混入本设计)

---

## Part 3: 平台隔离专项治理 (2026-08-19)

> 目标: 消除双平台开发的串扰风险, 让任何平台的调优/回归不影响其他平台。
> Part 2 是设计 (意图/机制分离), 本部分是专项治理记录 (盲区发现 + 修补落地)。

### 3.1 子项1: UI 隐藏隔离 (2026-08-19 已落地)

#### 问题

Windows 回归 (85aae31f..HEAD) 在 session.cpp 共享层新增 `InputLineVisible` dispatch
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

平台相关的 UI 修补 (InputLineVisible dispatch) 被放在共享层 (session.cpp),
其副作用平台相关 (Linux 共享内核破坏 vis=0 初始态, Windows 独立进程不破坏)。
隔离设计只覆盖"平台机制"层, UI 隐藏逻辑被误当作平台无关。

#### 解决方案: HideUiExtras 下沉

遵循 HideUiFloats 已建立的范式, 新增 `LinkPlatform::HideUiExtras(frame, factory, ctx)`:

| 平台 | 实现 | 行为 |
|------|------|------|
| Linux (XvfbSessionPlatform) | 空操作 | LO Xvfb 无头环境 UI 默认 vis=0, 无需平台修补 |
| Windows (WindowsPlatform) | InputLineVisible dispatch | 搬迁自 session.cpp, 行为不变 |

会话层改动:
```cpp
// session.cpp P9 后
platform_->HideUiExtras(frame_, factory, ctx_);  // 平台自决
```

#### 改动清单

- `platform/link_platform.h`: 新增 HideUiExtras 虚函数 + UNO include
- `platform/linux/xvfb_platform.h`: HideUiExtras 空实现 override
- `platform/windows/win_platform.h`: HideUiExtras 声明 override
- `platform/windows/win_platform.cpp`: HideUiExtras 实现 (InputLineVisible dispatch 搬入)
- `calc/session.cpp`: InputLineVisible dispatch 块 → platform_->HideUiExtras()

#### 验证

- **Linux**: 2 xlsx + 1 pptx demo, UI 全部干净 (menubar/toolbar/公式栏 隐藏) ✓
- **Linux impress**: 2 pptx + 1 xlsx demo (ORT_LOG_LEVEL=debug), UI 全部干净 ✓
- **Windows**: 行为不变 (InputLineVisible dispatch 逻辑原样搬迁, 仅日志前缀改) — 待 Windows 侧回归确认
- **隔离保证**: Linux 改 HideUiExtras 实现 (空) 不影响 Windows; Windows 改 HideUiExtras 实现不影响 Linux

#### UI 隐藏机制实证结论

- **LO Xvfb 无头环境 UI 默认 vis=0**: hideElement 是对已隐藏元素的冗余兜底
- **`.uno:FullScreen` dispatch 在 Xvfb 下从未生效**: desktop_ provider 找不到此 dispatch
- **setMenuBar(null) 消除 impress 1px 底边框**: 移除 menubar 容器触发窗口重绘, 消除初始化过渡期边框; hideElement 和 sleep 对 1px 无效
- **多文档并发无竞态**: 各会话 frame 隔离, setMenuBar/hideElement 互不影响

### 3.2 待评估子项

#### 3.2.1 模板隔离

当前模板 (registrymodifications.xcu) 是共享真相源 (NovaPlayerTools/templates/user/),
双平台共用一份。实验2 证明模板在旧代码 (无 InputLineVisible) 下无影响 (LO Xvfb 默认
vis=0, 模板条目被覆盖), 但在新代码 (含 InputLineVisible, 触发 UI 重建) 下起兜底作用。

评估点:
- 是否需要平台分叉 (templates/linux/ vs templates/windows/)?
- 或保持共享但明确"模板是共享的, 改动需双平台验证"?
- 当前结论: 保持共享, 纳入"共享层改动需双平台回归"规则 (见 3.4)

#### 3.2.2 隔离契约文档化

Part 2 已有 LinkPlatform 接口定稿形态 (D), 需补全:
- HideUiExtras 接口契约
- 隔离边界总账 (HANDOFF.md 4.1 表格) 的维护规则
- "哪些改动是平台安全的 (只改平台实现), 哪些是双平台共享的 (改共享层需双平台验证)"

### 3.3 反模式 (不做)

- 在共享层调用平台专属 dispatch (如 InputLineVisible 是 calc/Windows 专属, 不应在 session.cpp)
- 假设"平台机制隔离了 = UI 隔离了" (UI 隐藏副作用是平台相关的)
- 为统一而统一 (Linux 不需要 InputLineVisible, 不应为了"对齐"而在 Linux 也调用)
