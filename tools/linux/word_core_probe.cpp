// word_core_probe.cpp — NovaOfficeCore 的 Word LibreOffice 模式分发验证:
// dlopen libNovaOfficeCore.so -> NWordCreateInstance(WORD_PLAY_MODE_ANIMATION_
// LIBREOFFICE) -> 帧回调/翻页/页数 -> NWordReleaseInstance。
// 验证: mode 分发 -> LibreOfficeWriterManager -> dlopen writerlink -> 会话创建。
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <unistd.h>
#include <locale.h>

// pfunUpdateCaptureCallback (8 参, 与 CaptureHookCommon.h 一致)
typedef void (*pfunUpdateCaptureCallback)(const uint8_t* data, int32_t width, int32_t height,
                                          int32_t row_pitch, int32_t size, int32_t format,
                                          int64_t luid, void* opaque);
typedef bool (*pfnNWordCreateInstance)(const uint8_t* guid, const uint8_t* path, const uint8_t* password,
                                       pfunUpdateCaptureCallback callback, void* opaque,
                                       int mode, uint64_t flag);
typedef bool (*pfnNWordReleaseInstance)(const uint8_t* guid);
typedef bool (*pfnNWordStartInstance)(const uint8_t* guid);
typedef bool (*pfnNWordStopInstance)(const uint8_t* guid);
typedef bool (*pfnNWordNextPage)(const uint8_t* guid);
typedef bool (*pfnNWordGoToPage)(const uint8_t* guid, int32_t page);
typedef int32_t (*pfnNWordGetPageCount)(const uint8_t* guid);
typedef int32_t (*pfnNWordGetCurrentPage)(const uint8_t* guid);

#define WORD_PLAY_MODE_ANIMATION_LIBREOFFICE 3

static int s_frames = 0;
static int s_w = 0, s_h = 0;
static std::vector<uint8_t> s_last;
static void OnFrame(const uint8_t* data, int32_t width, int32_t height, int32_t rp,
                    int32_t size, int32_t format, int64_t luid, void* opaque) {
    (void)rp; (void)format; (void)luid; (void)opaque;
    s_frames++;
    s_w = width; s_h = height;
    s_last.assign(data, data + size);
}

static int NonWhitePct() {
    if (s_last.empty()) return -1;
    int nw = 0, total = (int)s_last.size() / 4;
    for (size_t i = 0; i < s_last.size(); i += 4) {
        uint8_t b = s_last[i], g = s_last[i + 1], r = s_last[i + 2];
        if (!(r > 245 && g > 245 && b > 245)) nw++;
    }
    return total ? nw * 100 / total : -1;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <doc>\n", argv[0]);
        return 2;
    }
    setenv("LANG", "zh_CN.UTF-8", 0);
    setlocale(LC_ALL, ""); // 激活 locale (mbstowcs 类路径转换依赖)
    const char* guid = "word_core_probe";

    // demo 同款加载 (NovaLoadLibrary = dlopen RTLD_LAZY): 未定义符号
    // (ExcelCopyManagerC1, Linux 不编译 if(MSVC) 源) 延迟解析, 不调用即不崩
    void* h = dlopen("libNovaOfficeCore.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "dlopen libNovaOfficeCore.so failed: %s\n", dlerror()); return 1; }
    auto create = (pfnNWordCreateInstance)dlsym(h, "NWordCreateInstance");
    auto release = (pfnNWordReleaseInstance)dlsym(h, "NWordReleaseInstance");
    auto start = (pfnNWordStartInstance)dlsym(h, "NWordStartInstance");
    auto next = (pfnNWordNextPage)dlsym(h, "NWordNextPage");
    auto goto_p = (pfnNWordGoToPage)dlsym(h, "NWordGoToPage");
    auto pc = (pfnNWordGetPageCount)dlsym(h, "NWordGetPageCount");
    auto cur = (pfnNWordGetCurrentPage)dlsym(h, "NWordGetCurrentPage");
    if (!create || !release || !start || !pc) { fprintf(stderr, "dlsym failed\n"); return 1; }

    long long t0 = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count();
    bool ok = create((const uint8_t*)guid, (const uint8_t*)argv[1], nullptr, OnFrame, nullptr,
                     WORD_PLAY_MODE_ANIMATION_LIBREOFFICE, 0);
    long long t_create = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch()).count() - t0;
    printf("Create(mode=ANIMATION_LIBREOFFICE): %s (%lld ms), pages=%d\n",
           ok ? "OK" : "FAIL", t_create, ok ? pc((const uint8_t*)guid) : -1);
    if (!ok) return 1;

    start((const uint8_t*)guid);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    printf("started: frames=%d %dx%d nonwhite=%d%%\n", s_frames, s_w, s_h, NonWhitePct());

    for (int i = 0; i < 3; i++) {
        next((const uint8_t*)guid);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        printf("next %d: page=%d/%d nonwhite=%d%%\n", i + 1,
               cur((const uint8_t*)guid), pc((const uint8_t*)guid), NonWhitePct());
    }
    release((const uint8_t*)guid);
    dlclose(h);
    printf("done\n");
    return 0;
}
