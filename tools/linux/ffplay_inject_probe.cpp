// ffplay_inject_probe.cpp — 验证 Manager_FFPlay 正规注入 (独立 service 名)
// EnsureKernel(office_runtime) -> createInstance("com.sun.star.comp.avmedia.Manager_FFPlay")
// -> createPlayer(url) -> 打印结果。绿色视频 = 壳的 GreenPlayer 渲染 (注入成功判据)。
// 用法: ffplay_inject_probe [media_url]
// 前提: unorc 已追加 ffplay.rdb; ffplay.so 在 office/program。
#include <runtime/runtime.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include <com/sun/star/lang/XMultiComponentFactory.hpp>
#include <com/sun/star/media/XManager.hpp>
#include <com/sun/star/media/XPlayer.hpp>
#include <com/sun/star/media/XPlayerWindow.hpp>

using css::uno::Reference;
using css::uno::UNO_QUERY;

static std::string u2s(const rtl::OUString& s) {
    rtl::OString o = rtl::OUStringToOString(s, RTL_TEXTENCODING_UTF8);
    return std::string(o.getStr(), o.getLength());
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* url = (argc > 1) ? argv[1] : "file:///tmp/test.mp4";

    // 1. 共享内核 (与生产同路径, profile=~/.office-link/player)
    OfficeRuntimeConfig cfg;
    cfg.max_docs = 8;
    cfg.max_doc_width = 3840;
    cfg.max_doc_height = 2160;
    if (!OfficeRuntime::Instance().Acquire(cfg)) { fprintf(stderr, "Acquire failed\n"); return 1; }
    if (!OfficeRuntime::Instance().EnsureKernel()) { fprintf(stderr, "EnsureKernel failed\n"); return 1; }
    auto ctx = OfficeRuntime::Instance().kernel();

    // 2. createInstance Manager_FFPlay (正规独立 service 名, 非 hack GStreamer)
    auto sm = ctx->getServiceManager();
    Reference<css::media::XManager> mgr;
    try {
        mgr.set(sm->createInstanceWithContext(
                    rtl::OUString("com.sun.star.comp.avmedia.Manager_FFPlay"), ctx),
                UNO_QUERY);
    } catch (const css::uno::Exception& e) {
        fprintf(stderr, "[INJECT] createInstance EXC: %s\n", u2s(e.Message).c_str());
    }
    if (!mgr.is()) {
        fprintf(stderr, "[INJECT] FAILED: Manager_FFPlay not creatable (unorc 注册未生效?)\n");
    }
    // 对照组: 系统 GStreamer manager (已部署组件, 验证 createInstance 环境本身)
    try {
        Reference<css::media::XManager> gstmgr;
        gstmgr.set(sm->createInstanceWithContext(
                       rtl::OUString("com.sun.star.comp.avmedia.Manager_GStreamer"), ctx),
                   UNO_QUERY);
        fprintf(stderr, "[INJECT] control GStreamer manager: %s\n", gstmgr.is() ? "OK" : "null");
    } catch (const css::uno::Exception& e) {
        fprintf(stderr, "[INJECT] control GStreamer EXC: %s\n", u2s(e.Message).c_str());
    }
    if (!mgr.is()) return 1;
    fprintf(stderr, "[INJECT] Manager_FFPlay created OK\n");

    // 3. createPlayer (壳返回 GreenPlayer; 窗口渲染绿色 = 注入成功判据)
    Reference<css::media::XPlayer> player;
    try {
        player = mgr->createPlayer(rtl::OUString::createFromAscii(url));
        if (player.is()) {
            fprintf(stderr, "[INJECT] createPlayer OK (GreenPlayer, 绿色视频判据)\n");
            fprintf(stderr, "[INJECT] duration=%.2f s\n", player->getDuration());
        } else {
            fprintf(stderr, "[INJECT] createPlayer returned null\n");
            return 1;
        }
    } catch (const css::uno::Exception& e) {
        fprintf(stderr, "[INJECT] createPlayer EXC: %s\n", u2s(e.Message).c_str());
        return 1;
    }

    // ===== UNO 跨进程远程调用验证 =====
    // 验证目标: 主进程通过 remote ctx 调子进程内 FfplayPlayer 的 setMute/isMute。
    // 不需创建引擎 (createPlayerWindow 才会创建引擎), 仅测 mute_ 成员的远程往返。
    // 若 setMute(true) 后 isMute() 返回 true, 证明 UNO marshalling 跨进程可用,
    // 则方案 C (C1 伪单例+XFastPropertySet / C2 枚举 MediaShape setMuted) 均成立。
    fprintf(stderr, "\n[UNO-REMOTE] === 跨进程 XPlayer::setMute/isMute 验证 ===\n");
    try {
        sal_Bool m0 = player->isMute();
        fprintf(stderr, "[UNO-REMOTE] initial isMute=%d\n", (int)m0);
        player->setMute(true);
        sal_Bool m1 = player->isMute();
        fprintf(stderr, "[UNO-REMOTE] after setMute(true) isMute=%d  %s\n",
                (int)m1, m1 ? "OK 跨进程生效" : "FAIL 未生效");
        player->setMute(false);
        sal_Bool m2 = player->isMute();
        fprintf(stderr, "[UNO-REMOTE] after setMute(false) isMute=%d  %s\n",
                (int)m2, !m2 ? "OK 恢复" : "FAIL 未恢复");
        // 二次 createInstance 验证: 是否同一 Manager 实例 (影响 C1 伪单例可行性)
        Reference<css::media::XManager> mgr2;
        try {
            mgr2.set(sm->createInstanceWithContext(
                        rtl::OUString("com.sun.star.comp.avmedia.Manager_FFPlay"), ctx),
                    UNO_QUERY);
        } catch (...) {}
        fprintf(stderr, "[UNO-REMOTE] second createInstance: mgr2=%s  %s\n",
                mgr2.is() ? "OK" : "null",
                (mgr2.is() && mgr2.get() == mgr.get()) ? "(同一实例, 已是单例!)" :
                (mgr2.is() ? "(新实例, 非 singleton — C1 需伪单例改造)" : "(创建失败)"));
    } catch (const css::uno::Exception& e) {
        fprintf(stderr, "[UNO-REMOTE] EXC: %s\n", u2s(e.Message).c_str());
    }

    fprintf(stderr, "[INJECT] SUCCESS\n");
    return 0;
}
