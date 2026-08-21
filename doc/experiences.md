# 经验详述（HANDOFF.md 二章伴随文件）

> 本文件承载 HANDOFF.md "二、历史经验" 中篇幅较长的详述（经验 38/40/41/42）、
> 关键经验失效条件表、零引用经验清单。
> 经验编号主索引（表格 + 时间 + 置信度）仍在 HANDOFF.md 二章；编号只增不改。
> 阅读路径: 先读 HANDOFF.md 二章表格定位经验号, 按需跳入本文件对应详述。

---

---

## 经验 42 详述：FramePoller 共性分析与治理

**FramePoller 共性分析与治理 (2026-08-18 分析, 2026-08-19 阶段0-4 全部落地)**

三链 (impress/calc/writer) 各自手写一份 poller (`poll_thread_`/`poll_running_`/`paused_`/`force_frame_`/`mu_` 同名同型), 六维不一致演化出 P1-P8 缺陷。本经验为完整治理记录; 设计决策论证/性能预算/测试矩阵见 [design-framepump.md](design-framepump.md)。

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

**阶段5 (可选, 未做)**: 平台层段内比对 (CaptureFrame 增量 unchanged 参数, 省应用层 8.3MB 拷贝) + dedupe + calc zoom 维度 A/B

**开放问题 (不影响阶段0-4, 可延后):**
- A. 帧新鲜度 TTL: 消费方是否存在"末帧超时视为无帧"? 决定心跳保留(近零成本) or 静止静默(收益最大)。dedupe 两阶段设计使该问题可延后且不返工
- B. calc tick 20ms 滚动延迟接受度 (最坏 +15ms): 默认 20ms, plan 一处可改, 可 A/B 探针实测后定

---

## 经验 40 详述：user 模板机制 + office-link 命名定稿

**user 模板机制 + office-link 命名定稿 (2026-08-18)**

