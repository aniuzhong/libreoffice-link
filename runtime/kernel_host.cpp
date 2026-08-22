// kernel_host.cpp (Linux) — KernelHost 实现 (G 缝, design-platform-isolation.md Part 2 G)。
// Linux: 共享内核引导缝 (OfficeRuntime::Acquire/BootLock/EnsureKernel)。
// 声明在 base/link_utils.h; 实现按平台归位 (平台差异的家, HANDOFF 3.3 三原则)。
// writer 无 LinkPlatform 层 (经验 38④), calc/impress 走 LinkPlatform 体系不用本类。
#include <base/link_utils.h>  // KernelHost 类声明
#include <base/log.h>         // OfficeLog
#include "runtime.h"  // OfficeRuntime/BootLock/OfficeRuntimeConfig

#include <cstdlib>  // setenv (LANG, 经验 25 陷阱)

namespace link_utils {

struct KernelHost::Impl {
    std::string profile_subdir;
    std::string guid;
    bool acquired = false;                    // OfficeRuntime::Acquire 引用计数
    std::unique_ptr<OfficeRuntime::BootLock> boot_lock;  // 串行化引导+加载 (经验 5)
};

KernelHost::KernelHost(const char* profile_subdir, const char* guid)
    : impl_(std::make_unique<Impl>()) {
    impl_->profile_subdir = profile_subdir ? profile_subdir : "";
    impl_->guid = guid ? guid : "";
    // 共享运行时 (Xvfb+内核) Acquire; writer 无窗口但内核进程 VCL 需 X。
    // 不 AllocSlot (无窗口/无抓帧)。同进程已有 calc/impress 会话则直接复用。
    OfficeRuntimeConfig cfg; // 默认值见 runtime.h (max_docs=8, 3840×2160)
    impl_->acquired = OfficeRuntime::Instance().Acquire(cfg);
    if (!impl_->acquired) {
        OfficeLogErr("[KernelHost] Acquire failed");
    }
}

KernelHost::~KernelHost() {
    // Release 共享运行时引用 (末个 session 释放 Xvfb+内核)
    impl_->boot_lock.reset(); // 确保释放 (若未 Release)
    if (impl_->acquired) {
        OfficeRuntime::Instance().Release();
        impl_->acquired = false;
    }
}

void KernelHost::BeginBoot() {
    if (!impl_->acquired)
        return;
    impl_->boot_lock = std::make_unique<OfficeRuntime::BootLock>();
    OfficeLog("[KernelHost] boot lock acquired");
}

void KernelHost::Release() {
    if (impl_->boot_lock) {
        impl_->boot_lock->Unlock(); // 引导+加载完成, 窗口查找可并行 (经验 5)
        impl_->boot_lock.reset();
        OfficeLog("[KernelHost] boot lock released");
    }
}

css::uno::Reference<css::uno::XComponentContext> KernelHost::ObtainCtx() {
    if (!impl_->acquired)
        return nullptr;
    if (!OfficeRuntime::Instance().EnsureKernel()) {
        OfficeLogErr("[KernelHost] EnsureKernel failed");
        return nullptr;
    }
    // LANG: LO type detection 依赖 locale (env -i 类环境会 type detection
    // failed, 经验 25 陷阱; 不覆盖已有值)
    setenv("LANG", "zh_CN.UTF-8", 0);
    return OfficeRuntime::Instance().kernel();
}

bool KernelHost::ShouldTerminateOnDestroy() const {
    return false; // Linux: 共享内核, 不 terminate
}

}  // namespace link_utils
