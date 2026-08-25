// topcorner_probe.cpp — 定位 pptx 顶/左角的 1px(手枪形) calc 残影
// 复现用户现状(FILL=1 生产默认), 抓 pptx/calc 帧, 输出:
//   [shape] 顶/左/上 逐行/逐列与 calc 匹配的位置 + 顶左 40x24 ASCII 图
//   [bbox]  全帧与 calc 匹配像素的包围盒(看手枪/残影范围)
//   [geo]   impress 窗口几何
// 判定: 若匹配区恰好呈"顶部1行全宽 + 左部1列几px高"的 L/手枪形, 且集中在印象的顶左,
//        说明是"内容相对窗口偏移1px"或"出生吸附,blit携带";
// 用法: ORT_HOME=<隔离> topcorner_probe <xlsx> <pptx>
#include <base/abi.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

static int s_impW=0,s_impH=0;
static std::vector<uint8_t> s_impLast,s_calcLast;
static std::mutex sm;
static void CalcCb(const uint8_t*d,int32_t w,int32_t h,int32_t,int32_t,int32_t,void*){std::lock_guard<std::mutex> l(sm);s_calcLast.assign(d,d+(size_t)w*h*4);}
static void ImpCb(const uint8_t*d,int32_t w,int32_t h,int32_t,int32_t,int32_t,void*){std::lock_guard<std::mutex> l(sm);s_impLast.assign(d,d+(size_t)w*h*4);s_impW=w;s_impH=h;}
static Window FindImp(Display*d){Window r=DefaultRootWindow(d),rr,pp,*k=nullptr;unsigned n=0;Window f=0;
 if(XQueryTree(d,r,&rr,&pp,&k,&n)){for(unsigned i=0;i<n&&!f;i++){XClassHint c{};if(XGetClassHint(d,k[i],&c)){if(c.res_class&&std::string(c.res_class).find("impress")!=std::string::npos)f=k[i];if(c.res_name)XFree(c.res_name);if(c.res_class)XFree(c.res_class);}}if(k)XFree(k);}return f;}

