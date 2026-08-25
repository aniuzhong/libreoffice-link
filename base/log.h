// log.h — 统一日志声明 (跨模块共用: office_runtime/links/common 会话层)。
// 前缀规范 (定稿): [模块名] = target 名 — [OfficeRuntime] /
// [CalcLink] / [ImpressLink] / [WriterLink] / [Common]; 子场景点分
// (如 [CalcLink.Scroll] / [Common.UIHide])。ffplay 组件运行在 soffice 进程
// 内 (office_runtime.so 不在场), 保留独立 fprintf + [FFPLAY] 前缀。
// 级别 API:
//   OfficeLog(fmt, ...)     = info   生命周期主线 (默认, 兼容存量调用)
//   OfficeLogDbg(fmt, ...)  = debug  诊断细节 (窗口扫描/UI 自省/计时), 默认不落
//   OfficeLogWarn(fmt, ...) = warn   防御分支拦截/回退路径
//   OfficeLogErr(fmt, ...)  = error  失败
// 控制: ORT_LOG_LEVEL = debug|info|warn|error (默认 info);
//       ORT_LOG = both(默认)|file|stderr|off (输出通道, 经验 39 同款双平台实现)。
//   Linux: 实现唯一在 office_runtime.so (spdlog 单点, 共享内核单例语义,
//     勿在 common 编译第二份 — STATIC 复制会双写)。
//   Windows: common/win_office_log.cpp (各 link DLL 自带一份, 无单例需求)。
// 落位 office_paths::logs_dir()/office_<pid>.log。
#pragma once

void OfficeLog(const char* fmt, ...);
void OfficeLogDbg(const char* fmt, ...);
void OfficeLogWarn(const char* fmt, ...);
void OfficeLogErr(const char* fmt, ...);
