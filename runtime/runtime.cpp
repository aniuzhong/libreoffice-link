// office_runtime.cpp — 进程级共享 LibreOffice 运行时 (Linux 共享内核模式)
// 实现从 calc_session.cpp / linux/calc_platform.cpp 迁移 (历史说明: 平台
// 公共侧 2026-08-14 已归组至 common/linux/xvfb_platform.cpp, 经验 32):
//   LO 共享内核 (bootstrap/复用/引用计数)      <- calc_session.cpp
//   Xvfb 生命周期 (启动/显示号/回收)           <- linux/calc_platform.cpp
//   Slot 分区 (alloc/free/位图)              <- linux/calc_platform.cpp
#include "runtime.h"
#include <base/office_paths.h> // .office-link 路径统一 (header-only, 零依赖)
#include "../third_party/scope_guard.hpp" // DEFER: C 资源清理 (XCloseDisplay/munmap/close)

#include <dlfcn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/time.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>

#include <cppuhelper/bootstrap.hxx>
#include <com/sun/star/frame/XDesktop.hpp>
#include <com/sun/star/bridge/UnoUrlResolver.hpp>
#include <com/sun/star/connection/NoConnectException.hpp>
#include <osl/file.hxx>
#include <osl/process.h>
#include <osl/security.hxx>
#include <rtl/bootstrap.hxx>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/ostream_sink.h>

namespace {

// Display number range for Xvfb instances (:90-:99)
constexpr int kDisplayMin = 90;
constexpr int kDisplayMax = 100; // exclusive upper bound

// ---- 统一日志 (spdlog 惰性初始化) ----
// 首次 OfficeLog 调用时初始化 (calclink/impresslink 的 Create begin 早于
// EnsureKernel)。
// 单点初始化: 所有依赖 office_runtime.so 的进程/库共享同一 logger。
// 环境变量 ORT_LOG: both(默认, 文件+stderr) | file | stderr | off;
// ORT_LOG_LEVEL: debug|info(默认)|warn|error (spdlog level)。
void InitOfficeLog() {
    const char* mode = getenv("ORT_LOG");
    if (mode && strcmp(mode, "off") == 0)
        return;
    std::string dir = office_paths::logs_dir(); // 统一路径命名空间 (office_paths.h)
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string file = dir + "/office_" + std::to_string(getpid()) + ".log";
    try {
        std::vector<spdlog::sink_ptr> sinks;
        bool to_file = !mode || strcmp(mode, "both") == 0 || strcmp(mode, "file") == 0;
        bool to_stderr = !mode || strcmp(mode, "both") == 0 || strcmp(mode, "stderr") == 0;
        if (to_file)
            sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                file, 5 * 1024 * 1024, 3));
        if (to_stderr)
            sinks.push_back(std::make_shared<spdlog::sinks::ostream_sink_mt>(std::cerr));
        if (sinks.empty())
            return;
        auto logger = std::make_shared<spdlog::logger>("office", sinks.begin(), sinks.end());
        // 格式与 Windows win_office_log 统一: [时间] [level] message
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        // 与 ffplay_log.h 同款: flush_on(info) 保证每条 info 及以上立即落盘,
        // 防止主进程异常退出 (SIGKILL/abort) 时 spdlog buffer 丢失日志,
        // 也便于运行期实时监控 (tail -f 不需等 buffer 满)。
        logger->flush_on(spdlog::level::info);
        // 级别: ORT_LOG_LEVEL (默认 info; debug 级为诊断细节, 平时关闭)
        const char* lvl = getenv("ORT_LOG_LEVEL");
        if (lvl && strcmp(lvl, "debug") == 0)
            logger->set_level(spdlog::level::debug);
        else if (lvl && strcmp(lvl, "warn") == 0)
            logger->set_level(spdlog::level::warn);
        else if (lvl && strcmp(lvl, "error") == 0)
            logger->set_level(spdlog::level::err);
        else
            logger->set_level(spdlog::level::info);
        spdlog::set_default_logger(logger);
    } catch (const spdlog::spdlog_ex&) {
        // 初始化失败时回退 stderr (不崩)
    }
}

}  // namespace

// 统一日志入口 (log.h): 时间戳由 spdlog pattern 提供, 前缀由调用方传入
// ("[OfficeRuntime]"/"[CalcLink]"...), 跨进程时序靠时间戳对照。
// 级别变体见 log.h (info 默认 / Dbg 诊断 / Warn 防御 / Err 失败)。
namespace {
// ORT_LOG=off 时真正静默: 不初始化 logger 且直接丢弃 (spdlog 默认 logger
// 会打 stdout, 仅跳过初始化并不能关掉 — 2026-08-17 收尾修正)
bool OfficeLogOff() {
    const char* mode = getenv("ORT_LOG");
    return mode && strcmp(mode, "off") == 0;
}
void OfficeLogV(spdlog::level::level_enum lvl, const char* fmt, va_list ap) {
    static std::once_flag s_once;
    std::call_once(s_once, InitOfficeLog);
    if (OfficeLogOff())
        return;
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    spdlog::log(lvl, "{}", buf);
}
} // namespace

void OfficeLog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    OfficeLogV(spdlog::level::info, fmt, ap);
    va_end(ap);
}
void OfficeLogDbg(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    OfficeLogV(spdlog::level::debug, fmt, ap);
    va_end(ap);
}
void OfficeLogWarn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    OfficeLogV(spdlog::level::warn, fmt, ap);
    va_end(ap);
}
void OfficeLogErr(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    OfficeLogV(spdlog::level::err, fmt, ap);
    va_end(ap);
}

// office_runtime.so 所在目录 (部署于 office/program 时 == UNO 组件目录)
std::string GetRuntimeDir() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&GetRuntimeDir), &info) && info.dli_fname) {
        std::string p = info.dli_fname;
        auto pos = p.rfind('/');
        return pos == std::string::npos ? std::string(".") : p.substr(0, pos);
    }
    return ".";
}