- ① **UI 控制三层优先级(定论)**: UNO API > 平台窗口 API(X11/Win32) > user 模板配置 — 单一层做不到完全控制, 模板是基线兜底不承担运行时控制; Windows 的 per-session fresh copy 正是该层配套防御(运行期写回的 UI 状态不跨 session 存活)
- ② **命名**: Linux `~/.office-link/xvfb/`(内核跑在 Xvfb 上, 名字直指机制; 原 player/ 更名, 运行时数据无迁移负担)、Windows `desktops/<link>/<guid>/`(每 session 独立桌面); office_paths: `xvfb_profile()/desktop_profile()/user_template()`
- ③ **模板 = 仓库 `templates/user/registrymodifications.xcu` 单文件**(126→66→69 item 两轮净化: 第一轮 66 条(保留 3 工具栏 Visible=false+Locked/TabBarVisible=false/SlideSorterBar 按视图/Misc.Start 放映 4 条/Sidebar ContextList 10 条/FirstRun=false/两个 Factory 窗口属性=固定值 `10,1,1920,1080;1;,,,;`(原值机器相关 3725x1992, 模板须跨机器); 剔除: 最近文件/Recovery/绝对路径/时间戳/Linguistic/ooLocale(让环境决定)/默认值写回约 60 条), 第二轮 +3 条补 sidebar/statusbar(见⑦)); 构建 POST_BUILD 随 OfficeRuntime 部署到 office/program/templates/
- ④ **消费语义双平台统一**: 引导/会话创建时 fresh copy(回模板基线), Linux `SeedKernelProfile`(EnsureKernel 引导前; **活内核防护**: cmdline 含 soffice.bin+该 profile 的进程活着时跳过 — 跨进程共享内核复用路径绝不能删正在运行的内核的 profile), Windows 平台层 seed(office/user 退役)
- ⑤ **实证**: 模板三要素(工具栏/TabBar/窗口属性固定值)在运行 profile 中生效且 LO 写回不覆盖; 全链探针 20/20
- ⑥ **孤儿文档锁坑(新)**: 用户 UI soffice 会话退出后 `.~lock.<doc>#` 残留(锁跨 profile 生效!)→ 播放链 Hidden 加载返回空组件("doc loaded FAILED"), 表现为"任何 profile/模板配置下都失败" — 排查先查文档同目录锁文件; 2026-08-18 实测差点误判为模板回归
- ⑦ **sidebar/statusbar 的真实存储位置(2026-08-18 定位, 模板第二轮 +3 条的依据)**:
  - **Sidebar(View>Sidebar, Ctrl+F5)不是 LayoutManager 元素** — SFX 子窗口, SID_SIDEBAR = SID_SVX_START(10000)+336 = **10336**(sfx2/source/sidebar/SidebarChildWindow.cxx `SFX_IMPL_DOCKINGWINDOW_WITHID(SidebarChildWindow, SID_SIDEBAR)`), 持久化在 `/org.openoffice.Office.Views/Windows` 的 **`WindowType['simpress/10336']`** 节点(2 item: UserData + WindowState)。序列化是整条路径进 `oor:path=`, **grep `oor:name="simpress/10336"` 查不到**(审计时易误判缺失)
  - **Statusbar 不在 toolbar 命名节点** — `/org.openoffice.Office.UI.ImpressWindowState/UIElements/States` 直项内嵌 `<node oor:name="private:resource/statusbar/statusbar">` Visible=false; 白名单规则若要求路径含 `resource/toolbar/` 会漏掉它(第一轮净化就这么丢的)
  - **UNO 自省盲区**: LayoutManager.isElementVisible 对 SFX 子窗口(sidebar)报 0 而像素仍在(hideElement 对它不生效/无意义) — UI 自省不能完全反映真实布局, 肉眼是最终裁判(用户定论); 这两个元素的**有效控制层 = 模板**(三层优先级里的第三层在此场景反而是唯一起效的)
  - 模板再净化(用户从 UI 重新配置再提取)时的保留规则: 上述两条的位置匹配必须保留(States 直项路径精确匹配 + `simpress/10336` 内容匹配), 否则重新丢条目
- ⑧ **模板部署陈旧坑(2026-08-18 实测踩过)**: 仓库模板更新后 office/program/templates/ 部署副本仍是旧的(上次构建早于模板编辑) → Linux seed 用部署副本, 运行时 profile 一直缺条目, 症状="模板明明加了配置但不生效"。**规则: 改 templates/user 后必须重建 OfficeRuntime(POST_BUILD 拷贝)或手动 cp, 并 diff 确认部署副本一致**; 排查 UI 不生效先核对三方(仓库模板/部署副本/运行时 profile)条目数
- ⑨ **活内核 seed 跳过的验证姿势**: SeedKernelProfile 的活内核防护(cmdline 匹配跳过)意味着**改模板后若内核还活着, 新配置不生效** — 需确保 soffice 重启(探针退出会 atexit 停内核; demo 常驻进程需重启)

---

## 经验 38 详述：writer 渲染方案可行性

**writer 渲染方案可行性 (2026-08-17 探针实测)**

**方案 A(自治 PDF, 采用)**: docx → PDF → PDF 导入 Draw → 逐页 XSlideRenderer::createPreview → XBitmap::getDIB → BGRA。全链路 UNO 公开接口, LO 自治零第三方(mupdf/poppler 均不需要); 不需要 Xvfb/窗口/抓帧(纯离屏渲染) —— 契合"不用截屏和虚拟屏"与"内核稳定优先"。

**接口细节(落地直接复用)**:

