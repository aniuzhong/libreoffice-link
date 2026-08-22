// 一次性: impress 会话抓帧存盘 (肉眼验证 UI 残留)
#include <abi/abi.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
static std::vector<uint8_t> s_last; static int s_w=0, s_h=0;
static void OnFrame(const uint8_t* d, int32_t w, int32_t h, int32_t rp, int32_t sz, int32_t f, void* o){
    (void)rp;(void)f;(void)o; s_w=w; s_h=h; s_last.assign(d, d+sz);
}
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <functional>
static void save_bmp(const char* path){
    int w=s_w, h=s_h; if(!w) return;
    int row=((w*3+3)/4)*4;
    std::vector<uint8_t> bmp(54+row*h);
    memcpy(&bmp[0], "BM", 2);
    *(int*)&bmp[2]=54+row*h; *(int*)&bmp[10]=54; *(int*)&bmp[14]=40;
    *(int*)&bmp[18]=w; *(int*)&bmp[22]=h; *(short*)&bmp[26]=1; *(short*)&bmp[28]=24;
    for(int y=0;y<h;y++){ const uint8_t* src=&s_last[(h-1-y)*w*4]; uint8_t* dst=&bmp[54+y*row];
        for(int x=0;x<w;x++){ dst[x*3]=src[x*4]; dst[x*3+1]=src[x*4+1]; dst[x*3+2]=src[x*4+2]; } }
    FILE* f=fopen(path,"wb"); fwrite(bmp.data(),1,bmp.size(),f); fclose(f);
    printf("saved %s (%dx%d)\n", path, w, h);
}
int main(int argc, char** argv){
    void* s = ImpressSessionCreate(argv[1], "", "iframe", OnFrame, nullptr, 1920, 1080);
    if(!s){ printf("create fail\n"); return 1; }
    ImpressSessionStart(s);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    // ---- 实验: 手动发 Expose (XClearArea exposure=True) 验证"隐藏了但没重绘" ----
    save_bmp("/tmp/iframe_before.bmp");
    {
        Display* d = XOpenDisplay(getenv("DISPLAY"));
        if (d) {
            // 遍历 root 子树找 LO 窗口 (对全部可见子窗口发 expose)
            std::function<void(Window)> flood = [&](Window w){
                Window rr, pp; Window* kids=nullptr; unsigned n=0;
                XWindowAttributes a;
                if (XGetWindowAttributes(d, w, &a) && a.map_state==IsViewable && a.width>100 && a.height>100)
                    XClearArea(d, w, 0,0,0,0, True);
                if (XQueryTree(d, w, &rr, &pp, &kids, &n))
                    { for(unsigned i=0;i<n;i++) flood(kids[i]); if(kids) XFree(kids); }
            };
            flood(DefaultRootWindow(d));
            XSync(d, False);
            printf("expose flood sent\n");
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    // BMP: BGRA -> BGRx BMP 文件
    int w=s_w, h=s_h; int row=((w*3+3)/4)*4;
    std::vector<uint8_t> bmp(54+row*h);
    memcpy(&bmp[0], "BM", 2);
    *(int*)&bmp[2]=54+row*h; *(int*)&bmp[10]=54; *(int*)&bmp[14]=40;
    *(int*)&bmp[18]=w; *(int*)&bmp[22]=h; *(short*)&bmp[26]=1; *(short*)&bmp[28]=24;
    for(int y=0;y<h;y++){ const uint8_t* src=&s_last[(h-1-y)*w*4]; uint8_t* dst=&bmp[54+y*row];
        for(int x=0;x<w;x++){ dst[x*3]=src[x*4]; dst[x*3+1]=src[x*4+1]; dst[x*3+2]=src[x*4+2]; } }
    save_bmp("/tmp/iframe_after.bmp");
    ImpressSessionDestroy(s);
    return 0;
}
