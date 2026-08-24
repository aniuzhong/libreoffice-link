// lockfile_probe.cpp — 验证 `.~lock.<name>#` 锁文件是否导致 loadComponentFromURL 静默失败
//
// 依据 LO 源码 (sfx2/source/doc/docfile.cxx LockOrigFileOnDemand + svl 的
// DocumentLockFile/lockfilecommon.cxx):
//   锁文件名  = <doc目录>/.~lock.<docbasename>#
//   锁内容    = OOOUSERNAME,SYSUSERNAME,LOCALHOST,EDITTIME,USERURL;   (按 LockFileComponent 枚举序)
//   own 判定  = 读锁的 SYSUSERNAME==本进程 && LOCALHOST==本机 && USERURL==本 profile
//               (命中 = 视作崩溃遗留 "own lock" → 直接放行继续打开)
//   非 own 时 headless/Hidden 加载 → eResult=FailedLockFile
//
// 用法:
//   ORT_HOME=<profile基目录> lockfile_probe <doc绝对路径>
// 依次测试: baseline(清锁) -> own(同 profile 锁) -> foreign_profile(异 profile 锁)
//          -> foreign_host(异主机名锁) -> report(缺陷报告样例格式)
//          -> repo(从源码仓库 tools/data/locks 装机备份锁, 引用 LOCKS_DATA_DIR)
// 每阶段打印 loadComponentFromURL 结果: OK / NULL / EXC(<msg>)
// 阶段可见 readOnly 变体: no RO 复现缺陷 (静默 null), RO=1 验证修复。
//
// 注: LOCKS_DATA_DIR 由 tools/CMakeLists.txt 通过编译宏注入 (指向
//     源码仓库 tools/data/locks), 兜底 /nonexistent。
#include <runtime/runtime.h>
#include <base/office_paths.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>
#include <filesystem>

#include <unistd.h>
#include <pwd.h>

#include <osl/file.hxx>
#include <rtl/string.hxx>
#include <com/sun/star/beans/PropertyValue.hpp>
#include <com/sun/star/frame/XComponentLoader.hpp>
#include <com/sun/star/frame/XDesktop.hpp>
#include <com/sun/star/lang/XComponent.hpp>
#include <com/sun/star/lang/XMultiComponentFactory.hpp>

#ifndef LOCKS_DATA_DIR
#define LOCKS_DATA_DIR "/nonexistent"
#endif

using css::uno::Reference;
using css::uno::UNO_QUERY;

static std::string s_docPath;
static std::string s_lockPath;
static std::string s_sysUser;   // SYSUSERNAME
static std::string s_host;      // LOCALHOST (短名)
static std::string s_profile;   // "file://<ORT_HOME>/xvfb" (本进程 USERURL)

// ---- 计算 .~lock.<basename># 路径 ----
static void InitPaths() {
    namespace fs = std::filesystem;
    fs::path p(s_docPath);
    s_lockPath = (p.parent_path() / (".~lock." + p.filename().string() + "#")).string();
}

// ---- 写锁文件 (内容 = 给定 LockFileEntry 五字段, 逗号分隔, ; 结尾, 无换行) ----
static void WriteLock(const std::string& ooouser, const std::string& sys,
                      const std::string& host, const std::string& edittime,
                      const std::string& userurl) {
    std::string content;
    content += ooouser.empty() ? "" : ooouser;
    content += "," + (sys.empty()? "" : sys);
    content += "," + (host.empty()? "" : host);
    content += "," + (edittime.empty()? "" : edittime);
    content += "," + (userurl.empty()? "" : userurl);
    content += ";";
    FILE* f = fopen(s_lockPath.c_str(), "w");
    if (!f) { fprintf(stderr, "  [lock] fopen fail: %s\n", s_lockPath.c_str()); return; }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    fprintf(stderr, "  [lock] wrote %s => %s\n", s_lockPath.c_str(), content.c_str());
}

static void RemoveLock() {
    if (!s_lockPath.empty()) { ::remove(s_lockPath.c_str()); }
}

// ---- 从仓库备份锁目录 (tools/data/locks) 装机与文档同名的真实锁 ----
static std::string InstallRepoLock(const std::string& doc) {
    namespace fs = std::filesystem;
    fs::path p(doc);
    std::string lockName = ".~lock." + p.filename().string() + "#";
    fs::path src = fs::path(LOCKS_DATA_DIR) / lockName;
    std::error_code ec;
    if (!fs::exists(src, ec) || ec) {
        fprintf(stderr, "[repos] no repo lock for '%s' (LOCKS_DATA_DIR=%s)\n",
                p.filename().string().c_str(), LOCKS_DATA_DIR);
        return std::string();
    }
    fs::path dst = p.parent_path() / lockName;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fprintf(stderr, "[repos] copy fail: %s\n", ec.message().c_str());
        return std::string();
    }
    fprintf(stderr, "[repos] installed repo lock -> %s\n", dst.string().c_str());
    return dst.string();
}

