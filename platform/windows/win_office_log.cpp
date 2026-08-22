// win_office_log.cpp — OfficeLog 家族的 Windows 实现 (Linux 在
// office_runtime.so spdlog, 见 log.h; Windows 每 session 独立 soffice、
// 无共享内核, 由 common STATIC 提供, 各 link DLL 自带一份 — 无单例需求)。
// 日志落位对偶 Linux: office_paths::logs_dir()/office_<pid>.log
// (Windows = %LOCALAPPDATA%\office-link\logs, 可 ORT_HOME 覆盖)。
// ORT_LOG = both(默认)|file|stderr|off; ORT_LOG_LEVEL = debug|info(默认)|
// warn|error — 双变量语义与 Linux spdlog 完全对齐。
#include "../base/log.h"
#include "../base/office_paths.h" // .office-link 路径统一 (基目录/子路径派生)

#ifdef _WIN32

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace {

const char* LogMode() {
    const char* v = getenv("ORT_LOG");
    return v && *v ? v : "both";
}

bool LogToFile() {
    const char* v = LogMode();
    return strcmp(v, "file") == 0 || strcmp(v, "both") == 0;
}

bool LogToStderr() {
    const char* v = LogMode();
    return strcmp(v, "stderr") == 0 || strcmp(v, "both") == 0;
}

// 级别过滤 (与 Linux spdlog set_level 对齐): lvl 0=dbg 1=info 2=warn 3=err
bool LevelEnabled(int lvl) {
    const char* v = getenv("ORT_LOG_LEVEL");
    int threshold = 1; // info
    if (v) {
        if (strcmp(v, "debug") == 0) threshold = 0;
        else if (strcmp(v, "warn") == 0) threshold = 2;
        else if (strcmp(v, "error") == 0) threshold = 3;
    }
    return lvl >= threshold;
}

void WriteLog(int lvl, const char* tag, const char* fmt, va_list ap) {
    if (!LevelEnabled(lvl))
        return;
    char body[4096];
    vsnprintf(body, sizeof(body), fmt, ap);

    if (LogToFile()) {
        static std::string s_path;
        if (s_path.empty()) {
            // 路径统一 office_paths::logs_dir (2026-08-17; 基目录可 ORT_HOME 覆盖)
            std::string dir = office_paths::logs_dir();
            std::replace(dir.begin(), dir.end(), '/', '\\'); // CreateDirectoryA 需求
            std::filesystem::create_directories(dir,
                std::error_code()); // 建链; 失败忽略 (fopen 兜底)
            s_path = dir + "\\office_" + std::to_string(GetCurrentProcessId()) + ".log";
        }
        SYSTEMTIME st;
        GetLocalTime(&st);
        FILE* f = fopen(s_path.c_str(), "a");
        if (f) {
            // 格式与 Linux office_runtime spdlog 统一:
            // [YYYY-MM-DD HH:MM:SS.mmm] [level] message
            fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] %s\n",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                    st.wMilliseconds, tag, body);
            fclose(f);
        }
    }
    if (LogToStderr())
        fprintf(stderr, "[%s] %s\n", tag, body);
}

}  // namespace

void OfficeLog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    WriteLog(1, "info", fmt, ap);
    va_end(ap);
}
void OfficeLogDbg(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    WriteLog(0, "debug", fmt, ap);
    va_end(ap);
}
void OfficeLogWarn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    WriteLog(2, "warning", fmt, ap); // 级别名与 Linux spdlog %l 一致 (warning)
    va_end(ap);
}
void OfficeLogErr(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    WriteLog(3, "error", fmt, ap);
    va_end(ap);
}

#endif // _WIN32
