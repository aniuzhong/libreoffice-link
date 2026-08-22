// pdf_render_probe.cpp — writer 两方案可行性探针 (经验 38 规划)
// 方案 A (自治 PDF): docx → PDF (同内核 storeToURL, 计时) → PDF 导入 Draw
//   (draw_pdf_Import) → XSlideRenderer::createPreview → XBitmap::getDIB (逐页计时)
// 方案 B (直接渲染): docx → XRenderable → getRendererCount/getRenderer → render
//   (记录可达性; 预期: 导出器基础设施, render 的 xOptions 是导出选项, 无位图目标)
// 用法: pdf_render_probe <doc> [A|B|both] [输出分辨率宽=1920]
// 前置: office_runtime 无其他使用者; 素材如 ~/文档/戴奥良-简历.docx
#include <runtime/runtime.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <com/sun/star/beans/PropertyValue.hpp>
#include <com/sun/star/awt/Size.hpp>
#include <com/sun/star/awt/XBitmap.hpp>
#include <com/sun/star/drawing/XDrawPage.hpp>
#include <com/sun/star/drawing/XDrawPages.hpp>
#include <com/sun/star/drawing/XDrawPagesSupplier.hpp>
#include <com/sun/star/drawing/XSlideRenderer.hpp>
#include <com/sun/star/frame/XComponentLoader.hpp>
#include <com/sun/star/frame/XDesktop.hpp>
#include <com/sun/star/frame/XModel.hpp>
#include <com/sun/star/lang/XComponent.hpp>
#include <com/sun/star/lang/XMultiComponentFactory.hpp>
#include <com/sun/star/util/XCloseable.hpp>
#include <com/sun/star/view/XRenderable.hpp>
#include <com/sun/star/frame/XStorable.hpp>
#include <osl/file.hxx>

using css::uno::Reference;
using css::uno::UNO_QUERY;
using css::uno::Any;

static std::string u2s(const rtl::OUString& s) {
    rtl::OString o = rtl::OUStringToOString(s, RTL_TEXTENCODING_UTF8);
    return std::string(o.getStr(), o.getLength());
}

static long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 打开文档 (Hidden)
static Reference<css::lang::XComponent> OpenDoc(
    const Reference<css::frame::XComponentLoader>& loader, const std::string& path,
    const char* filter, std::string* err) {
    // UTF-8 正确转换 (createFromAscii 会把中文按 Latin1 损坏 -> type detection failed)
    rtl::OUString sysPath = rtl::OStringToOUString(
        rtl::OString(path.c_str(), (sal_Int32)path.size()), RTL_TEXTENCODING_UTF8);
    rtl::OUString url;
    if (osl::FileBase::getFileURLFromSystemPath(sysPath, url) != osl::FileBase::E_None) {
        *err = "path->url failed";
        return nullptr;
    }
    css::uno::Sequence<css::beans::PropertyValue> props(2);
    props[0].Name = "Hidden";
    props[0].Value <<= true;
    if (filter) {
        props[1].Name = "FilterName";
        props[1].Value <<= rtl::OUString::createFromAscii(filter);
    }
    try {
        return loader->loadComponentFromURL(url, "_blank", 0, props);
    } catch (const css::uno::Exception& e) {
        *err = "load failed: " + u2s(e.Message);
        return nullptr;
    }
}

// 统计 DIB 像素非白占比 (从 BITMAPINFOHEADER 解析尺寸/位深, 不依赖 getSize)
static int NonWhitePct(const css::uno::Sequence<sal_Int8>& dib, int* ow, int* oh) {
    if (dib.getLength() < 40)
        return -1;
    const uint8_t* d = reinterpret_cast<const uint8_t*>(dib.getConstArray());
    // BMP 文件格式: 'BM'(0-1) + 文件大小(2-5) + 保留 + 像素偏移(10-13) + BITMAPINFOHEADER
    //   (14-17 biSize, 18-21 biWidth, 22-25 biHeight, 26-27 planes, 28-29 biBitCount)
    if (d[0] != 'B' || d[1] != 'M')
        return -4;
    int px_off = *(const int32_t*)(d + 10);
    int w = *(const int32_t*)(d + 18);
    int h = *(const int32_t*)(d + 22);
    int bpp = *(const uint16_t*)(d + 28);
    if (w <= 0 || h == 0 || (bpp != 24 && bpp != 32))
        return -2;
    int ah = h < 0 ? -h : h;
    int row = ((w * bpp + 31) / 32) * 4; // 行对齐 4 字节
    const uint8_t* px = d + px_off;
    if (dib.getLength() < 40 + (size_t)row * ah)
        return -3;
    if (ow) *ow = w;
    if (oh) *oh = ah;
    int nbytes = bpp / 8;
    int nw = 0;
    for (int y = 0; y < ah; y++) {
        const uint8_t* r = px + (size_t)y * row;
        for (int x = 0; x < w; x++) {
            const uint8_t* p = r + (size_t)x * nbytes; // BGR(A)
            if (!(p[0] > 245 && p[1] > 245 && p[2] > 245))
                nw++;
        }
    }
    return w * ah ? nw * 100 / (w * ah) : -1;
}