// 等待 Xvfb 显示可达
bool WaitForX(const std::string& dpy) {
    for (int i = 0; i < 50; i++) {
        Display* d = XOpenDisplay(dpy.c_str());
        if (d) {
            XCloseDisplay(d);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

std::string ReadProcFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

// /proc/<pid>/stat 的 state (第 3 字段; comm 可能含空格/括号, 从末个 ')' 后解析)
char ProcState(pid_t pid) {
    if (pid <= 0)
        return 0;
    const std::string stat = ReadProcFile("/proc/" + std::to_string(pid) + "/stat");
    auto close = stat.rfind(')');
    if (close == std::string::npos)
        return 0;
    std::istringstream ss(stat.substr(close + 1));
    char state = 0;
    ss >> state;
    return state;
}

// "可服务"判定: 进程存在 且 非僵尸。
// kill(pid,0) 对僵尸进程仍返回成功 —— Xvfb 锁检查/slot 回收/清场判定
// 若把僵尸当活服务器, 会走 adopt 分支后 WaitForX 失败 (或误占 slot)。
bool ProcAlive(pid_t pid) {
    return pid > 0 && kill(pid, 0) == 0 && ProcState(pid) != 'Z';
}

// /proc/<pid>/stat 的 ppid (第 4 字段; comm 可能含空格/括号, 从末个 ')' 后解析)
pid_t ReadStatPpid(const std::string& stat) {
    auto close = stat.rfind(')');
    if (close == std::string::npos)
        return 0;
    std::istringstream ss(stat.substr(close + 1));
    char state = 0;
    pid_t ppid = 0;
    ss >> state >> ppid;
    return ppid;
}

// /tmp/.X<N>-lock 中的服务器 PID
pid_t ReadXvfbLockPid(int display_num) {
    std::ifstream f("/tmp/.X" + std::to_string(display_num) + "-lock");
    pid_t pid = 0;
    f >> pid;
    return pid;
}

// ---- 启动前清场: 清理本 office 的孤儿 soffice.bin ----
// 宿主进程崩溃/SIGKILL 时, 其 soffice.bin 子进程可能残留 (管道断开后
// 未必立即自退)。安全判定 (三个条件缺一不杀):
//   1. 路径匹配: cmdline 含 "office/program/soffice.bin" (排除系统 LO)
//   2. 同用户: /proc 只能读同用户进程 (天然满足)
//   3. 孤儿: ppid==1 (宿主已死, 被 init 收养)
//      或: DISPLAY 对应的 Xvfb 已死 (挂着 UNO 但无显示 = 无主僵尸)
// 活内核 (ppid=活宿主 且 Xvfb 活) 绝不会被误杀。
void CleanupOrphanSoffice() {
    static const std::string kMarker = "office/program/soffice.bin";
    int scanned = 0;
    int killed = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc")) {
        const std::string name = entry.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; }))
            continue;
        pid_t pid = std::atoi(name.c_str());
        if (pid <= 0 || pid == getpid())
            continue;
        scanned++;
        const std::string cmdline = ReadProcFile(entry.path().string() + "/cmdline");
        if (cmdline.find(kMarker) == std::string::npos)
            continue; // 不是我们的 soffice

        const pid_t ppid = ReadStatPpid(ReadProcFile(entry.path().string() + "/stat"));
        const std::string env = ReadProcFile(entry.path().string() + "/environ");
        // DISPLAY=:N → 锁内 PID 死 = Xvfb 已死。**只判定我们的 Xvfb 号段
        // (90-99)**: 真实显示 (如 :0, Xorg 同样有 /tmp/.X0-lock) 不在本模块
        // 号段内, 按锁判死会把正在 --convert-to 的转换进程误杀 (2026-08-13
        // 实测: 转换进程继承宿主 DISPLAY=:0, 被当孤儿 SIGKILL -> PDF 未生成
        // -> 上层 assert 崩)。
        bool xvfb_dead = false;
        size_t pos = env.find("DISPLAY=:");
        if (pos != std::string::npos) {
            int num = std::atoi(env.c_str() + pos + 9);
            if (num >= kDisplayMin && num < kDisplayMax)
                xvfb_dead = !ProcAlive(ReadXvfbLockPid(num));
        }
        if (ppid == 1 || xvfb_dead) {
            OfficeLog("[OfficeRuntime] cleanup: kill orphan soffice pid=%d (ppid=%d, xvfb_dead=%d)", (int)pid, (int)ppid, xvfb_dead ? 1 : 0);
            kill(pid, SIGKILL);
            killed++;
        }
    }
    OfficeLog("[OfficeRuntime] cleanup: scanned %d procs, killed %d orphan soffice", scanned, killed);
}

// 内核 profile 初始化: 从部署模板 (templates/user, 仓库 git 管理) fresh copy。
// 语义 (2026-08-18, 与 Windows session seed 同一规则): 引导时重建 = 回到模板
// 基线, LO 运行期写回的状态 (UI/窗口残留) 不跨引导存活。
// 活内核防护: 该 profile 正被某 soffice.bin 使用时 (cmdline 匹配, CleanupOrphan-
// Soffice 同款判定) 跳过 —— 跨进程共享内核场景下复用引导路径会走到这里,
// 绝不能删正在运行的内核的 profile。
// 模板缺失 (部署未带) = 保留现有 profile, 行为退化为 LO 自建默认。
void SeedKernelProfile(const std::string& profile) {
    namespace fs = std::filesystem;
    std::string tmpl = GetRuntimeDir() + office_paths::user_template() + "/registrymodifications.xcu";
    if (!fs::exists(tmpl)) {
        OfficeLogWarn("[OfficeRuntime] SeedKernelProfile: template missing (%s), skip seed", tmpl.c_str());
        return;
    }
    for (const auto& entry : fs::directory_iterator("/proc")) {
        const std::string name = entry.path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; }))
            continue;
        const std::string cmdline = ReadProcFile(entry.path().string() + "/cmdline");
        if (cmdline.find("office/program/soffice.bin") == std::string::npos)
            continue;
        if (cmdline.find(profile) != std::string::npos) {
            OfficeLog("[OfficeRuntime] SeedKernelProfile: live kernel on this profile (pid %s), skip",
                      name.c_str());
            return;
        }
    }
    std::error_code ec;
    fs::remove_all(profile, ec);
    std::string user_dir = profile + "/user";
    fs::create_directories(user_dir, ec);
    fs::copy_file(tmpl, user_dir + "/registrymodifications.xcu",
                  fs::copy_options::overwrite_existing, ec);
    if (ec)
        OfficeLogWarn("[OfficeRuntime] SeedKernelProfile: copy failed (%s)", ec.message().c_str());
    else
        OfficeLog("[OfficeRuntime] SeedKernelProfile: seeded from %s", tmpl.c_str());
}

