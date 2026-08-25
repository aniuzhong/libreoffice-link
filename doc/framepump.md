# 帧泵专项（HANDOFF.md 五章伴随文件）

> 对应经验 42 (详述见 [experiences.md](experiences.md))。
> 本文件收录设计决策论证、性能预算、测试矩阵等深度内容 (源自原始设计稿, 已落地)。
> FramePump 是会话基础设施 (common, 与 link_utils 平级), 与平台隔离设计正交。

## 0. 契约（三链同一份, FramePump 构造性保证）

状态机: `Idle ─Start─ Running ⇄(Pause/Resume)⇄ Paused`; 任意状态 `─Stop─ Stopped`; `Stopped ─Start─ Running`。

| 方法 | 契约 |
|---|---|
| Start | 幂等; 任何状态调用后 = Running 且**未暂停** (无条件 `paused_=false` 再判幂等, 构造性消灭 P1/P5; 经验 41 同构回归: 幂等早返未重置 paused_ 致暂停后无法翻页) |
| Stop | 幂等; join 泵线程, 排空在途帧; 之后无自动推帧。**V6**: 若调用线程即泵线程 (如帧回调内 Destroy→Stop), 跳过 join 自己 (UB/死锁), 只置 stop_requested 由泵线程自退出 |
| Pause | 冻结周期推帧 (心跳是否照推 = `plan.heartbeat_when_paused`); 绝不影响 UpdateFrame |
| Resume | 恢复周期推帧 |
| UpdateFrame | 同步立即帧, Running/Paused/Stopped 任意状态有效; 与 tick 串行 (frame_mutex_); **不去重** (调用方要的就是一帧), 成功仅更新 dedupe 基线 |

生命周期不变量: **泵必须先 Stop, 会话才能清 UNO 对象/平台资源** (probe/FrameFn 引用的 pane_/view_/platform_ 仅泵停止后可销毁)。
锁纪律: 调用泵方法不得持会话锁 mu_; FrameFn/ChangeFn 内部自取短锁; 全局锁序 `frame_mutex_ → mu_(短)`, 严禁反向。

## 1. 设计决策

### 决策一: frame_mutex_ 串行化, 否决"单线程委托"

泵内一把 `frame_mutex_`: 所有 FrameFn 执行 (泵 tick 与 UpdateFrame 调用方) 都在这把锁内, 调用方线程就地执行, 不向泵线程投递。

- **否决的替代方案**: UpdateFrame 发请求→唤醒泵线程执行→等待完成。否决理由: (a) 引入唤醒延迟与请求/应答机制; (b) Stopped 态仍须回退为调用方就地执行——最终还是两个执行上下文, 规则反而变复杂; (c) mutex 模型对所有状态只有一条规则。
- **效果**: P3 构造性消灭 (两处执行不可能并发); XShm/cap_bgra_ 单线程语义保持。
- **成本**: UpdateFrame 与 tick 短暂互斥, 最坏等待一帧抓取时间 (~1-2ms 1080p), 可忽略。

### 决策二: 锁纪律与锁序 (修 P4, 防新死锁)

1. 调用泵方法 (含 UpdateFrame) 时**不得持有会话锁 mu_**。会话的 UpdateFrame 实现退化为 `return pump_->UpdateFrame();`。
2. FrameFn/ChangeFn 内部**自取所需的短会话锁** (writer 读页缓存时), 抓帧与 cb_ 投递**绝不在 mu_ 内**。
3. 全局锁序: `frame_mutex_ → mu_(短)`, 严禁反向。probe 的 UNO 调用不持 mu_ (pane_/view_ 引用在泵运行期稳定, Destroy 先 Stop 保证)。

### 决策三: 静止检测两阶段 (无损优先, 阶段5 可选)

**原则: 无损耗优先于省电**——只允许跳过"字节级完全相同"的帧, 任何基于状态预判的方案都可能在三处盲区漏帧, 一律否决:
- impress 内部动画无 UNO 状态接口
- ffplay 媒体画面在 LO 渲染管线之外
- CPU 转场逐帧变化

否决的具体方案: XDamage (ffplay 独立顶层窗口不触发 LO 窗口的 damage → 视频漏帧)、采样签名 (小变化落抽样间隙 = 假阴性 = 漏帧)、UNO 动画状态查询 (无接口)。

