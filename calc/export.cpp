// calclink.cpp — C ABI 导出: 会话生命周期/播放控制/翻页/查询。
// 会话实现见 calc_session.cpp。
//
// V4 修复 (, HANDOFF 七、已知漏洞): 会话生命周期外部状态机由
// SessionRegistry 建模 (在册=live / 不在册=destroyed)。所有 ABI 入口经
// Guard 校验在册才转发 — 销毁后调用任意 API 一律 no-op, 不触碰已释放
// 内存; Guard 持锁期间 Destroy 的注销+delete 阻塞等待, 消除 TOCTOU。
// 会话内 destroyed_ 标志为第二层防护。经验 45 (重复 Destroy) 由
// TryRevoke 原有语义承接。
//
// ABI 异常边界 (V4 入口守卫第二要素): 所有导出函数体经 AbiCall 包裹,
// C++ 异常 (UNO 竞态抛出的 RuntimeException/IllegalArgumentException 等)
// 不得逃逸 C ABI — 逃逸会 std::terminate → SIGABRT (attack_uaf_probe
// UAF-1 实证)。
#include <base/abi.h>
#include "session.h"

#include <base/session_registry.h>  // V4: ABI 入口守卫 + AbiCall 异常边界

namespace {
SessionRegistry g_registry;
}  // namespace

LINK_API void* CalcSessionCreate(const char* path, const char* password, const char* guid, CalcFrameCallback cb, void* opaque, int width, int height) {
    if (!path || !cb)
        return nullptr;
    CalcSession* s = new CalcSession();
    try {
        if (!s->Create(path, password, guid, cb, opaque, width, height)) {
            delete s;
            return nullptr;
        }
    } catch (const std::exception& e) {
        OfficeLogErr("[CalcLink] Create exception: %s (cleanup + fail)", e.what());
        try { delete s; } catch (...) {}
        return nullptr;
    } catch (...) {
        OfficeLogErr("[CalcLink] Create exception (unknown type, cleanup + fail)");
        try { delete s; } catch (...) {}
        return nullptr;
    }
    g_registry.Register(s);
    return s;
}

LINK_API void CalcSessionDestroy(void* session) {
    if (!g_registry.TryRevoke(session))
        return;  // 空指针或已销毁, no-op (V4/经验 45)
    try {
        delete static_cast<CalcSession*>(session);
    } catch (const std::exception& e) {
        // 析构内 UNO 清理抛异常: 对象内存已不可安全回收, 按泄漏处理 (不崩溃优先)
        OfficeLogErr("[CalcLink] Destroy exception: %s (object leaked)", e.what());
    } catch (...) {
        OfficeLogErr("[CalcLink] Destroy exception (unknown type, object leaked)");
    }
}

LINK_API int CalcSessionStart(void* session) {
    return AbiCall("CalcLink.Start", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<CalcSession*>(s)->Start() ? 1 : 0;
        });
    });
}

LINK_API int CalcSessionStop(void* session) {
    return AbiCall("CalcLink.Stop", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<CalcSession*>(s)->Stop() ? 1 : 0;
        });
    });
}

LINK_API int CalcSessionPause(void* session) {
    return AbiCall("CalcLink.Pause", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<CalcSession*>(s)->Pause() ? 1 : 0;
        });
    });
}

LINK_API int CalcSessionResume(void* session) {
    return AbiCall("CalcLink.Resume", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<CalcSession*>(s)->Resume() ? 1 : 0;
        });
    });
}

LINK_API int CalcSessionUpdateFrame(void* session) {
    return AbiCall("CalcLink.UpdateFrame", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<CalcSession*>(s)->UpdateFrame() ? 1 : 0;
        });
    });
}

LINK_API int CalcSessionSetResolution(void* session, int width, int height) {
    return AbiCall("CalcLink.SetResolution", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<CalcSession*>(s)->SetResolution(width, height) ? 1 : 0;
        });
    });
}

LINK_API int CalcSessionNextPage(void* session) {
    return AbiCall("CalcLink.NextPage", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<CalcSession*>(s)->NextPage() ? 1 : 0;
        });
    });
}

LINK_API int CalcSessionPreviousPage(void* session) {
    return AbiCall("CalcLink.PreviousPage", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<CalcSession*>(s)->PreviousPage() ? 1 : 0;
        });
    });
}

LINK_API int CalcSessionMoveScroll(void* session, int dx, int dy) {
    return AbiCall("CalcLink.MoveScroll", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<CalcSession*>(s)->MoveScroll(dx, dy) ? 1 : 0;
        });
    });
}

LINK_API int CalcSessionSetSheet(void* session, unsigned index) {
    return AbiCall("CalcLink.SetSheet", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<CalcSession*>(s)->SetSheet(index) ? 1 : 0;
        });
    });
}

LINK_API int CalcSessionGetSheetCount(void* session) {
    return AbiCall("CalcLink.GetSheetCount", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<CalcSession*>(s)->GetSheetCount();
        });
    });
}

LINK_API int CalcSessionGetCurrentSheet(void* session) {
    return AbiCall("CalcLink.GetCurrentSheet", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<CalcSession*>(s)->GetCurrentSheet();
        });
    });
}

LINK_API int CalcSessionGetWidth(void* session) {
    return AbiCall("CalcLink.GetWidth", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<CalcSession*>(s)->GetWidth();
        });
    });
}

LINK_API int CalcSessionGetHeight(void* session) {
    return AbiCall("CalcLink.GetHeight", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<CalcSession*>(s)->GetHeight();
        });
    });
}

LINK_API int CalcSessionSetScale(void* session, unsigned percent) {
    return AbiCall("CalcLink.SetScale", 0, [&]() -> int {
        return g_registry.WithGuard<int>(session, 0, [&](void* s) {
            return static_cast<CalcSession*>(s)->SetScale(percent) ? 1 : 0;
        });
    });
}

LINK_API unsigned CalcSessionGetScale(void* session) {
    return AbiCall("CalcLink.GetScale", 0u, [&]() -> unsigned {
        return g_registry.WithGuard<unsigned>(session, 0u, [&](void* s) {
            return static_cast<CalcSession*>(s)->GetScale();
        });
    });
}