// 复制官方 cppu::bootstrap 逻辑 (cppuhelper/source/bootstrap.cxx:84-229) + 可选
// -env:UserInstallation。profile 空 = 官方默认 profile (本函数底层语义);
// 非空 = 指定 profile。播放内核的实际 profile 由 EnsureKernel 决定
// (空参数时映射为 ~/.office-link/xvfb, 见经验 27; 引导前 SeedKernelProfile
// 从模板初始化)。踩坑记录见 HANDOFF 经验 23:
//   1. 必须先设 URE_BOOTSTRAP (宏展开器定位 fundamentalrc, unorc 的 ${ORIGIN}
//      依赖它; 缺失 -> UNO_TYPES 解析不全 -> binaryurp writeType 段错误)
//   2. 客户端进程需 UNO_PATH (EnsureKernel 已设)
//   3. 连接串 StarOffice.ComponentContext + UNO_QUERY_THROW (ServiceManager/Object 均不行)
//   4. 启动用 osl_executeProcess (官方原样)
css::uno::Reference<css::uno::XComponentContext> BootstrapOffice(const std::string& profile) {
    // 0. URE_BOOTSTRAP: 定位 fundamentalrc (bootstrap.cxx:123-133 同款)
    rtl::OUString uri;
    if (!rtl::Bootstrap::get("URE_BOOTSTRAP", uri)) {
        rtl::OUString prog_dir;
        osl::FileBase::getFileURLFromSystemPath(
            rtl::OStringToOUString(
                rtl::OString(GetRuntimeDir().c_str()), RTL_TEXTENCODING_UTF8),
            prog_dir);
        rtl::Bootstrap::set("URE_BOOTSTRAP",
                            rtl::Bootstrap::encode(prog_dir + "/fundamentalrc"));
    }

    // 1. 本地上下文 (客户端能力, 不启动 soffice) — bootstrap.cxx:136 同款
    auto local = cppu::defaultBootstrap_InitialComponentContext();
    if (!local.is()) {
        OfficeLogErr("[OfficeRuntime] BootstrapOffice: local context FAILED");
        return nullptr;
    }

    // 2. 随机 pipe 名 (bootstrap.cxx:141-153 用 rtlRandom; pid+单调时钟足够唯一)
    std::string pipe = "uno" + std::to_string(getpid()) + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());

    // 3. 参数: 官方 5 个 (bootstrap.cxx:156-162) + 可选 -env:UserInstallation
    std::vector<std::string> args = { "--nologo", "--nodefault", "--norestore", "--nolockcheck" };
    std::string env_arg;
    if (!profile.empty()) {
        std::filesystem::create_directories(profile);
        env_arg = "-env:UserInstallation=file://" + profile;
    }
    std::string acc_arg = "--accept=pipe,name=" + pipe + ";urp;";

    // 4. osl_executeProcess 启动 (bootstrap.cxx:170-180 原样)
    ::osl::Security sec;
    std::vector<rtl::OUString> args2;
    for (const auto& a : args)
        args2.push_back(rtl::OUString::createFromAscii(a.c_str()));
    if (!env_arg.empty())
        args2.push_back(rtl::OUString::createFromAscii(env_arg.c_str()));
    args2.push_back(rtl::OUString::createFromAscii(acc_arg.c_str()));
    std::vector<rtl_uString*> ar;
    for (auto& a : args2)
        ar.push_back(a.pData);
    rtl::OUString soffice_url;
    osl::FileBase::getFileURLFromSystemPath(
        rtl::OStringToOUString(
            rtl::OString((GetRuntimeDir() + "/soffice").c_str()), RTL_TEXTENCODING_UTF8),
        soffice_url);
    oslProcess hProcess = nullptr;
    oslProcessError rc = osl_executeProcess(soffice_url.pData, ar.data(), ar.size(),
        osl_Process_DETACHED, sec.getHandle(), nullptr, nullptr, 0, &hProcess);
    if (rc != osl_Process_E_None) {
        OfficeLogErr("[OfficeRuntime] BootstrapOffice: osl_executeProcess FAILED rc=%d", (int)rc);
        return nullptr;
    }
    // 暴露子进程 pid 便于 ffplay log 配对。注: osl_executeProcess 启动 soffice 脚本,
    // 脚本 exec oosplash → fork soffice.bin (二级 fork), 最终 ffplay.so 在 soffice.bin
    // 内执行 getpid() 拿到的是 soffice.bin 的 pid (与本处 pinfo.Ident 不同)。
    // 配对方式: ls ~/.office-link/logs/ffplay_*.log 找时间最新的 (与本 office log 同时段)。
    oslProcessInfo pinfo;
    pinfo.Size = sizeof(oslProcessInfo);
    if (osl_getProcessInfo(hProcess, osl_Process_IDENTIFIER, &pinfo) == osl_Process_E_None) {
        OfficeLog("[OfficeRuntime] soffice child started initial_pid=%lu "
                  "(soffice script; final soffice.bin pid differs due to 2-stage fork; "
                  "ffplay log: see latest ~/.office-link/logs/ffplay_*.log by mtime)",
                  (unsigned long)pinfo.Ident);
    }
    osl_freeProcessHandle(hProcess);

    // 5. 等待连接 (bootstrap.cxx:199-221 同款: 死循环等 NoConnectException)
    auto resolver = css::bridge::UnoUrlResolver::create(local);
    std::string url = "uno:pipe,name=" + pipe + ";urp;StarOffice.ComponentContext";
    css::uno::Reference<css::uno::XComponentContext> remote;
    for (;;) {
        try {
            remote.set(resolver->resolve(rtl::OUString::createFromAscii(url.c_str())),
                       css::uno::UNO_QUERY_THROW);
            break;
        } catch (const css::connection::NoConnectException&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        } catch (const css::uno::Exception& e) {
            OfficeLogErr("[OfficeRuntime] BootstrapOffice: resolve EXC: %s",
                  rtl::OUStringToOString(e.Message, RTL_TEXTENCODING_UTF8).getStr());
            break;
        }
    }
    return remote;
}


OfficeRuntime& OfficeRuntime::Instance() {
    // 共享库内仅一份, 跨调用方共享 (Linux so 单例机制)。
    // 故意泄漏: 进程退出时不析构 (静态析构阶段对 LO 内核的任何 UNO 调用
    // 会段错误), 由 OS 回收; Xvfb 由 StartXvfb 注册的 atexit 回调清理
    // (PDEATHSIG 在多线程下会误杀, 已弃用, 见 StartXvfb)。
    static OfficeRuntime* inst = new OfficeRuntime();
    return *inst;
}

OfficeRuntime::~OfficeRuntime() {
    // Instance() 泄漏静态实例, 正常情况下不会被调用; 防御性清理。
    OfficeLog("[OfficeRuntime] ~OfficeRuntime (defensive; instance is intentionally leaked)");
    StopXvfb();
}

bool OfficeRuntime::Acquire(const OfficeRuntimeConfig& config) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (ref_count_ == 0) {
        config_ = config;
        CleanupOrphanSoffice(); // 开机清场: 残留孤儿 soffice (幂等, 每次首 Acquire)
        if (!StartXvfb()) {
            OfficeLogErr("[OfficeRuntime] Acquire: FAILED to start Xvfb, first session aborted");
            return false;
        }
        SetDisplayEnv(); // 引导前设置 DISPLAY (bootstrap 子进程继承)
    } else {
        // 已定屏: 后续配置必须一致 (或更小), 避免冲突
        if (config.max_docs > config_.max_docs ||
            config.max_doc_width > config_.max_doc_width ||
            config.max_doc_height > config_.max_doc_height) {
            OfficeLog("[OfficeRuntime] Acquire: reject config exceeds existing screen (%d docs x %dx%d), "
                  "requested %d docs x %dx%d",
                  config_.max_docs, config_.max_doc_width, config_.max_doc_height,
                  config.max_docs, config.max_doc_width, config.max_doc_height);
            return false;
        }
    }
    ref_count_++;
    OfficeLog("[OfficeRuntime] Acquire: ok, ref_count=%d, display=%s", ref_count_, display_.c_str());
    return true;
}

