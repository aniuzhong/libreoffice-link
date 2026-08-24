// bleed_probe.cpp — 实证"pptx 底部透显 xlsx 表格栅格"根因 + 测试候选修复
//
// 背景 (像素取证): calc 引导期以全屏(30720x2160)渲染表格栅格, 内容留在共享大屏
// 底层; impress 幻灯片窗口底部未绘制带 (实测 37px, y>=1043; 1042 为放映视图底边
// 亮线) 在 Xvfb 无 backing store 下返回底层内容 -> impress 帧底部显示 xlsx 栅格。
//
// 与 Demo 一致的顺序 (先 calc 后 impress) 在隔离 ORT_HOME 里复现:
//   基線 : 计算 impress 帧底部带与 calc 帧同行的像素重合度 (止血指标)
//          -> 存在 >90% 相同的行段 => 透显确凿
//   Fix A: XSetWindowBackground(黑)+XClearArea 清 impress 底部带, 再测
//          -> 骤降 => "显式背景像素/清底带" 修复可行
//   几何 : 内容末行 / 未绘制带高 / 亮线行 (压扁与条带分析; 幻灯片内容应到 1079)
//
// 生产开关 (A/B 矩阵):
//   ORT_BLEED_FIX=0        关闭生产 Fix A (SizeWindowToSlot 显式背景+清+重映射)
//   ORT_ROOT_HYGIENE=0     关闭 root 卫生 (runtime 启动/adopt 时 root 黑背景+清)
//   ORT_IMPRESS_RELAYOUT=1 开启放映视图重排 nudge (消除 37px 未绘制带与纵向压扁)
//   BLEED_HOLD_SEC=N       测量完成后保持会话 N 秒 (现场 xwininfo/抓屏检查)
//
// 用法: ORT_HOME=<隔离基目录> bleed_probe <xlsx> <pptx>
#include <base/abi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

static std::atomic<int> s_impFrames{0}, s_calcFrames{0};
static int s_impW=0,s_impH=0,s_calcW=0,s_calcH=0;
static std::vector<uint8_t> s_impLast, s_calcLast;
static std::mutex s_mtx;

static void ImpressCb(const uint8_t* data,int32_t w,int32_t h,int32_t rp,
                      int32_t size,int32_t format,void*){
    std::lock_guard<std::mutex> lk(s_mtx);
    s_impFrames++; s_impW=w; s_impH=h;
    s_impLast.assign(data,data+size);
}
static void CalcCb(const uint8_t* data,int32_t w,int32_t h,int32_t rp,
                   int32_t size,int32_t format,void*){
    std::lock_guard<std::mutex> lk(s_mtx);
    s_calcFrames++; s_calcW=w; s_calcH=h;
    s_calcLast.assign(data,data+size);
}

struct Bleed {
    int maxRowPct=0, bandMeanPct=0, highRows=0; // >90%相同的行数
    int topMeanLum=0;     // impress 顶部90%行平均亮度 (幻灯片完整性, 空窗≈0)
    std::string leakRows; // 残留的 >90% 行号 (逗号分隔)
    bool valid=false;
};

// 几何指标 (2026-08-24): 内容末行 (最后均值>8 的行) / 未绘制带高 /
// 视图底边亮线行 (均值>200)。幻灯片满窗渲染时 contentEnd 应 = H-1。
struct Geometry {
    int contentEnd=-1, bandRows=-1, brightLineY=-1;
};
static Geometry MeasureGeometry(){
    Geometry g;
    std::lock_guard<std::mutex> lk(s_mtx);
    if(s_impLast.empty()) return g;
    int W=s_impW,H=s_impH;
    for(int y=H-1;y>=0;y--){
        long tot=0; int n=0;
        for(int x=0;x<W;x+=4){
            size_t o=((size_t)y*W+x)*4;
            tot += s_impLast[o]+s_impLast[o+1]+s_impLast[o+2];
            n++;
        }
        int mean=(int)(tot/(n*3));
        if(g.brightLineY<0 && mean>200) g.brightLineY=y;
        if(mean>8 && g.contentEnd<0) g.contentEnd=y;
        if(g.contentEnd>=0) break;
    }
    g.bandRows = (g.contentEnd>=0) ? (H-1-g.contentEnd) : -1;
    return g;
}

