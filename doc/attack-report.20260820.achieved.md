# 攻击性测试报告

> 评分规则：令程序**崩溃**（进程非零退出 / SIGABRT / 核心转储）或**卡死**（超时无响应）均计 **1 分**。
> 测试日期：2026-08-20；环境：Linux x86_64，LibreOffice 自定义构建（kylin 部署），共享内核 + Xvfb :90。

## 测试得分

**最终得分：4 分**（新增 4 个攻击探针，复用了已有攻击探针未覆盖的方向）。

> ⚠️ 历史记录更正：原报告记录 `attack_race_probe` 并发竞态 "1 分"（崩溃于 `IllegalArgumentException`）。
> 本次**重新实测 `attack_race_probe all` 结果为 0/2（EXIT=0，未崩溃、未卡死）**——
> 该探针的崩溃要么是当时 BootLock 超时等瞬态环境问题的误判，要么代码已被硬化。
> 原 "1 分" 视为**失效/不可复现**，已从有效得分中剔除。有效得分为下方 4 项。

## 得分攻击详情（4 项）

### 攻击 A：重复 Destroy 导致 Use-After-Free（确定性崩溃）
- **探针**：`attack_uaf_probe`（攻击点 att=2：`attack_null_double_destroy`）
- **攻击效果**：**崩溃 ✅ EXIT=134 (SIGABRT, 核心转储)**
- **确定性**：**必现**（与运行时序无关）
- **根因**：C ABI 层 `CalcSessionDestroy` / `ImpressSessionDestroy` 对"已销毁对象"无防护。
  第一次 `Destroy` 执行 `delete this`，对象内存已释放；随后再次 `Destroy` / `Start` / `NextPage`
  / `UpdateFrame` / `SetResolution` 均直接操作悬垂指针 → use-after-free，被 libc 以 SIGABRT 兜底。
- **复现命令**：
  ```bash
  ./build_probes.sh attack_uaf_probe
  pkill -9 Xvfb soffice; rm -f /tmp/.X9*-lock /tmp/.X11-unix/X9* \
        /dev/shm/nova_office_slots_v1 /dev/shm/sem.nova_office_boot
  ./attack_uaf_probe test.xlsx test.pptx 2
  # EXIT=134
  ```

### 攻击 B：销毁后立即同堆地址重建（确定性卡死）
- **探针**：`attack_uaf_probe`（攻击点 att=3：`attack_double_create_same_slot`）
- **攻击效果**：**卡死 ✅ EXIT=124（超时）**
- **确定性**：**必现**
- **根因**：200 轮 `Create→Start→Destroy`（即 `delete this`）后立即再次 `Create`。
  堆分配器极可能复用同一地址，而 `Destroy` 触发的 `BootLock`/profile seed 在快速
  teardown-recreate 节奏下发生级联阻塞（与 HANDOFF 经验 43 "BootLock 强制释放无日志无保护"
  同源的串行化弱点）。
- **复现命令**：
  ```bash
  ./attack_uaf_probe test.xlsx test.pptx 3   # EXIT=124
  ```

### 攻击 C：impress 高频 resize 风暴（确定性卡死）
- **探针**：`attack_resize_probe`（攻击点 att=3：`attack_rapid_resize`）
- **攻击效果**：**卡死 ✅ EXIT=124（超时）**
- **确定性**：**必现**
- **根因**：5000 次 `ImpressSessionSetResolution` 无停顿调用，每次都走
  `platform_->SetWindowSize` → XShm 段反复申请/释放/重建。在 Xvfb 无 GPU + 软件渲染
  路径下，连续 resize 触发段重建路径的串行化/阻塞，最终卡死。
  （对比 att=1 并发 resize vs 抓帧、att=2 非法尺寸 0/负/32768/INT_MAX **均未崩溃**，
  说明崩溃点集中在"无停顿高频 resize"而非单次非法尺寸。）
- **复现命令**：
  ```bash
  ./attack_resize_probe test.xlsx test.pptx 3   # EXIT=124
  ```

