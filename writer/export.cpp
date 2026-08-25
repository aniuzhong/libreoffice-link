// writerlink.cpp — C ABI 导出 (与 calclink/impresslink 同构): 会话生命周期/播放控制/翻页/查询。会话实现见 writer_session.cpp (经验 38)。
// ABI 生命周期守卫 (V4 SessionRegistry/Guard) 与异常边界见 [HANDOFF] 八、[experiences] 经验45。
#include "session.h"

#include <base/abi.h>
#include <base/session_registry.h>  // V4: ABI 入口守卫 + AbiCall 异常边界

namespace {
SessionRegistry g_registry;
}  // namespace

LINK_API void* WriterSessionCreate(const char* path, const char* password, const char* guid,
                                         WriterFrameCallback cb, void* opaque, int width, int height) {
    if (!path || !cb)
        return nullptr;
    WriterSession* s = new WriterSession();
    try {
        if (!s->Create(path, password, guid, cb, opaque, width, height)) {
            delete s;
            return nullptr;
        }
    } catch (const std::exception& e) {
        OfficeLogErr("[WriterLink] Create exception: %s (cleanup + fail)", e.what());
        try { delete s; } catch (...) {}
        return nullptr;
    } catch (...) {
        OfficeLogErr("[WriterLink] Create exception (unknown type, cleanup + fail)");
        try { delete s; } catch (...) {}
        return nullptr;
    }
    g_registry.Register(s);
    return s;
}

LINK_API void WriterSessionDestroy(void* session) {
    if (!g_registry.TryRevoke(session))
        return;  // 空指针或已销毁, no-op (V4/经验 45)
    try {
        delete static_cast<WriterSession*>(session);
    } catch (const std::exception& e) {
        // 析构内 UNO 清理抛异常: 按泄漏处理 (不崩溃优先)
        OfficeLogErr("[WriterLink] Destroy exception: %s (object leaked)", e.what());
    } catch (...) {
        OfficeLogErr("[WriterLink] Destroy exception (unknown type, object leaked)");
    }
}

LINK_API int WriterSessionStart(void* session) {
    return AbiCall("WriterLink.Start", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<WriterSession*>(s)->Start() ? 1 : 0;
        });
    });
}

LINK_API int WriterSessionStop(void* session) {
    return AbiCall("WriterLink.Stop", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<WriterSession*>(s)->Stop() ? 1 : 0;
        });
    });
}

LINK_API int WriterSessionPause(void* session) {
    return AbiCall("WriterLink.Pause", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<WriterSession*>(s)->Pause() ? 1 : 0;
        });
    });
}

LINK_API int WriterSessionResume(void* session) {
    return AbiCall("WriterLink.Resume", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<WriterSession*>(s)->Resume() ? 1 : 0;
        });
    });
}

LINK_API int WriterSessionUpdateFrame(void* session) {
    return AbiCall("WriterLink.UpdateFrame", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<WriterSession*>(s)->UpdateFrame() ? 1 : 0;
        });
    });
}

LINK_API int WriterSessionSetResolution(void* session, int width, int height) {
    return AbiCall("WriterLink.SetResolution", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<WriterSession*>(s)->SetResolution(width, height) ? 1 : 0;
        });
    });
}

LINK_API int WriterSessionNextPage(void* session) {
    return AbiCall("WriterLink.NextPage", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<WriterSession*>(s)->NextPage() ? 1 : 0;
        });
    });
}

LINK_API int WriterSessionPreviousPage(void* session) {
    return AbiCall("WriterLink.PreviousPage", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<WriterSession*>(s)->PreviousPage() ? 1 : 0;
        });
    });
}

LINK_API int WriterSessionGoToPage(void* session, int page) {
    return AbiCall("WriterLink.GoToPage", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<WriterSession*>(s)->GoToPage(page) ? 1 : 0;
        });
    });
}

LINK_API int WriterSessionGetCurrentPage(void* session) {
    return AbiCall("WriterLink.GetCurrentPage", -1, [&]() -> int {
        return g_registry.WithGuard<int>(session, -1, [&](void* s) {
            return static_cast<WriterSession*>(s)->GetCurrentPage();
        });
    });
}

LINK_API int WriterSessionGetPageCount(void* session) {
    return AbiCall("WriterLink.GetPageCount", -1, [&]() -> int {
        return g_registry.WithGuard<int>(session, -1, [&](void* s) {
            return static_cast<WriterSession*>(s)->GetPageCount();
        });
    });
}

LINK_API int WriterSessionGetWidth(void* session) {
    return AbiCall("WriterLink.GetWidth", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<WriterSession*>(s)->GetWidth();
        });
    });
}

LINK_API int WriterSessionGetHeight(void* session) {
    return AbiCall("WriterLink.GetHeight", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<WriterSession*>(s)->GetHeight();
        });
    });
}
