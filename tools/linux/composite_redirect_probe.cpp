// composite_redirect_probe.cpp — Direction A (capture 侧 XComposite redirect) 风险点逐项实证
// 对应 xvfb_platform 未来"从文档 backing pixmap 抓帧"改造, 逐项核证:
//   R1 运行时 Xvfb 是否带 Composite/DAMAGE/RENDER
//   R2 redirect 后台 pixmap 位格式是否 32bpp BGRX (经验#13 快路径前提)
//   R3 XShmGetImage 能否直接作用在 backing pixmap 上
//   R4 redirect 保持(不 unredirect)下 幻灯片是否仍推进 (NextPage 改变 pixmap)
//   R5 redirect 保持开启时 teardown(Stop/Destroy) 是否挂起 (生产最关键门槛)
//   R6 redirect 后 link 的 live-screen 帧泵是否停更
//   R7 impress 自身 backing pixmap 底部不含 calc (去泄漏)
// 全程 alarm 兜底防探针自身被 teardown 挂起卡死。
// 用法: ORT_HOME=<隔离基目录> composite_redirect_probe <xlsx> <pptx>
#include <base/abi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <unistd.h>
#include <dlfcn.h>
#include <csignal>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

static std::atomic<int> s_calcFrames{0};
static int s_impW=0,s_impH=0,s_calcW=0,s_calcH=0;
static std::vector<uint8_t> s_calcLast;
static std::mutex s_mtx;

static void CalcCb(const uint8_t* data,int32_t w,int32_t h,int32_t rp,
                   int32_t size,int32_t format,void*){
    std::lock_guard<std::mutex> lk(s_mtx);
    s_calcFrames++; s_calcW=w; s_calcH=h;
    s_calcLast.assign(data,data+size);
}
static void ImpressCb(const uint8_t* data,int32_t w,int32_t h,int32_t rp,
                      int32_t size,int32_t format,void*){
    (void)data;(void)w;(void)h;(void)rp;(void)size;(void)format;
}
static std::atomic<int> s_impLiveFrames{0};
static void ImpressLiveCb(const uint8_t* data,int32_t w,int32_t h,int32_t rp,
                          int32_t size,int32_t format,void*){
    (void)data;(void)w;(void)h;(void)rp;(void)size;(void)format;
    s_impLiveFrames++;
}

static void OnAlarm(int){ printf("[probe] ALARM 挂起超时, 强制退出(-9)\n"); _exit(9); }

static Window FindWindowByClass(Display* d, const char* clsSub){
    Window root=DefaultRootWindow(d),rr,pp; Window*kids=nullptr; unsigned n=0; Window found=0;
    if(XQueryTree(d,root,&rr,&pp,&kids,&n)){
        for(unsigned i=0;i<n&&!found;i++){
            XClassHint ch={};
            if(XGetClassHint(d,kids[i],&ch)){
                std::string rc=ch.res_class?ch.res_class:"";
                if(rc.find(clsSub)!=std::string::npos) found=kids[i];
                if(ch.res_name)XFree(ch.res_name); if(ch.res_class)XFree(ch.res_class);
            }
        }
        if(kids)XFree(kids);
    }
    return found;
}