// ---- 加载并报告结果 (readOnly 控制是否传 ReadOnly 描述符) ----
static void DoLoad(const char* tag, bool readOnly,
                   Reference<css::frame::XDesktop>& desktop,
                   Reference<css::frame::XComponentLoader>& loader) {
    // 正确 UTF-8 解码中文路径 (createFromAscii 会让非 ASCII 字节错乱 → type detection failed)
    rtl::OString in(s_docPath.c_str(), static_cast<sal_Int32>(s_docPath.size()));
    rtl::OUString sysPath = rtl::OStringToOUString(in, RTL_TEXTENCODING_UTF8);
    rtl::OUString docUrl;
    osl::FileBase::getFileURLFromSystemPath(sysPath, docUrl);

    css::uno::Sequence<css::beans::PropertyValue> props(readOnly ? 2 : 1);
    props[0].Name = "Hidden";
    props[0].Value <<= true;
    if (readOnly) {
        props[1].Name = "ReadOnly";
        props[1].Value <<= true;
    }

    std::string result;
    Reference<css::lang::XComponent> comp;
    try {
        comp = loader->loadComponentFromURL(docUrl, "_blank", 0, props);
        result = comp.is() ? "OK" : "NULL (no exception, empty result)";
    } catch (const css::uno::Exception& e) {
        result = "EXC: " + std::string(rtl::OUStringToOString(
            e.Message, RTL_TEXTENCODING_UTF8).getStr());
    } catch (...) {
        result = "EXC: unknown";
    }
    fprintf(stderr, "[load] %-24s (RO=%d) => %s\n", tag, readOnly ? 1 : 0, result.c_str());

    if (comp.is()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        try { comp->dispose(); fprintf(stderr, "[load] %-24s disposed OK\n", tag); }
        catch (...) { fprintf(stderr, "[load] %-24s dispose EXC\n", tag); }
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: ORT_HOME=<base> lockfile_probe <doc绝对路径>\n");
        return 2;
    }
    s_docPath = argv[1];
    InitPaths();

    // 本进程 own 值 (与 LO osl::Security::getUserName / SocketAddr::getLocalHostname /
    // locateUserInstallation(-env:UserInstallation=file://<ORT_HOME>/xvfb) 对齐)
    struct passwd* pw = getpwuid(getuid());
    s_sysUser = pw ? pw->pw_name : "unknown";
    char hn[256] = {0};
    gethostname(hn, sizeof(hn));
    s_host = hn;
    if (s_host.empty()) s_host = "unknown";
    s_profile = "file://" + office_paths::home() + "/xvfb";
    fprintf(stderr, "own: SYS=%s HOST=%s USERURL=%s\n",
            s_sysUser.c_str(), s_host.c_str(), s_profile.c_str());
    fprintf(stderr, "lock file: %s\n", s_lockPath.c_str());
    fprintf(stderr, "repo locks dir: %s\n", LOCKS_DATA_DIR);

    // 1. EnsureKernel (URP bridge to soffice.bin)
    OfficeRuntimeConfig cfg;
    cfg.max_docs = 1;
    if (!OfficeRuntime::Instance().Acquire(cfg)) {
        fprintf(stderr, "[main] Acquire FAILED\n"); return 1;
    }
    if (!OfficeRuntime::Instance().EnsureKernel()) {
        fprintf(stderr, "[main] EnsureKernel FAILED\n"); return 1;
    }
    auto ctx = OfficeRuntime::Instance().kernel();
    fprintf(stderr, "[main] kernel ready\n");

    auto sm = ctx->getServiceManager();
    Reference<css::frame::XDesktop> desktop(
        sm->createInstanceWithContext("com.sun.star.frame.Desktop", ctx), UNO_QUERY);
    Reference<css::frame::XComponentLoader> loader(desktop, UNO_QUERY);
    if (!loader.is()) { fprintf(stderr, "[main] loader FAILED\n"); return 1; }

    // A) 基线: 无锁
    RemoveLock();
    DoLoad("baseline(no lock)", false, desktop, loader);

    // B) own: 同 sys+host+profile → 应为 own-crash-leftover 放行
    RemoveLock();
    WriteLock("", s_sysUser, s_host, "01.01.2000 00:00", s_profile);
    DoLoad("own(same profile)", false, desktop, loader);
    RemoveLock();

    // C) foreign_profile: 异 profile → 应失败
    RemoveLock();
    WriteLock("", s_sysUser, s_host, "01.01.2000 00:00",
              "file:///tmp/ort_OTHERHOST/xvfb");
    DoLoad("foreign(profile)", false, desktop, loader);
    RemoveLock();

    // D) foreign_host: 异主机名 → 应失败
    RemoveLock();
    WriteLock("", s_sysUser, s_host + "X", "01.01.2000 00:00", s_profile);
    DoLoad("foreign(host)", false, desktop, loader);
    RemoveLock();

    // E) report 样例格式: 字段序 = OOO,SYS,LOCALHOST,EDITTIME,USERURL
    //    (报告把第1/2/3/5 字段误解成 用户名/主机名/日期/profile)
    //    这里 HOST=本机 Z2, SYS=hido, USERURL 指向 /tmp/ort_xxx → 异 profile
    RemoveLock();
    WriteLock("", s_sysUser, s_host, "21.08.2026 15:36",
              "file:///tmp/ort_xxx/xvfb");
    DoLoad("report(sample)", false, desktop, loader);
    RemoveLock();

    // F) 真实备份锁: 从仓库 tools/data/locks 装机 (引用 LOCKS_DATA_DIR),
    //    no RO 复现缺陷 (机器上残留的生产 profile 锁为 foreign → 静默 null),
    //    RO=1 验证修复 (ReadOnly 忽略锁)。
    RemoveLock();
    if (!InstallRepoLock(s_docPath).empty()) {
        DoLoad("repo backup", false, desktop, loader);
        DoLoad("repo backup RO", true, desktop, loader);
        RemoveLock();
    } else {
        fprintf(stderr, "[main] phase F skipped (doc 无对应仓库锁)\n");
    }

    fprintf(stderr, "[main] done\n");
    return 0;
}