# Impress 底部透显 xlsx 栅格缺陷 (bleed-through)

> 关联代码: `platform/linux/xvfb_platform.cpp` `SizeWindowToSlot` / LO `sd/source/ui/slideshow/slideshowimpl.cxx`
> 复现/验证探针: `tools/linux/bleed_probe.cpp` (`bleed_probe`, add_multi_tools)
> 状态: **根治已落地** (2026-08-24 二轮取证修正根因 + LO 补丁铺满窗口 + 全回归绿)

---

## 1. 现象

NovaPlayerDemo 同时播放 pptx 与 xlsx 时,**pptx 底部约 1/20 显示的是 xlsx 的表格栅格/底部页签(sheet栏)**。
不是随机噪点,而是"一张表格 + 底部 tabs"的稳定画面。

## 2. 复现探针 (bleed_probe)

```bash
# 隔离运行 (探针加载部署目录生产 .so; 须先编译: cmake --build build --target bleed_probe)
ORT_HOME=/tmp/ort_x ./build/tools/bleed_probe <xlsx绝对路径> <pptx绝对路径>
# 输出: [基線]/[FixA] 峰值/平均/>90%行数/残留行/幻灯片顶部亮度 + 几何指标
#        (内容末行/未绘制带行数/亮线行; 满窗渲染时 内容末行=1079、带=0)
# 生产开关 (A/B 矩阵):
#   ORT_BLEED_FIX=0        关闭生产 Fix A (SizeWindowToSlot 显式背景+清+重映射)
#   ORT_SLIDE_FILL_WINDOW=0 关闭 LO 铺满补丁 (EnsureKernel 默认置 1; 回退到原生行为)
#   BLEED_NO_PROBE_FIXA=1  跳过探针侧 Fix A (保留透显现场取证)
#   BLEED_HOLD_SEC=N       保持会话 N 秒供 xwininfo/抓屏检查
# 基线帧落盘: /tmp/bleed_imp_baseline.ppm + /tmp/bleed_calc.ppm (P6)
```

## 3. 根因 (2026-08-24 二轮取证修正, 实证链)

**三个先前结论被修正**: "calc 引导期全屏渲染是必要污染源" 不成立 (模板钉住 calc 后透显依旧);
"letterbox 纵横比不匹配造成未绘制带" 不成立 (幻灯片本就是 16:9); "残留行 y=1042 是泄漏" 是测量伪影。

实际机制 (每环均有实证):

1. **放映视图只画到 window-38px** (1080→1042): 窗口化放映的视图尺寸 = SFX `getClientRectangle()`,
   它**永远保留状态栏布局槽** (本机 Xvfb 恰为 100 DPI → 38px), 即使状态栏已隐藏
   (模板 Visible=false + hideElement 全程 vis=0, debug 日志实证)。且 SFX 布局还会把
   父 VCL 容器一并缩到 window-38 (LO 打点实证: `resize: parent=1920x1042`)。
   后果有二: 底部 38 行**从未被绘制** (bg=None 窗口透明) + **幻灯片被纵向压扁 ~3.4%**
   (与 GT 渲染比对: 压扁假设 diff 4.93 vs 裁剪 11.52, 质心法确认压扁)。
2. **未绘制带"吸附"别的文档像素**: 文档窗口按模板钉在 (10,1,1920,1080) 诞生 —— 后创建的
   文档窗口正好压在先创建文档窗口上 (时间线取证: impress 诞生于 +10+1, 与 slot0 的 calc 重叠)。
   bg=None 的未绘制带直接"看到"下方窗口像素 (calc 栅格)。
3. **XMoveWindow 屏幕级 blit 搬运**: FormWindow 把窗口移到 slot 时, Xorg 用屏幕拷贝实现移动,
   带着吸附的栅格像素一起搬到 slot —— 抓帧 (读 live screen) 于是读到 calc 栅格。
   (无任何修复时基线: 峰值 95%/13 行 >90% 逐像素相同, 必现。)

一句话: **接收漏洞 = 放映视图 38px 未绘制带; 污染路径 = 出生重叠吸附 + 移动 blit 搬运。**
calc 全屏瞬态 (30720x2160) 是加重因素但非必要条件。

## 4. 修复 (生产落地)

### 4.1 根治: LO 补丁 — 窗口化放映铺满窗口 (env 门控)