// ---- 方案 A: 自治 PDF 链路 ----
static int RunPlanA(const Reference<css::lang::XMultiComponentFactory>& factory,
                    const Reference<css::uno::XComponentContext>& ctx,
                    const Reference<css::frame::XComponentLoader>& loader,
                    const std::string& doc, int out_w) {
    // 1. 加载 docx + 转 PDF (同内核, 计时)
    std::string err;
    Reference<css::lang::XComponent> wdoc = OpenDoc(loader, doc, nullptr, &err);
    if (!wdoc.is()) { fprintf(stderr, "[A] docx load: %s\n", err.c_str()); return 1; }
    std::string pdf = "/tmp/pdf_render_probe.pdf";
    long long t0 = NowMs();
    rtl::OUString pdfUrl;
    osl::FileBase::getFileURLFromSystemPath(rtl::OUString::createFromAscii(pdf.c_str()), pdfUrl);
    css::uno::Sequence<css::beans::PropertyValue> exp(1);
    exp[0].Name = "FilterName";
    exp[0].Value <<= rtl::OUString("writer_pdf_Export");
    try {
        Reference<css::frame::XStorable> st(wdoc, UNO_QUERY);
        st->storeToURL(pdfUrl, exp);
    } catch (const css::uno::Exception& e) {
        fprintf(stderr, "[A] pdf export: %s\n", u2s(e.Message).c_str());
        return 1;
    }
    long long t_pdf = NowMs() - t0;
    printf("[A] docx->PDF: %lld ms (-> %s)\n", t_pdf, pdf.c_str());
    Reference<css::util::XCloseable> cw(wdoc, UNO_QUERY);
    if (cw.is())
        cw->close(false);

    // 2. PDF 导入为 Draw (计时)
    t0 = NowMs();
    Reference<css::lang::XComponent> ddoc = OpenDoc(loader, pdf, "draw_pdf_Import", &err);
    if (!ddoc.is()) { fprintf(stderr, "[A] pdf import: %s\n", err.c_str()); return 1; }
    long long t_imp = NowMs() - t0;
    printf("[A] PDF->Draw import: %lld ms\n", t_imp);

    // 3. 逐页 createPreview -> XBitmap -> getDIB (计时/尺寸/内容)
    Reference<css::drawing::XDrawPagesSupplier> sup(ddoc, UNO_QUERY);
    if (!sup.is()) { fprintf(stderr, "[A] not a draw doc\n"); return 1; }
    Reference<css::drawing::XDrawPages> pages = sup->getDrawPages();
    sal_Int32 n = pages->getCount();
    printf("[A] pages=%d\n", (int)n);

    Reference<css::drawing::XSlideRenderer> sr(
        factory->createInstanceWithContext(
            rtl::OUString("com.sun.star.drawing.SlideRenderer"), ctx),
        UNO_QUERY);
    if (!sr.is()) { fprintf(stderr, "[A] SlideRenderer not creatable\n"); return 1; }

    long long total = 0;
    for (sal_Int32 i = 0; i < n && i < 30; i++) {
        Reference<css::drawing::XDrawPage> page(pages->getByIndex(i), UNO_QUERY);
        t0 = NowMs();
        Reference<css::awt::XBitmap> bmp = sr->createPreview(page, css::awt::Size(out_w, 1080), 1);
        long long ms = NowMs() - t0;
        total += ms;
        if (!bmp.is()) { printf("[A] page %d: createPreview NULL (%lld ms)\n", (int)i, ms); continue; }
        css::awt::Size sz = bmp->getSize();
        css::uno::Sequence<sal_Int8> dib = bmp->getDIB();
        // 诊断: DIB 头部前 40 字节 (确认 header 结构)
        if (i == 0 && dib.getLength() >= 40) {
            printf("[A] dib head: ");
            const uint8_t* d = reinterpret_cast<const uint8_t*>(dib.getConstArray());
            for (int k = 0; k < 40; k++) printf("%02x ", d[k]);
            printf("\n");
        }
        int dw = 0, dh = 0;
        int nw = NonWhitePct(dib, &dw, &dh);
        printf("[A] page %d: %lld ms, getSize=%dx%d, dib=%d bytes, dib_header=%dx%d, nonwhite=%d%%\n",
               (int)i, ms, sz.Width, sz.Height, (int)dib.getLength(),
               dw, dh, nw);
    }
    printf("[A] 逐页渲染合计: %lld ms (pages=%d)\n", total, (int)n);
    return 0;
}