// 按 class(res_class) 子串找第一个 viewable 窗口
static Window FindWindowByClass(Display* d, const char* clsSub){
    Window root=DefaultRootWindow(d),rr,pp; Window*kids=nullptr; unsigned n=0;
    Window found=0;
    if(XQueryTree(d,root,&rr,&pp,&kids,&n)){
        for(unsigned i=0;i<n&&!found;i++){
            XClassHint ch={};
            if(XGetClassHint(d,kids[i],&ch)){
                std::string rc=ch.res_class?ch.res_class:"";
                if(rc.find(clsSub)!=std::string::npos) found=kids[i];
                if(ch.res_name)XFree(ch.res_name);
                if(ch.res_class)XFree(ch.res_class);
            }
        }
        if(kids)XFree(kids);
    }
    return found;
}

// 顶90%行平均亮度 (检测重映射后幻灯片是否还在 / 是否被清空)
static int ImpressTopLum(){
    if(s_impLast.empty()) return -1;
    int W=s_impW,H=s_impH, rows=H*90/100;
    long tot=0; int n=rows*W;
    for(int y=0;y<rows;y++)
        for(int x=0;x<W;x++){
            size_t o=((size_t)y*W+x)*4;
            unsigned char r=s_impLast[o+2],g=s_impLast[o+1],b=s_impLast[o];
            tot += 0.3*r+0.6*g+0.1*b;
        }
    return n? (int)(tot/n) : -1;
}

static Bleed MeasureBleed(int bandRows){
    Bleed r;
    std::lock_guard<std::mutex> lk(s_mtx);
    if(s_impLast.empty()||s_calcLast.empty()||s_impW!=s_calcW||s_impH!=s_calcH)
        return r;
    r.valid=true;
    int W=s_impW,H=s_impH;
    r.topMeanLum=ImpressTopLum();
    int lo=H-bandRows, hi=H, sum=0;
    bool first=true;
    for(int y=lo;y<hi;y++){
        int eq=0;
        for(int x=0;x<W;x++){
            size_t o=((size_t)y*W+x)*4;
            if(s_impLast[o]==s_calcLast[o]&&s_impLast[o+1]==s_calcLast[o+1]&&s_impLast[o+2]==s_calcLast[o+2])
                eq++;
        }
        int pct=eq*100/W;
        if(pct>=90){
            r.highRows++;
            if(first){ r.leakRows+=std::to_string(y); first=false; }
            else r.leakRows+=","+std::to_string(y);
        }
        if(pct>r.maxRowPct) r.maxRowPct=pct;
        sum+=pct;
    }
    r.bandMeanPct=sum/(hi-lo);
    return r;
}

static void Report(const char* tag, int band){
    Bleed b=MeasureBleed(band);
    Geometry g=MeasureGeometry();
    printf("[%s] impress %dx%d vs calc 底部%d行: 峰值=%d%% 平均=%d%% >90%%行=%d 残留行=[%s] 幻灯片顶部亮度=%d\n",
           tag,s_impW,s_impH,band,b.maxRowPct,b.bandMeanPct,b.highRows,b.leakRows.c_str(),b.topMeanLum);
    printf("[%s] 几何: 内容末行=%d 未绘制带=%d行 亮线行=%d %s\n",
           tag,g.contentEnd,g.bandRows,g.brightLineY,
           (!b.valid)?"(数据缺失)":(b.highRows>0?"=> 透显确凿":"=> 未观察到透显"));
}