`sd/source/ui/slideshow/slideshowimpl.cxx` 两处 (同 gstplayer/mediawindow 的本地改码模式):
- `startShow()`: 窗口化分支在 `ORT_SLIDE_FILL_WINDOW=1` 时改取**顶层 VCL 窗口**
  (`GetWindow(GetWindowType::Overlap)` = 抓帧的 X 窗口) 的输出尺寸, 而非 getClientRectangle;
- `resize()`: 同门控重算 (resize 传入的 rSize 与父窗口均已被 SFX 缩到 window-38, 必须取顶层)。

`office_runtime` `EnsureKernel()` 默认 `setenv("ORT_SLIDE_FILL_WINDOW","1",0)` (bootstrap 前,
env 快照继承, 经验 24; 宿主可 export 0 回退)。

效果: 放映铺满 1920x1080 → **未绘制带不存在** (透显结构性不可能) + **压扁消除** (真 16:9)。
注意: 补丁只影响本项目部署 (env 门控), vanilla LO 行为不变; **LO 升级需重打** (经验 34 同款纪律),
libsdlo.so 增量编译命令: `cd libreoffice/build/libreoffice_core && LD_LIBRARY_PATH=<lo>/gcc/lib:<lo>/gcc/lib64 make sd.build`。

### 4.2 保险带: Fix A 保留 (显式背景 + 清窗 + 重映射)

`xvfb_platform.cpp::SizeWindowToSlot` 落位后 `XSetWindowBackground(黑)+XClearWindow+unmap/map`。
补丁失效场景 (env 回退/未来 LO 行为变化/其他未绘制窗口) 的兜底; `ORT_BLEED_FIX=0` 可关 (回归 A/B)。

### 4.3 卫生层: calc 出生几何钉 0,0,3840×2160 (= 单 slot 大小) (2026-08-24)

模板 `templates/user/registrymodifications.xcu` 给 Calc Factory 增加
`ooSetupFactoryWindowAttributes = 0,0,3840,2160;1;,,,;`(此前 calc 无配置 → LO 默认
以整屏 30720×2160 创建并渲染,全屏栅格瞬态)。效果: calc 出生足迹收敛在**自己的
slot 内**,不再跨 slot 涂抹;落位时仍按需缩小 (SizeWindowToSlot → 1920×1080, 实测
`new window 3840x2160` + 最终帧 1920×1080)。位置 (0,0) 而非 (10,1): 消灭 10px 越界
(越界会被邻居 Expose 自愈, 但零越界更干净)。注意: ①此条**单独不治透显** (M6 实证:
钉住后透显依旧, 真机制是出生重叠吸附+移动 blit), 属纵深防御; ②共享模板改动,
双平台需回归 (Windows 侧 calc 会以 3840×2160 创建后被 FormWindow 缩小, 理论无影响
未验证); ③若 OfficeRuntimeConfig 的 max_doc 尺寸改配, 模板值需同步。
出生策略的边界 (讨论定论, 勿重走): 被覆盖的**活窗口会 Expose 自愈**, 持久残影只在
无人认领区 (未绘制带/中间窗口/已销毁窗旧位) —— 所以"出生钉小窗 (如 640×480) 再撑大"
无净收益 (终位残影与出生大小无关, 还多一跳重排+小窗 UI 构建窗口期风险);
"落位前刷黑 slot"亦无收益 (Fix A 已对落位窗口整窗刷黑, slot 内窗外的区域不被抓帧)。

### 4.4 边圈黑化 (2026-08-24 收尾: VCL 框架内缩圈, 最后的未绘制区)

铺满补丁消灭 37px 底带后, X 窗口里仍剩**内缩圈** (VCL 顶层内容原点内缩, 实测左 2px/顶 1px):

- **顶行整行由 VCL 在每次曝光时主动刷白** (实证: X 层清黑后 2s 内复原; 单独跑 impress 无
  calc 依然白; GT 真值顶行纯黑 → 白线非幻灯片内容)。黑模板上呈 1px 白线, 用户可辨。
- **左 2px 无人绘制** (清后可持有), 保险关时经"出生吸附+移动 blit"带上别的文档残影
  (topcorner_probe 实证 2px×80px 段, 周期=对方行高)。
- LO 侧**无解** (均实证): SetBackground(Wallpaper 黑) 换不动曝光重绘的白; 负坐标平移
  覆盖被 VCL 父矩形裁剪 (no-op)。