int main(int argc,char** argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    signal(SIGALRM,OnAlarm); alarm(150);
    if(argc<3){fprintf(stderr,"usage: ORT_HOME=.. composite_redirect_probe <xlsx> <pptx>\n");return 2;}
    const char*xlsx=argv[1],*pptx=argv[2];
    const char*dpy_name=getenv("DISPLAY")?getenv("DISPLAY"):":90";

    void* calc=CalcSessionCreate(xlsx,"","comp_r_calc",CalcCb,nullptr,1920,1080);
    printf("[probe] calc %s\n",calc?"OK":"FAILED");
    void* imp=ImpressSessionCreate(pptx,"","comp_r_imp",ImpressLiveCb,nullptr,1920,1080);
    printf("[probe] impress %s\n",imp?"OK":"FAILED");
    if(!calc||!imp) return 1;
    CalcSessionStart(calc); ImpressSessionStart(imp);
    auto teardown=[&](){ if(calc){CalcSessionStop(calc);CalcSessionDestroy(calc);} if(imp){ImpressSessionStop(imp);ImpressSessionDestroy(imp);} };
    sleep(4);
    // 帧尺寸(由 impress live cb 并未记尺寸; 从 calc 相似取 1920x1080 兜底)
    int w=1920,h=1080;
    printf("[probe] display=%s calcLiveFrames=%d impressLiveFrames=%d\n",
           dpy_name,s_calcFrames.load(),s_impLiveFrames.load());

    // R1 扩展
    Display* d=XOpenDisplay(dpy_name);
    if(!d){printf("no display \"%s\"\n",dpy_name);teardown();return 1;}
    {
        int maj,ev,err;
        printf("R1 Composite present=%s\n",XQueryExtension(d,"Composite",&maj,&ev,&err)?"YES":"NO");
        printf("R1 DAMAGE    present=%s\n",XQueryExtension(d,"DAMAGE",&maj,&ev,&err)?"YES":"NO");
        printf("R1 RENDER    present=%s\n",XQueryExtension(d,"RENDER",&maj,&ev,&err)?"YES":"NO");
    }

    Window win=FindWindowByClass(d,"impress");
    if(!win){printf("找不到 impress 窗口\n");teardown();return 1;}

    // redirect 永久(不 unredirect), 生产语义
    void*hc=dlopen("libXcomposite.so.1",RTLD_NOW|RTLD_LOCAL);
    if(!hc){printf("dlopen libXcomposite 失败: %s\n",dlerror());teardown();return 1;}
    using FnR=int(*)(Display*,Window,int); using FnN=Pixmap(*)(Display*,Window);
    FnR Redirect=(FnR)dlsym(hc,"XCompositeRedirectWindow");
    FnN NamePixmap=(FnN)dlsym(hc,"XCompositeNameWindowPixmap");
    if(!Redirect||!NamePixmap){printf("dlsym 缺失\n");teardown();return 1;}
    Redirect(d,win,1); XSync(d,False);
    Pixmap pm=NamePixmap(d,win); XSync(d,False);
    if(!pm){printf("NameWindowPixmap null\n");teardown();return 1;}
    printf("R2 impress backing pixmap=0x%lx\n",(unsigned long)pm);

    // R2 位格式
    XImage*xi=XGetImage(d,pm,0,0,w,h,AllPlanes,ZPixmap);
    if(!xi){printf("R2 XGetImage(pixmap) FAIL\n");teardown();return 1;}
    {
        // 注: XGetImage 对裸 Pixmap 不填 r/g/b_mask(需显式 visual), 故不依赖 mask。
        // 快路径形状=32bpp LSBFirst 且 bpl==w*4 (字节即 B,G,R,X, 见经验#13)。
        int bpp=xi->bits_per_pixel;
        bool fast=(bpp==32)&&(xi->byte_order==LSBFirst)&&(xi->bytes_per_line==w*4);
        printf("R2 backing pixmap: bpp=%d byte_order=%s bpl=%d depth=%d => %s(32bpp直拷形状候选; 真伪需窗口visual下XShm验证)\n",
               bpp,(xi->byte_order==LSBFirst?"LSBFirst":"MSBFirst"),xi->bytes_per_line,xi->depth,
               fast?"是":"否");
        if(!fast) printf("R2 非32bpp直拷形状 => 需走XGetImage+转换(~9ms), 性能下降\n");
    }

    // R3 XShmGetImage on pixmap
    {
        XShmSegmentInfo seg; memset(&seg,0,sizeof(seg));
        XImage* zi=XShmCreateImage(d,DefaultVisual(d,DefaultScreen(d)),xi->depth,ZPixmap,NULL,&seg,w,h);
        if(zi){
            seg.shmid=shmget(IPC_PRIVATE,(size_t)zi->bytes_per_line*h,IPC_CREAT|0600);
            seg.shmaddr=(char*)shmat(seg.shmid,NULL,0);
            if((long)seg.shmaddr!=-1){
                seg.readOnly=False; zi->data=seg.shmaddr; XShmAttach(d,&seg);
                XErrorHandler old=XSetErrorHandler([](Display*,XErrorEvent*)->int{return 0;});
                Status ok=XShmGetImage(d,pm,zi,0,0,AllPlanes);
                XSync(d,False); XSetErrorHandler(old);
                printf("R3 XShmGetImage(on backing pixmap)=%s\n",ok?"OK(快路径可行)":"FAIL(需回退XGetImage)");
                XShmDetach(d,&seg); shmdt(seg.shmaddr); shmctl(seg.shmid,IPC_RMID,NULL);
            } else { if(seg.shmid>=0)shmctl(seg.shmid,IPC_RMID,NULL); printf("R3 shmat fail\n"); }
            XDestroyImage(zi);
        } else printf("R3 XShmCreateImage FAIL\n");
    }

    // R7 底部带 vs calc —— 按 32bpp LSBFirst BGRX 字节序(byte0=B,1=G,2=R)直接解析
    // (与项目 GrabBgra/IsBgrxDirect 同口径, 不依赖 XImage.r/g/b_mask)
    {
        int bpl=xi->bytes_per_line;
        int band=54, high=0, peak=0, sum=0;
        std::lock_guard<std::mutex> lk(s_mtx);
        if((int)s_calcLast.size()>=w*h*4){
            for(int y=h-band;y<h;y++){int eq=0;
                const uint8_t* row=reinterpret_cast<const uint8_t*>(xi->data)+(size_t)y*bpl;
                for(int xx=0;xx<w;xx++){
                    const uint8_t* px=row+(size_t)xx*4; // B,G,R,X
                    unsigned char b=px[0],g=px[1],r=px[2];
                    size_t o=((size_t)y*w+xx)*4;
                    if(r==s_calcLast[o+2]&&g==s_calcLast[o+1]&&b==s_calcLast[o])eq++;
                }int pct=eq*100/w; if(pct>=90)high++; if(pct>peak)peak=pct; sum+=pct;
            }
            printf("R7 redirect后台pixmap底部%d行 vs calc (BGRX直析): 峰值=%d%% >90%%行=%d %s\n",
                   band,peak,high,high>0?"(仍泄漏)":"(不泄漏, Direction A 抓帧侧成立)");
        }
    }

    // R4 redirect 保持下幻灯片推进
    {
        int nbytes=(xi->bits_per_pixel+7)/8; if(nbytes<3)nbytes=3;
        auto sample=[&](XImage*img){  // 采样 BGRX 各通道均值(检测内容变化)
            long s=0; int n=0;
            for(int i=0;i<64;i++){ int yy=((i*37)%h); int xx=((i*53)%w);
                const uint8_t* px=reinterpret_cast<const uint8_t*>(img->data)+((size_t)yy*img->bytes_per_line)+((size_t)xx*nbytes);
                s+=(long)(px[0]+px[1]+px[2]); n++;
            }
            return n? (s/n):0L;
        };
        long bf=sample(xi);
        ImpressSessionNextPage(imp); sleep(2);
        XImage*xi2=XGetImage(d,pm,0,0,w,h,AllPlanes,ZPixmap);
        if(xi2){ long af=sample(xi2);
            printf("R4 redirect保持下 NextPage: 平均采样亮度 before=%ld after=%ld => %s\n",
                   bf,af, (bf!=af)?"pixmap已更新(幻灯片仍在推进)":"pixmap未变(可能推进失效)");
            XDestroyImage(xi2);
        } else printf("R4 无法二次读 pixmap\n");
    }

    // R6 redirect 后 link live 帧泵是否停更
    { int f0=s_impLiveFrames.load(); sleep(2); int f1=s_impLiveFrames.load();
      printf("R6 redirect后 link live 帧泵: %d -> %d (%s)\n",f0,f1,
             (f1>f0)?"仍在走(未感知redirect)":"停更(live路径失效=>须改读pixmap)"); }

    XDestroyImage(xi);

    // R5 teardown 且 redirect 保持
    printf("[R5] teardown(redirect保持)开始...\n");
    CalcSessionStop(calc); ImpressSessionStop(imp);
    CalcSessionDestroy(calc); ImpressSessionDestroy(imp);
    printf("R5 teardown 完成(无挂起) => redirect 生命周期内恒定可行\n");

    // 注: XCloseDisplay 必须先于 dlclose —— libXcomposite 经 Xext 在 Display 上
    // 注册 close hook, 先卸载会让 XCloseDisplay 跳进已卸载地址段 (2026-08-24
    // 实测 SIGSEGV, core bt: XCloseDisplay -> 未映射地址)。
    XCloseDisplay(d); dlclose(hc);
    printf("[probe] done\n");
    return 0;
}