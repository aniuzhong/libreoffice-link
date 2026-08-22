// impresslink.cpp — C ABI 导出 (与 calclink 同构): 会话生命周期/播放控制/
// 翻页/查询。会话实现见 impress_session.cpp。
//
// V4 修复 (2026-08-20, HANDOFF 七、已知漏洞): 所有 ABI 入口经
// SessionRegistry::Guard 校验在册才转发 — 销毁后调用任意 API 一律 no-op,
// 不触碰已释放内存; Guard 持锁期间 Destroy 的注销+delete 阻塞等待。
// 会话内 destroyed_ 标志为第二层防护。经验 45 (重复 Destroy) 由
// TryRevoke 原有语义承接。
//
// ABI 异常边界 (V4 入口守卫第二要素): 所有导出函数体经 AbiCall 包裹,
// C++ 异常不得逃逸 C ABI (逃逸 → std::terminate → SIGABRT, calclink
// attack_uaf_probe UAF-1 同类实证)。
#include "../abi/abi.h"

#include <cstdlib>

#include "session.h"
#include "../base/log.h" // OfficeLog (诊断: C ABI 边界调用追踪)
#include "../abi/session_registry.h"  // V4: ABI 入口守卫 + AbiCall 异常边界

namespace {
SessionRegistry g_registry;
}  // namespace

extern "C" {

void* ImpressSessionCreate(const char* path, const char* password, const char* guid, ImpressFrameCallback cb, void* opaque, int width, int height) {
    OfficeLog("[ImpressLink] C ABI Create path=%s guid=%s %dx%d", path ? path : "", guid ? guid : "", width, height);
    ImpressSession* s = new ImpressSession();
    try {
        if (!s->Create(path, password ? password : "", guid ? guid : "", cb, opaque, width, height)) {
            OfficeLog("[ImpressLink] C ABI Create FAILED");
            delete s;
            return nullptr;
        }
    } catch (const std::exception& e) {
        OfficeLogErr("[ImpressLink] Create exception: %s (cleanup + fail)", e.what());
        try { delete s; } catch (...) {}
        return nullptr;
    } catch (...) {
        OfficeLogErr("[ImpressLink] Create exception (unknown type, cleanup + fail)");
        try { delete s; } catch (...) {}
        return nullptr;
    }
    g_registry.Register(s);
    OfficeLog("[ImpressLink] C ABI Create OK session=%p", (void*)s);
    return s;
}

void ImpressSessionDestroy(void* session) {
    // V4/经验 45: 空指针或已销毁 → no-op (避免悬垂指针 delete)
    if (!g_registry.TryRevoke(session)) {
        OfficeLog("[ImpressLink] C ABI Destroy session=%p SKIP (already destroyed)", session);
        return;
    }
    OfficeLog("[ImpressLink] C ABI Destroy session=%p", session);
    try {
        delete static_cast<ImpressSession*>(session);
    } catch (const std::exception& e) {
        // 析构内 UNO 清理抛异常: 按泄漏处理 (不崩溃优先)
        OfficeLogErr("[ImpressLink] Destroy exception: %s (object leaked)", e.what());
    } catch (...) {
        OfficeLogErr("[ImpressLink] Destroy exception (unknown type, object leaked)");
    }
}

int ImpressSessionStart(void* session) {
    OfficeLog("[ImpressLink] C ABI Start session=%p", session);
    int r = AbiCall("ImpressLink.Start", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<ImpressSession*>(s)->Start() ? 1 : 0;
        });
    });
    OfficeLog("[ImpressLink] C ABI Start result=%d", r);
    return r;
}

int ImpressSessionStop(void* session) {
    OfficeLog("[ImpressLink] C ABI Stop session=%p", session);
    int r = AbiCall("ImpressLink.Stop", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<ImpressSession*>(s)->Stop() ? 1 : 0;
        });
    });
    OfficeLog("[ImpressLink] C ABI Stop result=%d", r);
    return r;
}

int ImpressSessionPause(void* session) {
    OfficeLog("[ImpressLink] C ABI Pause session=%p", session);
    int r = AbiCall("ImpressLink.Pause", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<ImpressSession*>(s)->Pause() ? 1 : 0;
        });
    });
    OfficeLog("[ImpressLink] C ABI Pause result=%d", r);
    return r;
}

int ImpressSessionResume(void* session) {
    OfficeLog("[ImpressLink] C ABI Resume session=%p", session);
    int r = AbiCall("ImpressLink.Resume", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<ImpressSession*>(s)->Resume() ? 1 : 0;
        });
    });
    OfficeLog("[ImpressLink] C ABI Resume result=%d", r);
    return r;
}

int ImpressSessionUpdateFrame(void* session) {
    return AbiCall("ImpressLink.UpdateFrame", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<ImpressSession*>(s)->UpdateFrame() ? 1 : 0;
        });
    });
}

int ImpressSessionSetResolution(void* session, int width, int height) {
    return AbiCall("ImpressLink.SetResolution", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<ImpressSession*>(s)->SetResolution(width, height) ? 1 : 0;
        });
    });
}

int ImpressSessionNextPage(void* session) {
    return AbiCall("ImpressLink.NextPage", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<ImpressSession*>(s)->NextPage() ? 1 : 0;
        });
    });
}

int ImpressSessionPreviousPage(void* session) {
    return AbiCall("ImpressLink.PreviousPage", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<ImpressSession*>(s)->PreviousPage() ? 1 : 0;
        });
    });
}

int ImpressSessionGoToPage(void* session, int page) {
    return AbiCall("ImpressLink.GoToPage", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<ImpressSession*>(s)->GoToPage(page) ? 1 : 0;
        });
    });
}

int ImpressSessionGetCurrentPage(void* session) {
    return AbiCall("ImpressLink.GetCurrentPage", -1, [&]() -> int {
        return g_registry.WithGuard<int>(session, -1, [&](void* s) {
            return static_cast<ImpressSession*>(s)->GetCurrentPage();
        });
    });
}

int ImpressSessionGetPageCount(void* session) {
    return AbiCall("ImpressLink.GetPageCount", -1, [&]() -> int {
        return g_registry.WithGuard<int>(session, -1, [&](void* s) {
            return static_cast<ImpressSession*>(s)->GetPageCount();
        });
    });
}

int ImpressSessionGetWidth(void* session) {
    return AbiCall("ImpressLink.GetWidth", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<ImpressSession*>(s)->GetWidth();
        });
    });
}

int ImpressSessionGetHeight(void* session) {
    return AbiCall("ImpressLink.GetHeight", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<ImpressSession*>(s)->GetHeight();
        });
    });
}

int ImpressSessionSetMute(void* session, int mute) {
    OfficeLog("[ImpressLink] C ABI SetMute session=%p mute=%d", session, mute);
    int r = AbiCall("ImpressLink.SetMute", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<ImpressSession*>(s)->SetMute(mute != 0) ? 1 : 0;
        });
    });
    OfficeLog("[ImpressLink] C ABI SetMute result=%d", r);
    return r;
}

} // extern "C"