- ① 转 PDF 用 `XStorable::storeToURL(url, {FilterName="writer_pdf_Export"})`(**XModel 无 storeToURL**; 同内核内转换, 不需要外部 --convert-to 进程, 独立 profile 隔离仍适用)
- ② PDF 导入 `loadComponentFromURL(pdf, FilterName="draw_pdf_Import")`(Hidden)
- ③ `XSlideRenderer` 服务名 `com.sun.star.drawing.SlideRenderer`(实现 com.sun.star.comp.Draw.SlideRenderer, sd/source/ui/presenter/SlideRenderer.cxx), `createPreview(XDrawPage, awt::Size(宽,高), superSample)` → `awt::XBitmap` —— 输出尺寸按页面比例适配(竖版 A4 @1080 高 → 763x1080, 完整页面)
- ④ `XBitmap::getDIB()` 返回 **BMP 文件格式**(非裸 DIB!): 'BM'(0-1) + 像素偏移(10-13=0x36=54) + BITMAPINFOHEADER(biWidth@18, biHeight@22, biBitCount@28=24bpp) + 行对齐 4 字节 —— 解析陷阱, 按 offset 10 的像素偏移取值, 勿假设 40 字节头

**性能实测**(pdf_render_probe, CMake `-DBUILD_TOOLS=ON`):

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

**上层接线**: 已回退 (见 HANDOFF.md 1.7)。LibreOfficeWriterManager.cpp/.h 作为样板保留在 NovaOfficeCore/word/ (去 IWordManager 依赖, 不参与构建)。

**质量收尾(2026-08-17 三轮)**:

- ⑦ **UpdateFrame 已修**: 原实现只置标志等轮询, Stop 后轮询线程已停→标志无人消费→"停止后取一帧"黑屏(writer_probe 复现 frames=0); 现锁内直接 PushFrame(语义对齐 impress)
- ⑧ **LO 统一尺寸认知(draw_pdf_Import)**: 混合页面尺寸 PDF(横竖混排实测 612x792/842x595/595x842)导入 Draw 后**所有页统一为第一页尺寸**, createPreview 全部同尺寸输出(834x1080)——"缓存命中不刷新 width_/height_ 的错配前提不存在"(per-page 尺寸处理不需要); writer_probe 的 WRITER_MIXED 段留作回归锚点(LO 行若变会 FAIL 提醒)
- ⑨ writerlink 纳入 linksmoke(ABI 一致性同机制, 单测 49→50 检查)
- ⑩ calc_session 精简 include 后 syscall 需显式 <unistd.h>(传递包含被移除暴露); 2026-08-18 改进: 加 <sys/syscall.h> 用 SYS_gettid 宏替代硬编码 186(x86_64=186, aarch64 不同, 可移植)

---

## 关键经验失效条件 (2026-08-20 补充)

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

---

## 零引用经验清单 (2026-08-20 审查)

> 以下经验在代码注释中无引用 (grep `经验 N` 命中 0 次), 但仍保留在第二章。原因: 编号锚定不可删 (未来代码可能引用); 经验本身仍承重 (踩坑风险仍在, 只是当前代码未引用)。维护时可优先考虑合并/降级这些经验。

**零引用经验**: 2 / 3 / 4 / 6 / 7 / 8 / 9 / 10 / 11 / 12 / 13 / 14 / 15 / 19c / 19d-g / 20 / 29 / 36 (共 19 条)

**说明**:
- 经验 19 基类 0 引用, 但变体 19b 有 6 处引用、19c/19d-g 完全 0 引用。19 的正文 (弯路勿重走 5 条) 已被 19b/19c/19d-g 完全拆分继承, 可考虑把基类 19 归并进 19b 并删除基类条目。
- 经验 13/14/15 (XShm 抓帧三件套) 虽 0 代码引用, 但实现已落地于 xvfb_platform 抓帧路径, 可改为附在 xvfb_platform.cpp 注释中作为"背景来源"。
- 经验 19d-g 自身注释已标注"已被 29/30 取代", 本身即承认零活引用, 可降级为附录"探索历程备查"。
- 完整代码引用审查报告见 git 历史 commit (2026-08-20 调研)。