// ---- 方案 B: 直接渲染 (XRenderable) ----
static int RunPlanB(const Reference<css::frame::XComponentLoader>& loader,
                    const std::string& doc) {
    std::string err;
    Reference<css::lang::XComponent> wdoc = OpenDoc(loader, doc, nullptr, &err);
    if (!wdoc.is()) { fprintf(stderr, "[B] docx load: %s\n", err.c_str()); return 1; }
    Reference<css::view::XRenderable> rend(wdoc, UNO_QUERY);
    if (!rend.is()) { fprintf(stderr, "[B] 文档不支持 XRenderable\n"); return 1; }
    try {
        long n = rend->getRendererCount(css::uno::Any(), {});
        printf("[B] renderer count=%d\n", (int)n);
        for (long i = 0; i < n && i < 5; i++) {
            auto props = rend->getRenderer(i, css::uno::Any(), {});
            printf("[B] renderer %d: %d props\n", (int)i, (int)props.getLength());
            for (const auto& p : props)
                printf("[B]   %s\n", u2s(p.Name).c_str());
            try {
                rend->render(i, css::uno::Any(), {});
                printf("[B]   render OK (无目标, 未验证输出)\n");
            } catch (const css::uno::Exception& e) {
                printf("[B]   render 异常: %s\n", u2s(e.Message).c_str());
            }
        }
    } catch (const css::uno::Exception& e) {
        printf("[B] XRenderable 调用异常: %s\n", u2s(e.Message).c_str());
    }
    return 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <doc> [A|B|both] [out_width=1920]\n", argv[0]);
        return 2;
    }
    std::string doc = argv[1];
    std::string plan = (argc >= 3) ? argv[2] : "both";
    int out_w = (argc >= 4) ? atoi(argv[3]) : 1920;

    setenv("LANG", "zh_CN.UTF-8", 0); // LO type detection 依赖 locale (经验 25 陷阱)
    OfficeRuntimeConfig cfg;
    cfg.max_docs = 8;
    cfg.max_doc_width = 3840;
    cfg.max_doc_height = 2160;
    if (!OfficeRuntime::Instance().Acquire(cfg)) { fprintf(stderr, "Acquire failed\n"); return 1; }
    if (!OfficeRuntime::Instance().EnsureKernel()) { fprintf(stderr, "EnsureKernel failed\n"); return 1; }
    auto ctx = OfficeRuntime::Instance().kernel();
    auto factory = ctx->getServiceManager();
    Reference<css::frame::XDesktop> desktop(
        factory->createInstanceWithContext("com.sun.star.frame.Desktop", ctx), UNO_QUERY);
    Reference<css::frame::XComponentLoader> loader(desktop, UNO_QUERY);

    if (plan == "A" || plan == "both") {
        printf("===== 方案 A (自治 PDF) =====\n");
        RunPlanA(factory, ctx, loader, doc, out_w);
    }
    if (plan == "B" || plan == "both") {
        printf("===== 方案 B (直接渲染 XRenderable) =====\n");
        RunPlanB(loader, doc);
    }
    printf("done\n");
    return 0;
}