void OfficeRuntime::Release() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (ref_count_ <= 0) {
        // 配对错误 (多发 Release), 提示但不崩溃
        OfficeLogWarn("[OfficeRuntime] Release: warning ref_count already 0, ignored");
        return;
    }
    ref_count_--;
    OfficeLog("[OfficeRuntime] Release: ref_count=%d (kernel/Xvfb kept alive while process runs)", ref_count_);
    // 共享内核模式: Xvfb/内核进程级存活 (首个 Acquire 创建; 末个 Release 不
    // 销毁 —— 内核的 X 连接依赖 Xvfb 存活, 不能随 session 销毁; 进程退出时
    // 由 StartXvfb 注册的 atexit 回调 StopXvfb 清理)。
}

const std::string& OfficeRuntime::display() const {
    return display_;
}

void OfficeRuntime::SetDisplayEnv() const {
    if (!display_.empty()) {
        // 注意: 进程级副作用 (污染宿主 DISPLAY, 后续 XOpenDisplay 若不显式
        // 传参将连到虚拟屏)。曾疑按键事件失效由此而来, 已排查否定 (实际根因
        // 是媒体页 slideshow 事件链阻塞, 即经验 17/18 的媒体问题, 已修复)。
        setenv("DISPLAY", display_.c_str(), 1);
        OfficeLog("[OfficeRuntime] SetDisplayEnv: DISPLAY=%s (process-global side effect)", display_.c_str());
    }
}

bool OfficeRuntime::EnsureKernel(const std::string& user_installation) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (kernel_.is()) {
        // 已引导: 不同 profile 的请求暂不支持多内核并存, 忽略并复用 (日志提示)
        if (!user_installation.empty() && kernel_profile_ != user_installation) {
            OfficeLogWarn("[OfficeRuntime] EnsureKernel: existing kernel profile=\"%s\", requested \"%s\" "
                  "ignored (multi-kernel not yet supported)",
                  kernel_profile_.c_str(), user_installation.c_str());
        }
        OfficeLog("[OfficeRuntime] EnsureKernel: reuse existing kernel (same process)");
        return true;
    }
    // 播放内核默认用独立 profile (经验 27):
    //   1) 初始配置 = 部署模板 fresh copy (2026-08-18, templates/user, 见
    //      SeedKernelProfile; UI 三层控制: UNO API > 平台窗口 API > user 模板)
    //   2) 防御: 与任何默认 profile 的 soffice 调用 (外部转换/其他进程)
    //      彻底隔离 (不同 UserInstallation -> 不复用、不共享配置, 经验 22)
    // 路径统一 office_paths::xvfb_profile (2026-08-18 player/ 更名)
    std::string profile = user_installation;
    if (profile.empty()) {
        profile = office_paths::xvfb_profile();
    }
    kernel_profile_ = profile;
    SeedKernelProfile(profile);
    // BootstrapOffice 引导 (复制官方 cppu::bootstrap 逻辑, 支持独立 profile):
    // 进程内引导 LO 内核 (共享内核多文档模式的基础)。
    // 跨进程: LO 按 UserInstallation 复用已运行的内核 (共享内核机制, 与共享
    // 虚拟屏配套 —— 共享屏上渲染, 新进程的会话窗口同样落在共享屏;
    // 同 profile 的转发复用见 HANDOFF 经验 22)。
    // 跨进程并发引导/加载由 BootLock 的命名信号量串行化 (见 BootLock)。
    setenv("UNO_PATH", GetRuntimeDir().c_str(), 1);
    // Xvfb 恒无 GPU: 禁用 LO 的 OpenGL 路径。GL 转场 (libOGLTranslo) 在
    // Xvfb 下走 EGL->swrast 软件光栅化会崩 soffice.bin (实测: AI时代.pptx
    // 的 morph/fade 转场导致 slideshow start 即崩, 桥断开所有会话)。
    // supportsOpenGL() 首行即读此变量 (vcl/source/opengl/OpenGLHelper.cxx),
    // 禁用后转场退化为 CPU 渲染, 效果保留。与媒体 sink 固定 ximagesink 同
    // 一哲学 (部署环境固定, 不做动态决策)。bootstrap 子进程继承本设置;
    // 跨进程复用内核时由内核进程的 EnsureKernel 兜底。部署侧 (run.sh)
    // 也建议设置, 覆盖不经过本模块的启动路径。
    setenv("SAL_DISABLEGL", "1", 0); // 不覆盖宿主已有的显式设置
    // 媒体后端选择开关 (方案 A, HANDOFF 经验 30/34): LO 的 mediawindow_impl.cxx
    // (libavmedialo.so, 已本地改码+增量编译) 读此变量选择后端:
    //   ORT_MEDIA_BACKEND=ffplay -> com.sun.star.comp.avmedia.Manager_FFPlay
    //     (自治媒体后端: ffplay 嵌入引擎, 真实视频+音频, 经验 34)
    //   其他/空                  -> 编译期默认 (GStreamer, 上游行为)
    // 默认 ffplay (2026-08-14 切换: gstreamer 已不能满足需求 — Xvfb 下
    // 无音频设备时 gst 静默无声; ffplay 引擎全链路验证通过)。
    // 机制同 SAL_DISABLEGL: bootstrap 子进程继承 env 快照 (经验 24);
    // 不覆盖宿主显式设置 (宿主 export ORT_MEDIA_BACKEND=gstreamer 可回退)。
    setenv("ORT_MEDIA_BACKEND", "ffplay", 0);
    // 放映视图铺满窗口开关 (2026-08-24, bleed 缺陷根治): LO sd 补丁
    // (slideshowimpl.cxx, 同 gstplayer/mediawindow 的本地改码模式) 读此变量 —
    // 窗口化放映默认取 getClientRectangle() (永远保留状态栏布局槽 ~37px@100dpi),
    // 底部留未绘制带 (透显缺陷的"接收漏洞") 且幻灯片被纵向压扁 ~3.4%。=1 时
    // 放映视图铺满父窗口: 带消失、比例精确。机制同上: env 快照继承 (经验 24),
    // 不覆盖宿主 (宿主 export ORT_SLIDE_FILL_WINDOW=0 可回退)。
    // 见 doc/defect-impress-bleed-through.md。
    setenv("ORT_SLIDE_FILL_WINDOW", "1", 0);
    // 媒体 sink 修复实际走 LO 源码改动 + 组件替换部署 (HANDOFF 经验 18):
    // gstplayer.cxx 回退分支优先 ximagesink, 增量编译后替换 libavmediagst.so
    // —— 生效组件是 libavmediagst.so (不是 libavmedialo.so), **仅 gstreamer
    // 回退路径相关** (默认 ffplay 引擎不经过 gst)。LD_PRELOAD 劫持方案已弃用
    // (符号解析随机不可靠, 且需在宿主启动前设置, 勿在代码里设置)。
    // gstreamer 依赖检测 (仅回退路径需要): 默认后端是 ffplay (经验 30/34),
    // 不依赖 gst; 仅当宿主显式 export ORT_MEDIA_BACKEND=gstreamer 时才告警
    // (缺失时 LO 优雅降级: 媒体不播, 事件链不阻塞, 经验 28)
    {
        const char* backend = getenv("ORT_MEDIA_BACKEND");
        if (backend && strcmp(backend, "gstreamer") == 0) {
            std::string gst_detail;
            int gst = CheckGstDeps(nullptr, &gst_detail);
            if (gst == 0)
                OfficeLog("[OfficeRuntime] EnsureKernel: gstreamer (回退路径) deps OK");
            else
                OfficeLogWarn("[OfficeRuntime] EnsureKernel: WARNING gstreamer (回退路径) deps incomplete "
                          "(level=%d): %s - 媒体页将无媒体 (不卡死, 经验 28/34)",
                          gst, gst_detail.c_str());
        }
    }
    OfficeLog("[OfficeRuntime] EnsureKernel: bootstrapping LO kernel (UNO_PATH=%s, profile=\"%s\")...",
          GetRuntimeDir().c_str(), kernel_profile_.c_str());
    auto t0 = std::chrono::steady_clock::now();
    kernel_ = BootstrapOffice(kernel_profile_);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    if (!kernel_.is()) {
        OfficeLogErr("[OfficeRuntime] EnsureKernel: bootstrap FAILED (took %lld ms)", static_cast<long long>(ms));
        return false;
    }
    OfficeLog("[OfficeRuntime] EnsureKernel: kernel ready (took %lld ms)", static_cast<long long>(ms));
    // 诊断: ORT_DUMP_WINDOWS=1 时引导后自省窗口树 + 重叠检测 + 边缘像素 (排障/回归)
    if (getenv("ORT_DUMP_WINDOWS")) {
        DumpWindows();
        CheckWindowOverlap();
        DumpWindowEdges();
    }
    return true;
}