修复 = `xvfb_platform.cpp::CaptureFrame` 首帧 (及 SetWindowSize 后) 的**无曝光清法**:
`XSetWindowBackground(黑)` + 四边 `XClearArea(expose=False)` —— server 直接画黑且不产生
Expose, VCL 不知情故不触发重绘, 实测长期持有。**时序关键**: 放首帧 (= P9 UI 隐藏之后),
避开 P9 重排版把顶行重新刷白的窗口期 (落位 P8 处做会被复原)。
效果: 四边恒黑 (白线消失), 保险关也零残影 (topcorner 修正判据全帧 0.000% 匹配)。

## 5. 验证矩阵 (2026-08-24, 全部实跑)

| 配置 | 峰值/平均/>90%行 | 几何 | 结论 |
|---|---|---|---|
| 无修复 (FIX=0, FILL=0) | 95% / 52% / 13 行 | 带=37 行 (1043..1079) | 缺陷必现, 根因链闭环 |
| 仅 LO 补丁 (FIX=0, FILL=1) | **3% / 0% / 0 行** | **内容末行=1079, 带=0** | 根治 ✓ |
| 仅 LO 补丁 + 撤出生钉住 (calc 30720 全屏瞬态回归) | 3% / 0% / 0 行 | 带=0 | **补丁是唯一承载; 钉住对抓帧输出冗余** (钉住保留理由=卫生: 渲染足迹 8×↓, 见 §4.3) |
| 生产默认 (FIX=1+FILL=1+边圈黑化) | 3% / 0% / 0 行 | 带=0, 四边黑 | ✓ |
| 保险关 (FIX=0, FILL=1+边圈黑化) | — | topcorner 全帧 **0.000%** 匹配 | 边圈黑化独立承载边圈, 不依赖保险 |
| 回退路径 (FIX=1, FILL=0) | 80% / 2% / 0 行 | 带黑 + **压扁仍在** | 黑底单独能止血但留黑条+3.4% 变形 → 补丁不可替代 |

- 纵横比: 质心 580.7 vs 真 16:9 参考 584.8 (Δ4px), 远离压扁参考 564.2 → **压扁消除**。
- 回归: office_runtime 单测 50 检查 PASS / impress_nextpage (29 页翻页+帧流) / impress_multi
  (2 xlsx + pptx 并发 slot 0/1/2) / media_green (媒体页 diff 活跃 + SetMute OK) 全绿。
- 探针自身崩溃修复: `dlclose(libXcomposite)` 必须在 `XCloseDisplay` 之后 (Xext close hook
  悬垂 → XCloseDisplay 跳未映射地址 SIGSEGV, core 实证)。

## 6. 证伪记录 (勿重走)

| 假设/方案 | 证据 | 结论 |
|---|---|---|
| "calc 全屏渲染残留是必要污染源" | 模板给 calc 钉 1920x1080 (new window 实证 1920x1080) 后透显**依旧** (13 行不变) | 证伪; 真机制 = 出生重叠吸附 + 移动 blit |
| "letterbox/纵横比不匹配造成带" | 幻灯片 16:9 = 窗口 16:9, 仍有 38px 带; 带 = SFX 客户区状态栏槽 (源码级) | 证伪; Fix C 客户端变体 (按纵横比改窗高) 前提不成立且无不动点 |
| root 卫生 (root 黑背景+清屏) | 与无修复**完全相同** (13 行) | 证伪; 暴露事件被中间 LO 窗口 (泛化 30605x2060, bg=None, unmapped) 截胡, root 够不到 |
| resize nudge/shake 撬动布局 | 1079→1080 与 1080→540→1080 后几何不变 (仍 1042) | 证伪; SFX 客户区扣除不可被 resize 撬动 (shake 仅顺带清了带) |
| 出生小窗 640×480+按需撑大 | 残影只在无人认领区持久, 活窗口被盖会 Expose 自愈; 终位残影与出生大小无关 | 无净收益; 多一跳重排+小窗 UI 构建窗口期风险 |
| 新文档落位前刷黑 slot 区域 | Fix A 已对落位窗口整窗刷黑 (bg 黑+清+重映射); slot 内窗口以外区域不被抓帧 | 无收益, 未采纳 |
| XComposite redirect 根治 | redirect 后的"残留 1 行"实为伪影 (见下) | 不必要; 补丁铺满后未绘制带不存在, redirect 无用武之地 |
| "残留行 y=1042 = 泄漏" | y=1042 是放映视图底边**亮线** (均值 246; GT 底部为深色), 与 calc 白色表格行巧合匹配 | 测量伪影; 该行本身是 LO 画的, 非透显 |
| "1px 手枪形顶左角残影" (topcorner_probe) | 双常数色巧合: 保险黑圈 vs calc 行号栏黑线 (周期=行高 24/51px) 逐像素相等; 顶行双白巧合。真残影 (保险关) = 左 2px 内缩圈 | 判据已修 (双近纯黑/纯白匹配不计); 真圈由 §4.4 边圈黑化根治 (保险关全帧 0.000%) |
| LO 侧治顶行白线: SetBackground(黑) / 负坐标平移 | SetBackground 换不动曝光重绘的白 (v4 实测无效); 平移被 VCL 父矩形裁剪 (v3 实测 no-op) | LO 侧无解; 客户端无曝光清法可持有 (§4.4) |

