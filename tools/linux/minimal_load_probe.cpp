// minimal_load_probe.cpp — 最简 URP 远程 loadComponentFromURL 验证
// 对比 soffice --convert-to (进程内) vs URP 远程调用。
// 用法: minimal_load_probe <file_path>
#include <runtime/runtime.h>

#include <cstdio>
#include <cstring>

#include <osl/file.hxx>
#include <com/sun/star/beans/PropertyValue.hpp>
#include <com/sun/star/frame/XComponentLoader.hpp>
#include <com/sun/star/frame/XDesktop.hpp>
#include <com/sun/star/lang/XMultiComponentFactory.hpp>

using css::uno::Reference;
using css::uno::UNO_QUERY;

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file_path>\n", argv[0]);
        return 2;
    }
    const char* path = argv[1];

    // 1. EnsureKernel (URP bridge to soffice.bin)
    OfficeRuntimeConfig cfg;
    cfg.max_docs = 1;
    if (!OfficeRuntime::Instance().Acquire(cfg)) {
        fprintf(stderr, "[MIN] Acquire FAILED\n");
        return 1;
    }
    if (!OfficeRuntime::Instance().EnsureKernel()) {
        fprintf(stderr, "[MIN] EnsureKernel FAILED\n");
        return 1;
    }
    auto ctx = OfficeRuntime::Instance().kernel();
    fprintf(stderr, "[MIN] kernel ready\n");

    // 2. Desktop + loader (URP remote call)
    auto sm = ctx->getServiceManager();
    Reference<css::frame::XDesktop> desktop(
        sm->createInstanceWithContext("com.sun.star.frame.Desktop", ctx), UNO_QUERY);
    if (!desktop.is()) {
        fprintf(stderr, "[MIN] Desktop createInstance FAILED\n");
        return 1;
    }
    Reference<css::frame::XComponentLoader> loader(desktop, UNO_QUERY);
    fprintf(stderr, "[MIN] loader OK\n");

    // 3. loadComponentFromURL (THE remote call under test)
    rtl::OUString docUrl;
    osl::FileBase::getFileURLFromSystemPath(
        rtl::OUString::createFromAscii(path), docUrl);
    fprintf(stderr, "[MIN] loadComponentFromURL url=%s ...\n",
            rtl::OUStringToOString(docUrl, RTL_TEXTENCODING_UTF8).getStr());

    css::uno::Sequence<css::beans::PropertyValue> props(1);
    props[0].Name = "Hidden";
    props[0].Value <<= true;

    try {
        auto comp = loader->loadComponentFromURL(docUrl, "_blank", 0, props);
        fprintf(stderr, "[MIN] loadComponentFromURL: %s\n",
                comp.is() ? "OK" : "NULL (no exception, empty result)");
        if (comp.is()) {
            comp->dispose();
            fprintf(stderr, "[MIN] disposed OK\n");
        }
    } catch (const css::uno::Exception& e) {
        fprintf(stderr, "[MIN] loadComponentFromURL EXC: %s\n",
                rtl::OUStringToOString(e.Message, RTL_TEXTENCODING_UTF8).getStr());
    } catch (...) {
        fprintf(stderr, "[MIN] loadComponentFromURL unknown exception\n");
    }

    fprintf(stderr, "[MIN] done\n");
    return 0;
}
