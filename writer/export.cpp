// writerlink.cpp — C ABI 导出 (与 calclink/impresslink 同构): 会话生命
// 周期/播放控制/翻页/查询。会话实现见 writer_session.cpp (经验 38)。
//
// V4 修复 (2026-08-20, HANDOFF 七、已知漏洞): 所有 ABI 入口经
// SessionRegistry::Guard 校验在册才转发 — 销毁后调用任意 API 一律 no-op,
// 不触碰已释放内存; Guard 持锁期间 Destroy 的注销+delete 阻塞等待。
// 会话内 destroyed_ 标志为第二层防护。经验 45 (重复 Destroy) 由
// TryRevoke 原有语义承接 (writer 此前完全无防护, 本次补齐)。
//
// ABI 异常边界 (V4 入口守卫第二要素): 所有导出函数体经 AbiCall 包裹,
// C++ 异常不得逃逸 C ABI (逃逸 → std::terminate → SIGABRT, calclink
// attack_uaf_probe UAF-1 同类实证)。
#include <base/abi.h>

#include "session.h"

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
