#pragma once

#include <stdint.h>

// LINK_API — C ABI 导出宏 (对齐 LO dllapi.h 模式: 每模块一个 DLLPUBLIC 宏)。
// 三个 link (calclink/impresslink/writerlink) 共用; 探针/上层 dlopen 侧
// 不含宏使用, 纯 dlopen/dlsym 不依赖导入声明。
#ifdef _WIN32
#define LINK_API extern "C" __declspec(dllexport)
#else
#define LINK_API extern "C"
#endif

// 三 link (calclink/impresslink/writerlink) 统一 C ABI 接口声明。
// 实际调用方 (探针/NovaOfficeCore) 均为 dlopen/dlsym 动态加载, 本头仅
// 供编译期声明; ABI 面 = 函数签名 + LinkFrameCallback 布局, 三 link 同构。
// 各 link 独有 API (CalcSessionMoveScroll/SetScale, ImpressSessionSetMute)
// 在对应节内声明; 通用会话 API 三 link 一致。

/** 通用帧回调类型 (三 link 同构)。
    @param data      BGRA 像素数据, 行主序
    @param width     帧宽 (像素)
    @param height    帧高 (像素)
    @param row_pitch 每行字节数 (width*4 + 对齐填充)
    @param size      总字节数
    @param format    保留 (当前恒为 kFrameFormatBGRA)
    @param opaque    Create 时传入的用户数据
 */
typedef void (*LinkFrameCallback)(const uint8_t* data, int32_t width,
    int32_t height, int32_t row_pitch, int32_t size, int32_t format, void* opaque);

// 各 link 回调类型别名 (兼容旧名, 与 LinkFrameCallback 同布局)
typedef LinkFrameCallback ImpressFrameCallback;
typedef LinkFrameCallback CalcFrameCallback;
typedef LinkFrameCallback WriterFrameCallback;

// ---- 通用契约 (三 link 一致) ----
// 返回码: 0=成功, 非0=失败 (具体错误码未细分, 失败详情在日志)。
// 线程安全: 不同 session 句柄的 API 可并发; 同一 session 的调用须外部串行化
//   (帧回调 cb_ 内不得再调本 session 的任何 API, 见 V5 锁序约束)。
// 生命周期: Destroy 后该句柄上的一切 API 调用为 no-op (SessionRegistry 守卫)。
// 帧回调: 在帧泵线程调用, 调用方须尽快返回, 不得阻塞 (拷贝语义)。
// width/height: 文档输出分辨率 (≤ 子屏位最大分辨率; writer 为渲染边界框按比例适配)。

// ================= Impress =================
// 核心播放接口, 非核心 (备注/IsNextToLast/媒体/静音) 由上层 PptManager
// 基类默认处理。
LINK_API void* ImpressSessionCreate(const char* path, const char* password, const char* guid, ImpressFrameCallback cb, void* opaque, int width, int height);
LINK_API void  ImpressSessionDestroy(void* session);
LINK_API int   ImpressSessionStart(void* session);                      // 恢复幻灯片 + 开始推送帧
LINK_API int   ImpressSessionStop(void* session);                       // 暂停幻灯片 + 停止帧流 (会话保留, UpdateFrame 可抓)
LINK_API int   ImpressSessionPause(void* session);                      // 幻灯片暂停 + 冻结帧流
LINK_API int   ImpressSessionResume(void* session);
LINK_API int   ImpressSessionUpdateFrame(void* session);                // 强制一帧
LINK_API int   ImpressSessionSetResolution(void* session, int width, int height); // 窗口 resize + XShm 段重建
LINK_API int   ImpressSessionNextPage(void* session);                   // 效果级推进 (gotoNextEffect)
LINK_API int   ImpressSessionPreviousPage(void* session);               // 效果级回退 (gotoPreviousEffect)
LINK_API int   ImpressSessionGoToPage(void* session, int page);         // gotoSlideIndex, 0-based
LINK_API int   ImpressSessionGetCurrentPage(void* session);             // getCurrentSlideIndex, 0-based
LINK_API int   ImpressSessionGetPageCount(void* session);
LINK_API int   ImpressSessionGetWidth(void* session);
LINK_API int   ImpressSessionGetHeight(void* session);
// 静音专项: 全进程 PptX 媒体 shape 静音。
// 上层 (LibreOfficeImpressManager::SetMute override) 经此 ABI 触发
// ImpressSession::SetMute → FfplayPlayer::SetMuteAll 遍历引擎表 set_volume。
// mute: 0=恢复音量, 非0=静音; 返回 1=成功, 0=失败。
LINK_API int   ImpressSessionSetMute(void* session, int mute);