### 攻击 D：Destroy 与帧泵 PollThread 竞态（竞态崩溃，组合运行确凿）
- **探针**：`attack_uaf_probe`（攻击点 att=1：`attack_destroy_race`）
- **攻击效果**：**崩溃 ✅**（组合运行 `attack_uaf_probe all` 时 `EXIT=134`，
  报错 `terminate called after throwing an instance of 'com::sun::star::lang::IllegalArgumentException'` 并核心转储）
- **确定性**：**竞态依赖时序**（隔离运行 att=1 未必命中，但 8 线程 `Create→Start→sleep(3~8ms)→Destroy`
  与帧泵 tick 重叠时确凿崩溃）
- **根因**：`Destroy()` 在 `mu_` 之外 `reset platform_` / `pump_`，而
  `FramePump::PollThread` 持 `frame_mutex_` 运行 `frame_fn_`（即 `CaptureFrame`，触及 `platform_`）。
  两把锁（`mu_` vs `frame_mutex_`）不重合，存在窗口期：泵正在抓帧时 `platform_` 被 reset，
  下一帧 `CaptureFrame` 访问已释放资源 → 非法参数/崩溃。
- **复现命令**：
  ```bash
  ./attack_uaf_probe test.xlsx test.pptx all   # 关注 [UAF-1] 段 EXIT=134
  ```

## 未得分（稳健，未崩溃/未卡死）

| 探针 | 攻击方向 | 结果 |
|---|---|---|
| `attack_pagenav_probe` | impress 越界 goto / 超界 NextPage / stopped 态翻页 / 并发翻页；calc 切表越界 / 极端缩放 / 海量滚动 | 0/3 稳健 |
| `attack_mute_teardown_probe` | 销毁中 SetMute / owner 退出连坐 / mute 抖动 | 0/3 稳健 |
| `attack_resize_probe` att=1 | 并发 resize vs 抓帧 | 稳健 |
| `attack_resize_probe` att=2 | 非法尺寸（0/负/32768/INT_MAX） | 稳健 |
| `attack_race_probe`（原 1 分） | 8 线程并发 Create（BootLock 死锁测试） | **重新实测 0/2，原 1 分失效** |

## 攻击成功代码（新增 4 探针核心攻击点）

> 完整源码见 `xvfb_calc_demo/attack_uaf_probe.cpp`、`attack_resize_probe.cpp`、
> `attack_pagenav_probe.cpp`、`attack_mute_teardown_probe.cpp`。
> 均已登记进 `xvfb_calc_demo/build_probes.sh`（链接组 `attack_new = -l:impresslink.so -l:calclink.so -lX11 -lXext`）。

### A. 重复 Destroy（UAF att=2）—— 必现崩溃
```cpp
void* s = CalcSessionCreate(xlsx, "", "attack_uaf", OnFrame, nullptr, 1920, 1080);
if (s) {
    CalcSessionStart(s);
    CalcSessionDestroy(s);     // delete this
    CalcSessionDestroy(s);     // 悬垂指针 → use-after-free → SIGABRT
    CalcSessionStart(s);       // 同样崩溃
    CalcSessionNextPage(s);
    CalcSessionSetResolution(s, 800, 600);
}
// 另测 NULL 防护缺口：所有 C ABI 在传入 nullptr 时仅返回 0，
// 但若某 API 漏判即崩溃（本探针均传入验证，当前全部安全返回）。
```

### B. 销毁后立即重建（UAF att=3）—— 必现卡死
```cpp
for (int i = 0; i < 200; i++) {
    void* s = ImpressSessionCreate(pptx, "", "attack_uaf", OnFrame, nullptr, 1920, 1080);
    if (s) {
        ImpressSessionStart(s);
        ImpressSessionDestroy(s);   // delete this；立即再次分配复用同地址
    }
}
```

### C. impress 高频 resize 风暴（RESIZE att=3）—— 必现卡死
```cpp
void* p = ImpressSessionCreate(pptx, "", "attack_resize", OnFrame, nullptr, 1920, 1080);
ImpressSessionStart(p);
for (int i = 0; i < 5000; i++) {
    int w = (i % 2) ? 1280 : 2560;
    int h = (i % 2) ? 720 : 1440;
    ImpressSessionSetResolution(p, w, h);   // XShm 段反复重建，无停顿
    if (i % 50 == 0) ImpressSessionUpdateFrame(p);
}
```