## 7. 经验 / 教训

- **"帧里多了别的内容"类缺陷, 先做窗口树时间线 + 小区域像素轮询取证** (xtap 模式), 一次还原
  "谁在哪里画了什么"; 比"合理推断+单点验证"快且不易被巧合误导 (本缺陷第一轮两个推断都是错的)。
- **bg=None 的 X 窗口未绘制区是"透明玻璃"**: 显示/被抓帧时呈现的都是屏幕上同位置的旧内容;
  XMoveWindow 的屏幕 blit 会把这些"借来的像素"一起搬走 —— 任何"窗口曾经压在别人上面"都可能污染。
- **修复验证必须带完整性指标** (顶部亮度/纵横比/帧流), 光看"透显没了"会漏掉同时被引入/暴露的
  质量问题 (本例: 压扁 3.4% 在修复透显后才显形)。
- LO 侧 UI 保留 (状态栏槽) 藏在 SfxWorkWindow 客户区计算里, UNO/LayoutManager 层够不到;
  自编 LO 场景下 env 门控源码补丁是干净出路 (先例: ORT_MEDIA_BACKEND / SAL_DISABLEGL)。
- 探针凡 dlopen 的 X 扩展库, **dlclose 必须晚于 XCloseDisplay** (Xext close hook 生命周期)。

## 8. 未解之谜 (有界): 顶行白线的画家身份 (2026-08-24 定案)

边圈黑化 (§4.4) 控制了症状, 但**白色重绘者的 LO 内部身份未定位** —— 诚实记录如下:

**已知 (行为契约, 全部实证可复现)**:
- 顶行 y=0 全宽 1px, 任何 Expose 都触发一次白色重绘 (清黑后 2s 内复原);
- 无 Expose 则永不重绘 (无曝光清黑长期持有);
- 左 2px 圈无人绘制; 幻灯片内容从 (2,1) 起 (VCL 内容原点内缩);
- 非 calc (单实例无 calc 仍白)、非幻灯片内容 (GT 顶行纯黑)。

**未知 (重开时从这里挖)**:
- 曝光时画白的具体 VCL 窗口/代码路径 (已知不是 pTop+父链的 Wallpaper —— SetBackground(黑) 无效);
- 内缩量 (2,1) 的来源 (哪个边框度量);
- 挖法: vcl/unx (UnixSalFrame) 曝光处理路径加日志, 或 XDamage 定位绘制源。

**为何现在不挖**: 两个低成本 LO 侧方案已证伪, 深挖需在 vcl 里逐层插桩; 客户端
无曝光清法以协议语义 (server 画黑、VCL 不知情) 完整控制症状, 边际收益为零。

**重开条件**: ①边圈黑化失效 (无曝光清后白线复发 = 有非 Expose 途径的重绘);
②LO 升级后边圈形态变化; ③需要 LO 侧根治 (如未来别的消费方复用同一 LO)。

## 9. 遗留

- LO 补丁需按惯例固化到 LO 仓库远端 (同 commit 83e0b9c3e 模式)。
- 双平台: 本补丁仅 Linux 共享内核路径消费 (Windows 无 Xvfb 抓帧); libsdlo.so 为平台部署物,
  Windows 侧 soffice 二进制不受影响, 无需回归。
