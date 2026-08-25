// ffplay_log.h — ffplay 组件日志基础设施 (ffplay 日志专项, 见 [ffplay-embed] §8)。
// header-only (inline + FFLOG 宏), C++17 magic statics 保证 per-process 单实例。
// 不改上游代码 (ffplay_embed.c / cmdutils.c / ffplay.c, patch 纪律, 经验 34)。
#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/ostream_sink.h>

extern "C" {
#include <libavutil/log.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>  // std::cerr (ostream_sink_mt)
#include <mutex>
#include <string>
#include <vector>

namespace ffplay_log {

// 日志目录 (与 office_runtime OfficeLog 同处; ORT_HOME 可覆盖)。
inline std::string LogsDir() {
    const char* ov = getenv("ORT_HOME");
    std::string home;
    if (ov && ov[0]) {
        home = ov;
    } else {
#ifdef _WIN32
        // Windows 走 %LOCALAPPDATA%\office-link (office_paths.h 同款决策)
        const char* local = getenv("LOCALAPPDATA");
        home = (local && local[0]) ? (std::string(local) + "/office-link")
                                   : "C:/office-link";
#else
        const char* h = getenv("HOME");
        home = (h && h[0]) ? (std::string(h) + "/.office-link")
                           : "/tmp/.office-link";
#endif
    }
    return home + "/logs";
}

// logger 懒初始化 (std::call_once 保证多线程并发安全; 首次调用 Init/Log 触发)。
inline std::shared_ptr<spdlog::logger> InitLogger() {
    static std::shared_ptr<spdlog::logger> logger;
    static std::once_flag once;
    std::call_once(once, []() {
        const char* mode = getenv("ORT_LOG");
        if (mode && strcmp(mode, "off") == 0) {
            logger = nullptr;
            return;
        }
        std::string dir = LogsDir();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::string file = dir + "/ffplay_" + std::to_string(getpid()) + ".log";
        try {
            std::vector<spdlog::sink_ptr> sinks;
            bool to_file = !mode || strcmp(mode, "both") == 0 || strcmp(mode, "file") == 0;
            bool to_stderr = !mode || strcmp(mode, "both") == 0 || strcmp(mode, "stderr") == 0;
            if (to_file)
                sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    file, 5 * 1024 * 1024, 3));
            if (to_stderr)
                sinks.push_back(std::make_shared<spdlog::sinks::ostream_sink_mt>(std::cerr));
            if (sinks.empty()) {
                logger = nullptr;
                return;
            }
            auto lg = std::make_shared<spdlog::logger>("ffplay", sinks.begin(), sinks.end());
            // 与 OfficeLog 同格式: [时间] [level] message
            lg->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
            // soffice.bin 子进程可能被异常终止 (SIGTERM/SIGKILL 或 dlclose 未触发析构),
            // 默认 buffer 不 flush 会丢日志。flush_on(info) 保证每条 info 及以上立即落盘。
            lg->flush_on(spdlog::level::info);
            const char* lvl = getenv("ORT_LOG_LEVEL");
            if (lvl && strcmp(lvl, "debug") == 0) lg->set_level(spdlog::level::debug);
            else if (lvl && strcmp(lvl, "warn") == 0) lg->set_level(spdlog::level::warn);
            else if (lvl && strcmp(lvl, "error") == 0) lg->set_level(spdlog::level::err);
            else lg->set_level(spdlog::level::info);
            logger = lg;
            // 文件首行交叉引用: pid = getpid() (soffice.bin 子进程, 可能与 office log
            // 报告的 osl_executeProcess 初始 pid 不同 — soffice 脚本 exec oosplash ->
            // soffice.bin 二级 fork; 主进程拿到的 pid 是初始脚本, ffplay.so 在最终
            // soffice.bin 内执行)。文件名带本 pid 即可定位。
            lg->info("[FFPLAY] log initialized, pid={}, file=ffplay_{}.log",
                     getpid(), getpid());
        } catch (const spdlog::spdlog_ex&) {
            logger = nullptr;  // 初始化失败回退 null (不崩)
        }
    });
    return logger;
}

// 日志入口: 与 OfficeLog 同风格, fmt 是 printf 格式串。
inline void Log(spdlog::level::level_enum lvl, const char* fmt, ...) {
    auto lg = InitLogger();
    if (!lg || lvl < lg->level()) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    spdlog::source_loc loc;  // 空 source_loc, 调用方位置由 fmt 内容自带 [FFPLAY] 前缀
    lg->log(loc, lvl, "{}", buf);
}