// ================= Calc =================
// width/height: 文档输出分辨率 (目标画面尺寸); 窗口落位在子屏位内,
// 抓帧尺寸 = 实际分辨率。
LINK_API void*     CalcSessionCreate(const char* path, const char* password, const char* guid, CalcFrameCallback cb, void* opaque, int width, int height);
LINK_API void      CalcSessionDestroy(void* session);
LINK_API int       CalcSessionStart(void* session);                      // begin delivering frames
LINK_API int       CalcSessionStop(void* session);
LINK_API int       CalcSessionPause(void* session);                      // freeze frame delivery
LINK_API int       CalcSessionResume(void* session);
LINK_API int       CalcSessionUpdateFrame(void* session);                // force one frame
LINK_API int       CalcSessionSetResolution(void* session, int width, int height); // 窗口 resize + XShm 段重建
LINK_API int       CalcSessionNextPage(void* session);                   // scroll down one viewport
LINK_API int       CalcSessionPreviousPage(void* session);               // scroll up one viewport
LINK_API int       CalcSessionMoveScroll(void* session, int dx, int dy); // +/- 1 per call
LINK_API int       CalcSessionSetSheet(void* session, unsigned index);   // 0-based
LINK_API int       CalcSessionGetSheetCount(void* session);
LINK_API int       CalcSessionGetCurrentSheet(void* session);
LINK_API int       CalcSessionGetWidth(void* session);
LINK_API int       CalcSessionGetHeight(void* session);
LINK_API int       CalcSessionSetScale(void* session, unsigned percent);
LINK_API unsigned  CalcSessionGetScale(void* session);

// ================= Writer =================
// 页模型播放接口。实现链路 = 自治 PDF 位图管线 (经验 38): docx→PDF(缓存协同)
// →Draw 导入→XSlideRenderer 逐页位图, 无窗口/无抓帧 (静态排版内容)。
// SetAutoPlay/SetLoopPlay 不在本层 —— 由上层 Manager 定时器实现 (经验 38 落地决策⑥)。
LINK_API void* WriterSessionCreate(const char* path, const char* password, const char* guid, WriterFrameCallback cb, void* opaque, int width, int height);
LINK_API void  WriterSessionDestroy(void* session);
LINK_API int   WriterSessionStart(void* session);            // 恢复帧流
LINK_API int   WriterSessionStop(void* session);             // 停止帧流 (会话保留, UpdateFrame 可抓)
LINK_API int   WriterSessionPause(void* session);            // 冻结帧流
LINK_API int   WriterSessionResume(void* session);
LINK_API int   WriterSessionUpdateFrame(void* session);      // 强制一帧
LINK_API int   WriterSessionSetResolution(void* session, int width, int height); // 页位图重渲染 (LRU 失效)
LINK_API int   WriterSessionNextPage(void* session);         // 下一页 (同步渲染后立即推帧)
LINK_API int   WriterSessionPreviousPage(void* session);
LINK_API int   WriterSessionGoToPage(void* session, int page); // 0-based
LINK_API int   WriterSessionGetCurrentPage(void* session);   // 0-based
LINK_API int   WriterSessionGetPageCount(void* session);
LINK_API int   WriterSessionGetWidth(void* session);
LINK_API int   WriterSessionGetHeight(void* session);
