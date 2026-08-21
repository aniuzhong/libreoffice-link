# soffice loadComponentFromURL 排查实战 (2026-08-20)

> **触发**: attack_uaf_probe 全部 doc loaded FAILED, V4 验证空转。
> **根因**: 相对路径 → osl::FileBase::getFileURLFromSystemPath 生成非法 URL → soffice.bin 拒绝。
> **经验编号**: 46 (HANDOFF.md 二、经验索引)

---

## 1. 症状

attack_uaf_probe 传 `./test.xlsx` / `./test.pptx`:
- calc: `Create exception (unknown type)` — UNO 异常不继承 std::exception, 逃过 catch
- impress: `doc loaded FAILED` — loadComponentFromURL 返回 null (无异常)
- soffice.bin 输出 `Unspecified Application Error` (oosplash 报子进程异常)

所有 office_*.log (13:28 后) 无一条 `doc loaded OK`。

## 2. 排除项 (已验证正常)

| 排除项 | 验证方式 | 结果 |
|---|---|---|
| ffplay.so 13:12 重构版 | ffplay_engine_probe 双实例 | 全绿 (创建/播放/pause/seek/销毁隔离) |
| ffplay UNO 注入链 | ffplay_inject_probe | SUCCESS (Manager_FFPlay/createPlayer/跨进程 mute) |
| soffice.bin 进程内加载 | `soffice --convert-to pdf test.xlsx` | OK |
| soffice.bin 进程内加载 pptx | `soffice --convert-to pdf Reference.pptx` | OK |
| URP 远程加载 xlsx (绝对路径) | minimal_load_probe | OK (loadComponentFromURL OK) |
| URP 远程加载 xlsx (相对路径) | minimal_load_probe | FAIL ("Unsupported URL <./test.xlsx>") |

## 3. 根因

`osl::FileBase::getFileURLFromSystemPath` 对相对路径 `./test.xlsx` 不报错,
生成 `./test.xlsx` 而非 `file:///...`。soffice.bin 的 LoadEnv::startLoading
拒绝此 URL, 抛 `Unsupported URL` (calc 路径) 或静默返回 null (impress 路径)。

**产品代码不受影响**: CalcSession::Create / ImpressSession::Create 的调用方
(NovaOfficeCore) 传的是绝对路径。仅探针传相对路径时触发。

## 4. 治理措施

### 4.1 探针侧 (已修)
attack_uaf_probe 应传绝对路径。示例:
```bash
./attack_uaf_probe /home/hido/.../test.xlsx /home/hido/.../test.pptx all
```

### 4.2 代码侧 (建议, 非必须)
session Create 可在 `getFileURLFromSystemPath` 后校验 URL 以 `file://` 开头,
否则用 `realpath` 补全。优先级低 (产品调用方传绝对路径)。

### 4.3 验证侧 (已建)
新增 `minimal_load_probe` (tools/linux/), 最简 URP 远程 loadComponentFromURL
验证, 不依赖 session/FramePump/平台层。用法:
```bash
build/tools/minimal_load_probe /abs/path/to/file.xlsx
```

## 5. 残留问题: pptx URP 远程加载返回 null

xlsx 通过 URP 远程加载 OK, 但 pptx (test.pptx / Reference.pptx) 返回 null (无异常)。
soffice --convert-to 进程内能加载同一 pptx。疑点:
- impress 文档类型在 URP 远程模式下的加载路径差异
- Hidden 属性 + impress 初始化时序

此问题不影响 V4 验证 (calc UAF-1/UAF-2 已用绝对路径验证通过)。