// 基线帧落盘 PPM (取证: 帧回调的 BGRA 字节序)
static void DumpPpm(const char* path, const std::vector<uint8_t>& bgra, int w, int h){
    FILE* f=fopen(path,"wb");
    if(!f) return;
    fprintf(f,"P6\n%d %d\n255\n",w,h);
    for(int y=0;y<h;y++)
        for(int x=0;x<w;x++){
            size_t o=((size_t)y*w+x)*4;
            fputc(bgra[o+2],f); fputc(bgra[o+1],f); fputc(bgra[o],f);
        }
    fclose(f);
    printf("[probe] dumped %s (%dx%d)\n",path,w,h);
}

int main(int argc,char** argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    if(argc<3){fprintf(stderr,"usage: ORT_HOME=.. bleed_probe <xlsx> <pptx>\n");return 2;}
    const char*xlsx=argv[1],*pptx=argv[2];
    const int band=54;

    printf("[probe] create calc(%s)...\n",xlsx);
    void* calc=CalcSessionCreate(xlsx,"","bleed_calc",CalcCb,nullptr,1920,1080);
    printf("[probe] calc %s\n", calc?"OK":"FAILED"); if(!calc) return 1;
    void* imp=ImpressSessionCreate(pptx,"","bleed_impress",ImpressCb,nullptr,1920,1080);
    printf("[probe] impress %s\n", imp?"OK":"FAILED"); if(!imp) return 1;

    CalcSessionStart(calc);
    ImpressSessionStart(imp);
    printf("[probe] started; collecting frames...\n");
    sleep(6);

    const char* dpy_name = getenv("DISPLAY")?getenv("DISPLAY") : ":90";
    printf("[probe] frames calc=%d impress=%d (display=%s)\n",
           s_calcFrames.load(),s_impFrames.load(),dpy_name);
    Report("基線",band);
    {
        std::lock_guard<std::mutex> lk(s_mtx);
        if(!s_impLast.empty()) DumpPpm("/tmp/bleed_imp_baseline.ppm",s_impLast,s_impW,s_impH);
        if(!s_calcLast.empty()) DumpPpm("/tmp/bleed_calc.ppm",s_calcLast,s_calcW,s_calcH);
    }

    // ---- Fix A 实验 (探针侧独立于生产开关; BLEED_NO_PROBE_FIXA=1 跳过以保留现场) ----
    if(getenv("BLEED_NO_PROBE_FIXA")){
        printf("[x11] BLEED_NO_PROBE_FIXA set, skip probe-side Fix A (保留透显现场)\n");
    } else {
        Display* d=XOpenDisplay(dpy_name);
        if(d){
            Window impwin=FindWindowByClass(d,"impress");
            if(impwin){
                XSetWindowBackground(d,impwin,BlackPixel(d,DefaultScreen(d)));
                XClearArea(d,impwin,0,s_impH-band,s_impW,band,False);
                XSync(d,False);
                printf("[x11] Fix A applied to impress 0x%lx (bg=black, clear bottom %dpx)\n",
                       (unsigned long)impwin,band);
            } else printf("[x11] impress窗口未找到, skip Fix A\n");
            XCloseDisplay(d);
        } else printf("[x11] 无 display %s, skip Fix A\n",dpy_name);
        sleep(2);
    }
    Report("FixA",band);

    // ---- 保持会话供现场检查 (BLEED_HOLD_SEC) ----
    if(const char* hold=getenv("BLEED_HOLD_SEC")){
        int sec=atoi(hold);
        printf("[probe] holding sessions %ds (可 xwininfo -display %s -root -tree 检查)...\n",sec,dpy_name);
        sleep(sec);
    }

    if(calc){CalcSessionStop(calc);CalcSessionDestroy(calc);}
    if(imp){ImpressSessionStop(imp);ImpressSessionDestroy(imp);}
    printf("[probe] done\n");
    return 0;
}