int main(int argc,char**argv){
    setvbuf(stdout,nullptr,_IONBF,0);
    if(argc<3){fprintf(stderr,"usage: ORT_HOME=.. topcorner_probe <xlsx> <pptx>\n");return 2;}
    void*C=CalcSessionCreate(argv[1],"","tc_calc",CalcCb,nullptr,1920,1080);
    void*I=ImpressSessionCreate(argv[2],"","tc_imp",ImpCb,nullptr,1920,1080);
    if(!C||!I){fprintf(stderr,"create fail\n");return 1;}
    CalcSessionStart(C); ImpressSessionStart(I); sleep(5);
    const char*dpy=getenv("DISPLAY")?getenv("DISPLAY"):":90";
    int W=s_impW,H=s_impH;
    printf("impress frame %dx%d calc=%zu\n",W,H,s_calcLast.size()/4);
    Display*d=XOpenDisplay(dpy); if(d){Window w=FindImp(d);XWindowAttributes a{};if(XGetWindowAttributes(d,w,&a))printf("[geo] impress 0x%lx %dx%d+%d+%d map=%d\n",(unsigned long)w,a.width,a.height,a.x,a.y,(int)a.map_state);XCloseDisplay(d);}
    if(s_impLast.empty()||s_calcLast.size()<(size_t)W*H*4){printf("no frames\n");return 1;}
    auto match=[&](int y,int x){
        size_t o=((size_t)y*W+x)*4;
        return s_impLast[o]==s_calcLast[o]&&s_impLast[o+1]==s_calcLast[o+1]&&s_impLast[o+2]==s_calcLast[o+2];
    };
    // 顶1行与左1列匹配范围
    printf("[top.y=0] x匹配段: ");{bool in=false;int s0=0;
        for(int x=0;x<W;x++){bool m=match(0,x);if(m&&!in){in=true;s0=x;}else if(!m&&in){printf("[%d,%d) ",s0,x);in=false;}}
        if(in)printf("[%d,%d) ",s0,W); printf("\n");}
    for(int y=1;y<=3;y++){printf("[top.y=%d] x匹配段: ",y);bool in=false;int s0=0;
        for(int x=0;x<W;x++){bool m=match(y,x);if(m&&!in){in=true;s0=x;}else if(!m&&in){printf("[%d,%d) ",s0,x);in=false;}}
        if(in)printf("[%d,%d) ",s0,W); printf("\n");}
    printf("[left.x=0] y匹配段: ");{bool in=false;int s0=0;
        for(int y=0;y<H;y++){bool m=match(y,0);if(m&&!in){in=true;s0=y;}else if(!m&&in){printf("[%d,%d) ",s0,y);in=false;}}
        if(in)printf("[%d,%d) ",s0,H); printf("\n");}
    for(int x=1;x<=3;x++){printf("[left.x=%d] y匹配段: ",x);bool in=false;int s0=0;
        for(int y=0;y<H;y++){bool m=match(y,x);if(m&&!in){in=true;s0=y;}else if(!m&&in){printf("[%d,%d) ",s0,y);in=false;}}
        if(in)printf("[%d,%d) ",s0,H); printf("\n");}
    // 顶左 1px 边框的真实颜色语境: 顶部全行(0..min(64,W)) + 左列 0..3 的 y0..80
    // 若这些"希望是边框"的像素里出现白色(RGB>200) => calc 白色单元格混入 = 真残影;
    // 若整体为单一近黑/均匀色 => 是 PPT 自身黑色模板边框。
    {
        auto px=[&](int y,int x)->std::string{
            size_t o=((size_t)y*W+x)*4;
            char b[16];snprintf(b,sizeof b,"%d,%d,%d",s_impLast[o+2],s_impLast[o+1],s_impLast[o]);return b;};
        // 顶部行 y=0, x 范围 [0,64): 唯一色 + 是否含白/含黑
        std::vector<std::string> c1; int white=0,black=0;
        for(int x=0;x<64;x++){std::string c=px(0,x);bool newv=true;for(auto&e:c1)if(e==c){newv=false;break;}if(newv)c1.push_back(c);
            auto unroll=[&](std::string&s,int n){int rr=0,gg=0,bb=0;sscanf(s.c_str(),"%d,%d,%d",&rr,&gg,&bb);if(rr>200&&gg>200&&bb>200)white++;else if(rr<30&&gg<30&&bb<30)black++;};
            // count white/black at this pixel
            {int rr,gg,bb;sscanf(c.c_str(),"%d,%d,%d",&rr,&gg,&bb);if(rr>200&&gg>200&&bb>200)white++;else if(rr<30&&gg<30&&bb<30)black++;}
        }
        printf("[top.y=0 x0..63] 唯一色=%zu 白色像素=%d 黑色像素=%d => %s\n",c1.size(),white,black,
               white>0?"边框内含白色(calc单元格?)":"边框内无白色");
        // 左列手枪区 x=0..3, y=0..79: 唯一色 + 白色计数
        std::vector<std::string> c2; int w2=0,b2=0;
        for(int yy=0;yy<80;yy++)for(int xx=0;xx<4;xx++){std::string c=px(yy,xx);bool nv=true;for(auto&e:c2)if(e==c){nv=false;break;}if(nv)c2.push_back(c);
            int rr,gg,bb;sscanf(c.c_str(),"%d,%d,%d",&rr,&gg,&bb);if(rr>200&&gg>200&&bb>200)w2++;else if(rr<30&&gg<30&&bb<30)b2++;}
        printf("[left pistol x0..3,y0..79] 唯一色=%zu 白色=%d 黑色=%d => %s\n",c2.size(),w2,b2,
               w2>0?"边缘含白色(calc单元格? 真残影)":"边缘为深色(更似PPT黑边框)");
    }
    // 全帧匹配 bbox(手枪/残影范围, 忽略孤点)
    int minX=W,minY=H,maxX=-1,maxY=-1,mtot=0;
    for(int y=0;y<H;y++)for(int x=0;x<W;x++)if(match(y,x)){if(x<minX)minX=x;if(y<minY)minY=y;if(x>maxX)maxX=x;if(y>maxY)maxY=y;mtot++;}
    printf("[bbox] 与 calc 匹配像素 bbox=(%d,%d)-(%d,%d) 总数=%d (%.3f%%)\n",minX,minY,maxX,maxY,mtot,100.0*mtot/(W*H));
    CalcSessionStop(C);CalcSessionDestroy(C);ImpressSessionStop(I);ImpressSessionDestroy(I);
    printf("[probe] done\n");return 0;
}