const css::uno::Reference<css::uno::XComponentContext>& OfficeRuntime::kernel() const {
    return kernel_;
}

OfficeRuntime::BootLock::BootLock(int timeout_ms)
    : timeout_ms_(timeout_ms > 0 ? timeout_ms : 60000) {
    Lock();
}

OfficeRuntime::BootLock::~BootLock() {
    Unlock();
}

void OfficeRuntime::BootLock::Lock() {
    // 进程内: so 单例 mutex, 跨 calclink/impresslink/writerlink 共享
    static std::mutex s_proc_mutex;
    mtx_ = &s_proc_mutex;
    mtx_->lock();
    // 跨进程: 命名信号量 (共享内核被多进程复用时, 引导+加载须全机器串行)
    sem_ = sem_open("/nova_office_boot", O_CREAT, 0644, 1);
    if (sem_ == SEM_FAILED) {
        OfficeLogErr("[OfficeRuntime] BootLock: sem_open(/nova_office_boot) FAILED: %s", strerror(errno));
        return;
    }
    // 有界等待: 持锁进程崩溃会遗留 0 值信号量, 等待超过 timeout_ms_ 视为
    // 残留, 强制恢复 (任何正常引导远小于默认 60s; 启发式, 总比永久卡死好)。
    // 日志仅在实质竞争时出现 (等待 >= 1s), 无竞争时静默。
    int max_waits = timeout_ms_ / 100;
    int waits = 0;
    bool contended = false;
    while (sem_trywait(sem_) != 0) {
        if (errno != EAGAIN) {
            OfficeLogWarn("[OfficeRuntime] BootLock: sem_trywait FAILED: %s", strerror(errno));
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (++waits * 100 >= 1000 && !contended) {
            OfficeLog("[OfficeRuntime] BootLock: waiting for cross-process semaphore (%d ms elapsed, "
                  "timeout %d ms, pid=%d)", waits * 100, timeout_ms_, (int)getpid());
            contended = true;
        }
        if (waits >= max_waits) {
            OfficeLogWarn("[OfficeRuntime] BootLock: semaphore stuck >%d ms, forcing release (pid=%d)",
                  timeout_ms_, (int)getpid());
            sem_post(sem_);
            waits = 0;
        }
    }
}

void OfficeRuntime::BootLock::Unlock() {
    if (sem_ != SEM_FAILED) {
        sem_post(sem_);
        sem_close(sem_);
        sem_ = SEM_FAILED;
    }
    if (mtx_) {
        mtx_->unlock();
        mtx_ = nullptr;
    }
}

namespace {
// 跨进程共享 slot 位图 (共享虚拟屏被多进程采用时, 窗口分区必须全机器一致;
// 各进程私有位图会重复分配同一 slot, 窗口重叠 -> XGetImage 被遮挡返回背景)。
// 布局: pid_t owner[MAX_SLOTS]; 0=空闲; 分配者记 PID, 崩溃后由后来者回收。
constexpr int kSlotMax = 16;
constexpr const char* kSlotShmName = "/nova_office_slots_v1";

struct SlotShm {
    pid_t owner[kSlotMax];
    int32_t magic; // 校验用
};

SlotShm* OpenSlotShm(int& fd_out) {
    int fd = shm_open(kSlotShmName, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        OfficeLogErr("[OfficeRuntime] slots: shm_open(%s) FAILED: %s", kSlotShmName, strerror(errno));
        return nullptr;
    }
    if (ftruncate(fd, static_cast<off_t>(sizeof(SlotShm))) != 0) {
        OfficeLogErr("[OfficeRuntime] slots: ftruncate FAILED: %s", strerror(errno));
        close(fd);
        return nullptr;
    }
    void* p = mmap(nullptr, sizeof(SlotShm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        OfficeLogErr("[OfficeRuntime] slots: mmap FAILED: %s", strerror(errno));
        close(fd);
        return nullptr;
    }
    SlotShm* shm = static_cast<SlotShm*>(p);
    if (shm->magic != 0x534C4F54) { // 首次创建
        memset(shm, 0, sizeof(SlotShm));
        shm->magic = 0x534C4F54;
    }
    fd_out = fd;
    return shm;
}
} // namespace

int OfficeRuntime::AllocSlot() {
    std::lock_guard<std::mutex> lk(mutex_);
    int fd = -1;
    SlotShm* shm = OpenSlotShm(fd);
    if (!shm)
        return -1;
    DEFER { munmap(shm, sizeof(SlotShm)); close(fd); };
    int slot = -1;
    bool reclaimed = false;
    if (flock(fd, LOCK_EX) == 0) {
        int max = static_cast<int>(config_.max_docs) > kSlotMax ? kSlotMax : config_.max_docs;
        for (int i = 0; i < max; i++) {
            if (shm->owner[i] == 0) {
                shm->owner[i] = getpid();
                slot = i;
                break;
            }
            if (!ProcAlive(shm->owner[i])) { // 所有者已死/僵尸 (崩溃/退出), 回收
                reclaimed = true;
                shm->owner[i] = getpid();
                slot = i;
                break;
            }
        }
        flock(fd, LOCK_UN);
    } else {
        OfficeLogWarn("[OfficeRuntime] AllocSlot: flock FAILED: %s", strerror(errno));
    }
    if (slot < 0) {
        OfficeLogWarn("[OfficeRuntime] AllocSlot: exhausted (max %d docs, %d slots)", config_.max_docs, kSlotMax);
    } else {
        OfficeLog("[OfficeRuntime] AllocSlot: got slot=%d (pid=%d%s)", slot, (int)getpid(),
              reclaimed ? ", reclaimed from dead owner" : "");
    }
    return slot;
}

void OfficeRuntime::FreeSlot(int slot) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (slot < 0 || slot >= kSlotMax) {
        OfficeLogWarn("[OfficeRuntime] FreeSlot: out of range slot=%d ignored", slot);
        return;
    }
    int fd = -1;
    SlotShm* shm = OpenSlotShm(fd);
    if (!shm)
        return;
    DEFER { munmap(shm, sizeof(SlotShm)); close(fd); };
    if (flock(fd, LOCK_EX) == 0) {
        if (shm->owner[slot] == getpid()) { // 只释放自己的 slot
            shm->owner[slot] = 0;
            OfficeLog("[OfficeRuntime] FreeSlot: slot=%d released (pid=%d)", slot, (int)getpid());
        } else {
            // 释放别人的 slot = 用法错误 (正常路径不可能出现), 提示
            OfficeLogWarn("[OfficeRuntime] FreeSlot: skip slot=%d (owner pid=%d != self pid=%d)",
                  slot, (int)shm->owner[slot], (int)getpid());
        }
        flock(fd, LOCK_UN);
    } else {
        OfficeLogWarn("[OfficeRuntime] FreeSlot: flock FAILED: %s", strerror(errno));
    }
}

// ---- 诊断: X 窗口自省 (只读) ----
namespace {

// 收集可见 LO 窗口: map_state==IsViewable 且尺寸 > 50 (滤掉 1x1 辅助窗)。
// 输出: 窗口 id / 根坐标位置 / 尺寸 / class / 父窗口 id。
// 坐标用"沿父链累加 XGetWindowAttributes 相对坐标"计算 (递归传累计偏移),
// 不用 XTranslateCoordinates —— 并发会话创建时窗口树被 LO 并发修改,
// 对失效窗口的 XTranslateCoordinates 失败不可检测, 实测 SIGSEGV。
struct WinInfo {
    Window wid = 0;
    Window parent = 0;
    int x = 0, y = 0;
    unsigned w = 0, h = 0;
    std::string cls;
};

void CollectWindows(Display* d, Window w, int ox, int oy, std::vector<WinInfo>& out) {
    Window rr, pp;
    Window* kids = nullptr;
    unsigned n = 0;
    if (!XQueryTree(d, w, &rr, &pp, &kids, &n))
        return;
    for (unsigned i = 0; i < n; i++) {
        XWindowAttributes a;
        // 窗口被并发销毁时 XGetWindowAttributes 返回 0 (可检测), 安全跳过
        if (XGetWindowAttributes(d, kids[i], &a) && a.map_state == IsViewable &&
            a.width > 50 && a.height > 50) {
            WinInfo wi;
            wi.wid = kids[i];
            wi.parent = w;
            wi.x = ox + a.x;
            wi.y = oy + a.y;
            wi.w = a.width;
            wi.h = a.height;
            XClassHint ch = {};
            if (XGetClassHint(d, kids[i], &ch)) {
                if (ch.res_class)
                    wi.cls = ch.res_class;
                if (ch.res_name)
                    XFree(ch.res_name);
                if (ch.res_class)
                    XFree(ch.res_class);
            }
            out.push_back(wi);
        }
        CollectWindows(d, kids[i], ox + a.x, oy + a.y, out);
    }
    if (kids)
        XFree(kids);
}

bool RectIntersect(const WinInfo& a, const WinInfo& b) {
    return a.x < b.x + (int)b.w && b.x < a.x + (int)a.w &&
           a.y < b.y + (int)b.h && b.y < a.y + (int)a.h;
}

}  // namespace

void OfficeRuntime::DumpWindows() {
    Display* d = XOpenDisplay(display_.c_str());
    if (!d) {
        OfficeLog("[OfficeRuntime] DumpWindows: XOpenDisplay(%s) failed", display_.c_str());
        return;
    }
    DEFER { XCloseDisplay(d); };
    OfficeLog("[OfficeRuntime] DumpWindows: screen=%dx%d", DisplayWidth(d, DefaultScreen(d)),
          DisplayHeight(d, DefaultScreen(d)));
    std::vector<WinInfo> wins;
    CollectWindows(d, DefaultRootWindow(d), 0, 0, wins);
    for (const auto& wi : wins)
        OfficeLogDbg("[OfficeRuntime] win 0x%lx parent=0x%lx pos=(%d,%d) size=%ux%u class='%s'",
              (unsigned long)wi.wid, (unsigned long)wi.parent,
              wi.x, wi.y, wi.w, wi.h, wi.cls.c_str());
}

// ---- 诊断: 窗口边缘像素分析 ----
// 排查"pptx 最外层像素黑/串流 xlsx": 采样每个 LO 窗口 4 边 + 中心的
// 像素颜色分布。判定: 边缘全黑 = LO 未绘制边框 (内容比窗口小);
// 边缘含相邻内容颜色 = 露底 (窗口未盖住底层, Xvfb 底层残留相邻画面)。
void OfficeRuntime::DumpWindowEdges() {
    Display* d = XOpenDisplay(display_.c_str());
    if (!d) {
        OfficeLog("[OfficeRuntime] DumpWindowEdges: XOpenDisplay(%s) failed", display_.c_str());
        return;
    }
    DEFER { XCloseDisplay(d); };
    std::vector<WinInfo> wins;
    CollectWindows(d, DefaultRootWindow(d), 0, 0, wins);
    for (const auto& wi : wins) {
        if (wi.cls.find("libreofficedev") == std::string::npos)
            continue;
        XImage* img = XGetImage(d, wi.wid, 0, 0, wi.w, wi.h, AllPlanes, ZPixmap);
        if (!img) {
            OfficeLog("[OfficeRuntime] edges: win 0x%lx(%s) XGetImage failed", (unsigned long)wi.wid, wi.cls.c_str());
            continue;
        }
        auto sample = [&](int x0, int y0, int x1, int y1, const char* tag) {
            long black = 0, white = 0, other = 0, total = 0;
            for (int y = y0; y <= y1; y++) {
                for (int x = x0; x <= x1; x++) {
                    if (x < 0 || y < 0 || x >= (int)wi.w || y >= (int)wi.h)
                        continue;
                    unsigned long px = XGetPixel(img, x, y);
                    int r = (px >> 16) & 0xff, g = (px >> 8) & 0xff, b = px & 0xff;
                    if (r < 8 && g < 8 && b < 8) black++;
                    else if (r > 250 && g > 250 && b > 250) white++;
                    else other++;
                    total++;
                }
            }
            if (total)
                OfficeLogDbg("[OfficeRuntime] edges[%s] 0x%lx(%s): black=%.1f%% white=%.1f%% other=%.1f%%",
                      tag, (unsigned long)wi.wid, wi.cls.c_str(),
                      100.0 * black / total, 100.0 * white / total, 100.0 * other / total);
        };
        const int ew = 2; // 边缘采样厚度 (px)
        sample(wi.w / 4, 0, wi.w * 3 / 4, ew - 1, "top");
        sample(wi.w / 4, wi.h - ew, wi.w * 3 / 4, wi.h - 1, "bottom");
        sample(0, wi.h / 4, ew - 1, wi.h * 3 / 4, "left");
        sample(wi.w - ew, wi.h / 4, wi.w - 1, wi.h * 3 / 4, "right");
        sample(wi.w / 2 - 10, wi.h / 2 - 10, wi.w / 2 + 10, wi.h / 2 + 10, "center");
        // 底部黑边分层 (1080p 窗口时代的实测值: LO 内容高 ~1057 vs 窗口
        // 1080, 底部 ~23px 黑边结构; 2160p 下数值不同, 仅结构参考)
        sample(wi.w / 4, wi.h - 8, wi.w * 3 / 4, wi.h - 1, "bottom-0to8");
        sample(wi.w / 4, wi.h - 18, wi.w * 3 / 4, wi.h - 9, "bottom-8to18");
        sample(wi.w / 4, wi.h - 28, wi.w * 3 / 4, wi.h - 19, "bottom-18to28");
        // 内容 bbox: 非黑像素包围盒 (判定黑边厚度; 步进 4px 采样加速)
        {
            int minx = (int)wi.w, miny = (int)wi.h, maxx = -1, maxy = -1;
            for (int y = 0; y < (int)wi.h; y += 4) {
                for (int x = 0; x < (int)wi.w; x += 4) {
                    unsigned long px = XGetPixel(img, x, y);
                    int r = (px >> 16) & 0xff, g = (px >> 8) & 0xff, b = px & 0xff;
                    if (r >= 8 || g >= 8 || b >= 8) { // 非黑
                        if (x < minx) minx = x;
                        if (x > maxx) maxx = x;
                        if (y < miny) miny = y;
                        if (y > maxy) maxy = y;
                    }
                }
            }
            if (maxx >= 0)
                OfficeLogDbg("[OfficeRuntime] edges[bbox] 0x%lx(%s): content=(%d,%d)-(%d,%d) size=%dx%d window=%ux%u",
                          (unsigned long)wi.wid, wi.cls.c_str(), minx, miny, maxx, maxy,
                          maxx - minx + 1, maxy - miny + 1, wi.w, wi.h);
            else
                OfficeLogDbg("[OfficeRuntime] edges[bbox] 0x%lx(%s): all black", (unsigned long)wi.wid, wi.cls.c_str());
        }
        XDestroyImage(img);
    }
}

// ---- gstreamer 依赖检测 ----
// 关键插件 (媒体解码链): coreelements/playback(uridecodebin)/autodetect/
// typefind/videoconvert/videoscale (base 包, 依赖清单见 HANDOFF 经验 28)。
int OfficeRuntime::CheckGstDeps(const char* plugin_dir_override, std::string* detail) {
    auto fail = [&](int level, const std::string& msg) {
        if (detail)
            *detail = msg;
        return level;
    };
    // 1. 核心库: dlopen 实测可加载 (不引入 gst 头/链接依赖)
    void* h = dlopen("libgstreamer-1.0.so.0", RTLD_NOW);
    if (!h)
        return fail(1, std::string("libgstreamer-1.0.so.0 not loadable"));
    dlclose(h);
    // 2. 插件目录
    const char* dir = plugin_dir_override
                          ? plugin_dir_override
                          : "/usr/lib/x86_64-linux-gnu/gstreamer-1.0";
    if (!std::filesystem::is_directory(dir))
        return fail(2, std::string("gst plugin dir missing: ") + dir);
    // 3. 关键插件文件
    static const char* kPlugins[] = {
        "libgstcoreelements.so", "libgstplayback.so", "libgstautodetect.so",
        "libgsttypefindfunctions.so", "libgstvideoconvert.so", "libgstvideoscale.so",
    };
    for (const char* p : kPlugins) {
        if (!std::filesystem::exists(std::string(dir) + "/" + p))
            return fail(3, std::string("gst plugin missing: ") + p);
    }
    if (detail)
        detail->clear();
    return 0;
}

int OfficeRuntime::CheckWindowOverlap() {
    Display* d = XOpenDisplay(display_.c_str());
    if (!d) {
        OfficeLog("[OfficeRuntime] CheckWindowOverlap: XOpenDisplay(%s) failed", display_.c_str());
        return -1;
    }
    DEFER { XCloseDisplay(d); };
    std::vector<WinInfo> wins;
    CollectWindows(d, DefaultRootWindow(d), 0, 0, wins);

    int overlaps = 0;
    for (size_t i = 0; i < wins.size(); i++) {
        for (size_t j = i + 1; j < wins.size(); j++) {
            // 父子窗口重叠视为正常 (LO 框架与视图结构), 跳过
            if (wins[i].parent == wins[j].wid || wins[j].parent == wins[i].wid)
                continue;
            if (!RectIntersect(wins[i], wins[j]))
                continue;
            OfficeLog("[OfficeRuntime] OVERLAP: 0x%lx(%s)@(%d,%d)%ux%u ∩ 0x%lx(%s)@(%d,%d)%ux%u",
                  (unsigned long)wins[i].wid, wins[i].cls.c_str(),
                  wins[i].x, wins[i].y, wins[i].w, wins[i].h,
                  (unsigned long)wins[j].wid, wins[j].cls.c_str(),
                  wins[j].x, wins[j].y, wins[j].w, wins[j].h);
            overlaps++;
        }
    }
    OfficeLogDbg("[OfficeRuntime] CheckWindowOverlap: %d visible LO windows, %d overlaps", (int)wins.size(), overlaps);
    return overlaps;
}

const OfficeRuntimeConfig& OfficeRuntime::config() const {
    return config_;
}

bool OfficeRuntime::StartXvfb() {
    // 大屏 + slot 分区: 每 session 窗口独立区域, 互不重叠。
    // 窗口从不被遮挡, XGetImage 始终返回真实内容 (规避 Xvfb backing 缺陷)。
    int screen_w = config_.max_docs * config_.max_doc_width;
    int screen_h = config_.max_doc_height;
    for (int n = kDisplayMin; n < kDisplayMax; n++) {
        display_ = ":" + std::to_string(n);
        std::string lock = "/tmp/.X" + std::to_string(n) + "-lock";
        if (access(lock.c_str(), F_OK) == 0) {
            // 锁文件存在: 验证服务器 PID 是否存活可服务 (SIGKILL 退出留残留锁;
            // 僵尸进程不算活 —— kill(pid,0) 对僵尸仍成功, 须查 state)
            pid_t lock_pid = 0;
            std::ifstream ifs(lock);
            ifs >> lock_pid;
            if (ProcAlive(lock_pid)) {
                // 共享虚拟屏 (机器级): 服务器活着 → 直接连接采用, 不再另起。
                // 与共享内核配套: 新进程的会话按 UserInstallation 复用已运行
                // 内核 (经验 22 转发机制), 文档渲染在共享屏上, 窗口与抓帧都
                // 在本显示 —— 若各自另起 Xvfb, 文档画在别人的屏上, 本进程
                // 抓帧全空。
                if (WaitForX(display_)) {
                    if (xvfb_pid_ != lock_pid) {
                        // 非本进程所有: 退出时不杀, 交由所有者清理
                        xvfb_pid_ = -1;
                        xvfb_owned_ = false;
                    }
                    // 锁内 PID 就是本进程起的 (引用计数归零后再次 Acquire):
                    // 保持自有标记, atexit 仍清理。
                    OfficeLog("[OfficeRuntime] StartXvfb: adopted shared Xvfb display=%s (owner pid=%d)",
                          display_.c_str(), (int)lock_pid);
                    return true;
                }
                OfficeLogWarn("[OfficeRuntime] StartXvfb: display=%s alive but unreachable, skip", display_.c_str());
                continue;
            }
            // 残留锁: 服务器已死, 清理后使用该号
            OfficeLogWarn("[OfficeRuntime] StartXvfb: stale lock on display=%s (dead pid=%d), removing",
                  display_.c_str(), (int)lock_pid);
            unlink(lock.c_str());
        }
        // 残留 socket 文件清理 (此刻该号已确认无活 server): unlink 的只是
        // 文件系统路径, 不影响垂死 server 已绑定的监听, 仅消除残留面
        // (经验 35)
        unlink(("/tmp/.X11-unix/X" + std::to_string(n)).c_str());
        pid_t pid = fork();
        if (pid == 0) {
            char scr[64];
            snprintf(scr, sizeof(scr), "%dx%dx24", screen_w, screen_h);
            execl("/usr/bin/Xvfb", "Xvfb", display_.c_str(), "-screen", "0",
                  scr, "-nolisten", "tcp", static_cast<char*>(nullptr));
            _exit(127);
        }
        // 等待 Xvfb 存活确认: 显示号竞争失败时 Xvfb 会较快退出
        bool alive = true;
        for (int i = 0; i < 5; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            int st = 0;
            if (waitpid(pid, &st, WNOHANG) == pid) {
                alive = false;
                break;
            }
        }
        if (!alive) {
            OfficeLogWarn("[OfficeRuntime] StartXvfb: display=%s lost race (Xvfb exited), trying next", display_.c_str());
            // 清 fork 失败的残局 (Xvfb 失败退出可能残留半写的 lock/socket;
            // unlink 路径不影响垂死同号 server 的已绑定监听, 经验 35)
            unlink(("/tmp/.X" + std::to_string(n) + "-lock").c_str());
            unlink(("/tmp/.X11-unix/X" + std::to_string(n)).c_str());
            display_.clear();
            continue; // 该号竞争失败, 试下一个
        }
        if (WaitForX(display_)) {
            xvfb_pid_ = pid;
            xvfb_owned_ = true; // 本进程启动, 退出时清理
            OfficeLog("[OfficeRuntime] StartXvfb: started display=%s pid=%d (screen %dx%d)",
                  display_.c_str(), (int)pid, screen_w, screen_h);
            // 进程正常退出时清理 Xvfb (PDEATHSIG 在多线程下会误杀, 不适用)
            static bool s_atexit_done = false;
            if (!s_atexit_done) {
                s_atexit_done = true;
                std::atexit([]() {
                    OfficeLog("[OfficeRuntime] atexit: stopping owned Xvfb");
                    OfficeRuntime::Instance().StopXvfb();
                });
            }
            return true;
        }
        OfficeLogWarn("[OfficeRuntime] StartXvfb: display=%s unreachable after spawn, killing pid=%d", display_.c_str(), (int)pid);
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        display_.clear();
    }
    OfficeLogErr("[OfficeRuntime] StartXvfb: FAILED - no free display in :%d-:%d", kDisplayMin, kDisplayMax - 1);
    return false;
}

void OfficeRuntime::StopXvfb() {
    // 仅清理本进程启动的 Xvfb; 采用共享屏 (xvfb_owned_ == false) 时
    // 交由所有者进程退出时清理。
    if (xvfb_owned_ && xvfb_pid_ > 0) {
        OfficeLog("[OfficeRuntime] StopXvfb: killing owned Xvfb pid=%d (display=%s)", (int)xvfb_pid_, display_.c_str());
        kill(xvfb_pid_, SIGKILL);
        waitpid(xvfb_pid_, nullptr, 0);
        xvfb_pid_ = -1;
    }
    // 自有屏被杀后主动清锁+socket (Xvfb 被 SIGKILL 不删自己的锁/socket 文件;
    // 残留 socket 一般无害但留下垂死误判面, 退出即干净; 经验 35)
    if (xvfb_owned_ && !display_.empty()) {
        std::string lock = "/tmp/.X" + display_.substr(1) + "-lock";
        std::string sock = "/tmp/.X11-unix/X" + display_.substr(1);
        OfficeLog("[OfficeRuntime] StopXvfb: removing lock %s", lock.c_str());
        unlink(lock.c_str());
        unlink(sock.c_str());
    }
    xvfb_pid_ = -1;
    display_.clear();
}