- **阶段一 (纯会话层, 零平台改动)**: FrameFn 抓帧 (memcpy 后) 对上一已推帧做全量 memcmp (早退): 相同 → 只更新"末帧时间"不计推帧, 跳过 cb_ 及下游; 不同 → 拷贝/投递/缓冲互换。软件渲染确定性保证同输入同输出, 无假阳性抖动。成本: 动画/视频期 memcmp 首差异行早退 ≈ 0; 静止期全量扫描 8.3MB ≈ 0.3-1ms。收益: 静止期下游管线完全静默; 上游拷贝照付。
- **阶段二 (可选优化, 触碰平台接口)**: LinkPlatform 增量扩展 `CaptureFrame(..., bool& unchanged)` —— GrabBgra 在 XShm 段内与上帧先比对后拷贝 (XShmGetImage 服务端拷贝 ~0.01ms 级, 见经验 13, 保留; 省掉的是 8.3MB 应用层拷贝+alpha 填充)。属平台隔离设计的接口增量, 走其"改接口=双平台共同决策"流程。
- **契约联动 (开放问题 A)**: 若消费方存在"帧新鲜度 TTL"(超时视为无帧), 心跳必须保留 (推相同帧, 但阶段一已把内部拷贝省掉, 心跳近零成本); 若消费方是末帧语义, 静止期可完全静默, 208MB/s → 趋近 0。两阶段设计使该问题可延后决策且无论答案都不返工。

### 决策四: tick 循环合并探测与心跳

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

## 2. 性能预算 (1080p BGRA ≈ 8.3MB/帧)

| 场景 | 现状 (per-session poller) | FramePump 后 |
|---|---|---|
| impress 静止页 | 25fps 全量: 拷贝+alpha+回调+下游 ≈208MB/s | 阶段一: 服务端拷贝+memcmp(~0.5ms/tick), 下游 0; 阶段二: 趋近 0 |
| impress 动画/视频 | 25fps 全量 (必要) | 不变 (memcmp 早退 ≈0) |
| calc 空转 | 200 唤醒/s + 200 UNO IPC/s + 83MB/s 心跳 | 50 唤醒/s + 50 IPC/s + 心跳近零成本 (dedupe) |
| writer 空转 | 200 唤醒/s + 83MB/s 心跳 | 200 唤醒/s (tick=5 未放宽) + 心跳近零成本 (dedupe) |

注: writer tick 落地为 5ms (保守, 与原 PollThread 一致), 未按设计稿 20ms 放宽; dedupe (阶段一) 尚未实施, "心跳近零成本"为阶段一落地后的预期。

## 3. 测试矩阵

- **单元测试** (`common/frame_pump_test.cpp`, 树内编译不部署, 沿 office_runtime_test 模式): 9 个测试 15 checks — Start 幂等/重置、Stop 排空、Pause 冻结与 UpdateFrame 可用、UpdateFrame 与 tick 串行 (序列断言)、心跳间隔 (1ms tick 加速)、失败退避、Stop/Start 重启、ChangeFn 探测、Start 重置 paused_ 回归 (经验 41 同构)。
- **探针回归**: impress (impress_nextpage / media_green 双态 / impress_multi 并发)、writer (writer_probe 翻页/停止后取帧段)、calc (滚动/切表/缩放 + impress_multi 混跑观察 IPC 竞争)。
- **A/B 性能探针** (开放 B 用): calc 滚动在 tick 5/10/20ms 三档视觉对比。

## 4. 与平台隔离设计的关系

正交互补: FramePump 是会话基础设施 (common, 与 link_utils 平级), 不触碰 LinkPlatform 语义; 唯一交点 = 阶段二的 `CaptureFrame(+unchanged)` 增量, 走平台隔离设计的接口变更流程 (改接口=双平台共同决策)。落地顺序无依赖, 可并行推进。

## 5. 开放问题 (不影响阶段0-4, 可延后)

- **A. 帧新鲜度 TTL**: 消费方 (取帧链) 是否存在"末帧超时视为无帧"? 决定心跳保留 (近零成本) or 静止静默 (收益最大); 以及 hbp 三链统一值。验证手段: NovaPlayer PlayerItem.getVideoFrame 侧读帧逻辑确认。无论答案如何, 阶段0-4 不需要该答案 (dedupe 无内容风险, 心跳保留现状)。
- **B. calc tick 20ms 滚动延迟接受度** (最坏 +15ms): 接受 / 改 10ms (+5ms) / A/B 探针实测后定。默认 20ms, plan 一处可改。