### D. Destroy 与帧泵竞态（UAF att=1）—— 组合运行确凿崩溃
```cpp
std::atomic<bool> stop{false};
for (int r = 0; r < 8; r++) {
    ts.emplace_back([&, r]() {
        while (!stop.load()) {
            void* s = CalcSessionCreate(xlsx, "", "attack_uaf", OnFrame, nullptr, 1920, 1080);
            if (!s) continue;
            CalcSessionStart(s);
            // 极短存活：在泵 tick(20ms) 内即销毁，最大化 Destroy 命中 PollThread 的概率
            std::this_thread::sleep_for(std::chrono::milliseconds(r % 2 ? 3 : 8));
            CalcSessionDestroy(s);   // platform_/pump_ reset 与抓帧竞态
        }
    });
}
```

## 攻击技术分析

### 共性结论
1. **C ABI 缺"已销毁/已失效"守卫**：`*SessionDestroy` 可重复调用（崩溃）、可调用 `nullptr`（当前安全返回，
   但属脆弱约定）。修复方向：在 `ImpressSession`/`CalcSession` 内部维护 `destroyed_` 标志，
   `Destroy` 幂等（重复调用 no-op），其余 API 在 `!created_` 时直接返回失败而非触碰悬垂成员。
2. **`Destroy` 与帧泵的生命周期边界是最大雷区**：`Destroy` 在 `mu_` 外 reset `platform_/pump_`，
   而 `FramePump::PollThread` 持 `frame_mutex_` 跑 `CaptureFrame`。两锁不重合 → 竞态窗口（攻击 D）。
   经验 42 的契约"泵必须先 Stop，会话才能清 UNO/平台资源"依赖 `Destroy` 先 `pump_->Stop()` 并 `join`，
   但若 `Stop` 与仍在途的 `UpdateFrame`（调用方线程持 `frame_mutex_`）交错，窗口期仍存。
3. **高频 resize 卡死**：`SetWindowSize` 的 XShm 段重建在 Xvfb 软件渲染下串行阻塞加重，
   无停顿风暴直接卡死（与单次非法尺寸稳健形成对照，说明是频率/重建路径而非参数校验问题）。

### 关联经验编号
- 经验 43：BootLock 构造即加锁 + 非递归 mutex 自死锁（攻击 B 的级联阻塞同源）
- 经验 42：FramePoller/FramePump 治理（攻击 D 的泵生命周期竞态；P3 锁纪律 `frame_mutex_→mu_`）
- HANDOFF 1.6：所有者退出连坐（攻击 B 的快速 teardown 触发）

## 建议

1. **立即修复重复 Destroy（攻击 A）**：在会话类加 `destroyed_` 守卫，`Destroy` 幂等；所有公开 API
   首行判 `if (!created_ || destroyed_) return false/0;`。这是**确定性必现**缺陷，优先级最高。
2. **收敛 Destroy 与帧泵的竞态（攻击 D）**：确保 `pump_->Stop()` 的 `join` 完成前不 reset `platform_`；
   可考虑在 `Stop` 内用 `frame_mutex_` 保护 `platform_` 的最后一个使用点。
3. **resize 节流/串行化（攻击 C）**：`SetWindowSize` 的 XShm 重建应串行于帧泵或加最小间隔去抖。
4. **撤销原报告中失效的 1 分**：`attack_race_probe` 并发竞态当前实测 0/2，不应计入得分。

## 测试环境

- OS: Linux 5.10 x86_64（kylin 部署）
- LibreOffice: 自定义构建版本（commit 83e0b9c3e 固化远端）
- 部署：NovaPlayer/bin_x86_64_kylin/office/program（calclink.so / impresslink.so / office_runtime.so / ffplay.so）
- 测试文件：test.xlsx、test.pptx（最小有效文件）
- 运行前需清场：`pkill -9 Xvfb soffice; rm -f /tmp/.X9*-lock /tmp/.X11-unix/X9* /dev/shm/nova_office_slots_v1 /dev/shm/sem.nova_office_boot`
- 日志降噪：`ORT_LOG_LEVEL=warn`（默认 info 会在压测下产生海量 per-frame 日志）
