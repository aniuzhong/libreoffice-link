// lockfile_fix_probe.cpp — 最小验证: ReadOnly 加载描述符可忽略残留锁文件
// 场景: 文档同目录残留一个 "foreign" 锁 (`~lock.<name>#`, 异 profile/host),
//       此前 loadComponentFromURL(Hidden only) 静默返回 null (缺陷)。
// 探针优先从仓库备份锁目录 tools/data/locks (编译宏 LOCKS_DATA_DIR) 装机
// 与文档同名的真实锁, 若存在则测两态; 否则用文档同目录已有锁。
//   锁来源: tools/data/locks/.~lock.<basename>#   (引用 LOCKS_DATA_DIR)
// 结论判定: 相同锁下
//   [A] Hidden only              -> 预期 NULL  (复现缺陷)
//   [B] Hidden + ReadOnly=true   -> 预期 OK    (修复见 effect)
// 与 session.cpp 修复对照: 三链会话 Create 现传 ReadOnly=true, 即生产路径恒为 [B]。
// 用法: ORT_HOME=<base> lockfile_fix_probe <doc绝对路径>
#include <runtime/runtime.h>
#include <base/office_paths.h>
#include <rtl/string.hxx>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>
#include <filesystem>

#include <osl/file.hxx>
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

// ---- 从仓库备份锁目录 (tools/data/locks) 装机与文档同名的真实锁 ----
static void InstallRepoLockIfAny(const std::string& doc) {
    namespace fs = std::filesystem;
    fs::path p(doc);
    std::string lockName = ".~lock." + p.filename().string() + "#";
    fs::path src = fs::path(LOCKS_DATA_DIR) / lockName;
    std::error_code ec;
    if (!fs::exists(src, ec) || ec) {
        fprintf(stderr, "[locks] no repo lock for '%s' (LOCKS_DATA_DIR=%s); use doc-dir lock if present\n",
                p.filename().string().c_str(), LOCKS_DATA_DIR);
        return;
    }
    fs::path dst = p.parent_path() / lockName;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fprintf(stderr, "[locks] copy fail: %s\n", ec.message().c_str());
        return;
    }
    fprintf(stderr, "[locks] installed repo lock -> %s\n", dst.string().c_str());
}

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
        result = comp.is() ? "OK" : "NULL";
    } catch (const css::uno::Exception& e) {
        result = "EXC: " + std::string(rtl::OUStringToOString(
            e.Message, RTL_TEXTENCODING_UTF8).getStr());
    } catch (...) {
        result = "EXC: unknown";
    }
    fprintf(stderr, "[load] %-22s (ReadOnly=%d) => %s\n", tag, readOnly ? 1 : 0, result.c_str());
    if (comp.is()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        try { comp->dispose(); fprintf(stderr, "[load] %-22s disposed\n", tag); }
        catch (...) { fprintf(stderr, "[load] %-22s dispose EXC\n", tag); }
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: ORT_HOME=<base> lockfile_fix_probe <doc绝对路径>\n");
        return 2;
    }
    s_docPath = argv[1];
    fprintf(stderr, "home(%s) doc=%s repo_locks=%s\n",
            office_paths::home().c_str(), s_docPath.c_str(), LOCKS_DATA_DIR);

    OfficeRuntimeConfig cfg;
    cfg.max_docs = 1;
    if (!OfficeRuntime::Instance().Acquire(cfg)) { fprintf(stderr, "Acquire FAILED\n"); return 1; }
    if (!OfficeRuntime::Instance().EnsureKernel()) { fprintf(stderr, "EnsureKernel FAILED\n"); return 1; }
    auto ctx = OfficeRuntime::Instance().kernel();
    fprintf(stderr, "kernel ready\n");

    auto sm = ctx->getServiceManager();
    Reference<css::frame::XDesktop> desktop(
        sm->createInstanceWithContext("com.sun.star.frame.Desktop", ctx), UNO_QUERY);
    Reference<css::frame::XComponentLoader> loader(desktop, UNO_QUERY);
    if (!loader.is()) { fprintf(stderr, "loader FAILED\n"); return 1; }

    // 优先从仓库备份锁目录装机真实锁, 测 no-RO 与 RO 两态
    InstallRepoLockIfAny(s_docPath);
    DoLoad("stale-lock, no-RO", false, desktop, loader);
    DoLoad("stale-lock, RO=1", true, desktop, loader);

    fprintf(stderr, "done\n");
    return 0;
}