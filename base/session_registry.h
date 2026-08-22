// session_registry.h — C ABI 会话登记表 (V4 修复: 生命周期入口守卫)。
//
// State machine transition diagram:
//
//   +-----------+   Register()     +--------+   TryRevoke()      +-------------+
//   |  (none)   | ---------------> |  live  | -----------------> |  destroyed  |
//   +-----------+                  +--------+                    +-------------+
//                                      |                              |
//                                      |                              |
//                                      +---------- while -------------+
//                                                 Guard holds lock
//                                                 TryRevoke blocks
//                                                 (prevents in-use delete)
//
// 状态语义:
//   live      ABI 可转发到会话对象 (Guard 校验通过)
//   destroyed ABI 一律 no-op, 不触碰对象内存
//
// Guard (ABI 入口守卫): 构造时持锁并校验在册; 存活期间 (Guard 未析构)
//   TryRevoke 阻塞等待, 对象不会被 delete, 消除 check-then-use TOCTOU。
//
// recursive_mutex: 帧回调 (cb_) 在 UpdateFrame 调用栈内同线程重入其他
//   ABI 函数时安全 (shared_mutex 同线程重入是 UB)。
//
// 与会话内 destroyed_ 标志 (calc/impress/writer_session) 构成双层防护:
//   登记表拦"已 delete 的悬垂指针", 会话标志拦"delete 前后的内部路径"。
#pragma once

#include <exception>
#include <mutex>
#include <unordered_set>

#include "log.h"  // OfficeLogErr (AbiCall 异常边界日志)

class SessionRegistry {
public:
    // Create 成功后调用 (登记 = 进入 live 态)
    void Register(void* s) {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        live_.insert(s);
    }

    // Destroy 入口: 在册则注销返回 true (调用方执行 delete);
    // 不在册返回 false (已销毁, no-op — 经验 45 双重 Destroy 防护)
    bool TryRevoke(void* s) {
        if (!s)
            return false;
        std::lock_guard<std::recursive_mutex> lk(mu_);
        return live_.erase(s) != 0;
    }

    // ABI 入口守卫: 构造时持锁并校验在册; 存活期间 (Guard 未析构) 对象
    // 不会被 TryRevoke+delete。
    class [[nodiscard]] Guard {
    public:
        Guard(SessionRegistry& reg, void* s) : reg_(reg), s_(s) {
            reg_.mu_.lock();
            ok_ = (s_ != nullptr) && reg_.live_.count(s_) != 0;
        }
        ~Guard() { reg_.mu_.unlock(); }
        explicit operator bool() const { return ok_; }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    private:
        SessionRegistry& reg_;
        void* s_;
        bool ok_ = false;
    };

    // 回调式守卫: 持锁校验在册后转发到 session 对象, 异常安全, 省去手动
    // if(!g) return fallback 模板代码。fallback 用于"不在册或异常"两种场景。
    // 用法: return reg.WithGuard<int>(session, 0, [&](void* s){
    //           return static_cast<CalcSession*>(s)->Start() ? 1 : 0;
    //       });
    template <typename Ret, typename Fn>
    Ret WithGuard(void* session, Ret fallback, Fn&& fn) {
        Guard g(*this, session);
        if (!g)
            return fallback;
        return fn(session);
    }

private:
    std::recursive_mutex mu_;
    std::unordered_set<void*> live_;  // guarded by mu_
};

// ---- ABI 异常边界 (V4 入口守卫第二要素, 与 Guard 并列) ----
// C ABI 禁止 C++ 异常逃逸: 宿主线程无 handler (如裸 std::thread 工作线程)
// 时 std::terminate → SIGABRT 全进程崩溃。实证 (2026-08-20 attack_uaf_probe
// UAF-1): 8 线程并发 Create/Destroy 竞态下 loadComponentFromURL 抛
// lang::IllegalArgumentException 逃出 CalcSessionCreate → abort。
// 所有 ABI 导出函数体经 AbiCall 包裹: 异常 → error 日志 + 返回失败值。
// 用法: return AbiCall("CalcLink.Start", 0, [&]{ ...原函数体... });
template <typename Ret, typename Fn>
inline Ret AbiCall(const char* tag, Ret fallback, Fn&& fn) {
    try {
        return fn();
    } catch (const std::exception& e) {
        OfficeLogErr("[%s] exception at C ABI boundary: %s (swallowed)", tag, e.what());
        return fallback;
    } catch (...) {
        OfficeLogErr("[%s] exception at C ABI boundary (unknown type, swallowed)", tag);
        return fallback;
    }
}