// AV_LOG_* → spdlog::level 映射 (ffmpeg/libavutil/log.h 常量)。
// AV_LOG_QUIET=-8, PANIC=0, FATAL=8, ERROR=16, WARNING=24, INFO=32, VERBOSE=40, DEBUG=48
inline spdlog::level::level_enum MapAvLevel(int av_level) {
    if (av_level <= AV_LOG_FATAL)    return spdlog::level::critical;
    if (av_level <= AV_LOG_ERROR)    return spdlog::level::err;
    if (av_level <= AV_LOG_WARNING)  return spdlog::level::warn;
    if (av_level <= AV_LOG_INFO)     return spdlog::level::info;
    if (av_level <= AV_LOG_VERBOSE)  return spdlog::level::debug;
    return spdlog::level::trace;  // AV_LOG_DEBUG
}

// ffmpeg av_log 自定义 callback: 转发到 spdlog, 输出 [FFmpeg/<module>] 前缀。
// module = AVClass.class_name (如 "avi"/"h264"/"mp3"/"matroska,webm"); avcl==NULL 时 module="ffmpeg"。
// 线程安全: spdlog rotating_file_sink_mt + ostream_sink_mt 内部已加锁,
// ffmpeg 多线程 codec 调 av_log 不会出问题 (符合 log.h:314 注释要求)。
inline void AvLogToSpdlog(void* avcl, int level, const char* fmt, va_list vl) {
    auto lg = InitLogger();
    if (!lg) return;
    auto mapped = MapAvLevel(level);
    if (mapped < lg->level()) return;
    const char* module = "ffmpeg";
    if (avcl) {
        // avcl 第一字段是 AVClass* (libavutil/log.h), 含 class_name 字符串
        auto* avc = *static_cast<AVClass**>(avcl);
        if (avc && avc->class_name) module = avc->class_name;
    }
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    spdlog::source_loc loc;
    lg->log(loc, mapped, "[FFmpeg/{}] {}", module, buf);
}

// 同步 ORT_LOG_LEVEL → av_log_set_level (ORT_LOG_LEVEL=debug → AV_LOG_DEBUG,
// 让 ENGINE-DBG 等 AV_LOG_DEBUG 级日志也输出; ffmpeg av_log 语义: level > av_log_level 过滤)。
inline void SyncAvLogLevel() {
    const char* lvl = getenv("ORT_LOG_LEVEL");
    int av_lvl = AV_LOG_INFO;
    if (lvl && strcmp(lvl, "debug") == 0) av_lvl = AV_LOG_DEBUG;     // 48 (含 ENGINE-DBG)
    else if (lvl && strcmp(lvl, "warn") == 0) av_lvl = AV_LOG_WARNING;
    else if (lvl && strcmp(lvl, "error") == 0) av_lvl = AV_LOG_ERROR;
    av_log_set_level(av_lvl);
}

// 初始化入口: 注册 av_log callback + 同步级别 (idempotent, std::call_once)。
// 调用时机: ffplay_get_implementation 入口 (ffplay.so 首次被 soffice.bin 加载时)。
inline void Init() {
    static std::once_flag s_once;
    std::call_once(s_once, []() {
        InitLogger();  // 触发 logger 创建 (写首行交叉引用日志)
        av_log_set_callback(AvLogToSpdlog);
        SyncAvLogLevel();
    });
}

}  // namespace ffplay_log

// 便捷宏: 替换原 fprintf(stderr, "[FFPLAY] ...") 的样板。调用方仍自带 [FFPLAY] 前缀
// 保持与现有日志风格一致 (日志 reader grep [FFPLAY] 即可拿到 ffplay 组件全部日志)。
#define FFLOG_INFO(...) ffplay_log::Log(spdlog::level::info,  __VA_ARGS__)
#define FFLOG_WARN(...) ffplay_log::Log(spdlog::level::warn,  __VA_ARGS__)
#define FFLOG_ERR(...)  ffplay_log::Log(spdlog::level::err,   __VA_ARGS__)
#define FFLOG_DBG(...)  ffplay_log::Log(spdlog::level::debug, __VA_ARGS__)
