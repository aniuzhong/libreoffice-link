// kernel_host.cpp (Windows) — KernelHost 实现 (G 缝, [platform-isolation] Part 2 G)。
// Windows: 每 session 独立 soffice 三参 bootstrap (自引导缝)。
// 声明在 base/link_utils.h; 实现按平台归位 (平台差异的家, HANDOFF 3.3 三原则)。
#include <base/link_utils.h>    // KernelHost 类声明 + GetLinkDir/BootstrapSession/to_path
#include <base/office_paths.h>  // desktop_profile (per-session profile 目录)
#include <base/log.h>           // OfficeLog

#include <filesystem>

namespace link_utils {

struct KernelHost::Impl {
    std::string profile_subdir;
    std::string guid;
    std::string profile; // Windows: per-session profile 目录
};

KernelHost::KernelHost(const char* profile_subdir, const char* guid)
    : impl_(std::make_unique<Impl>()) {
    impl_->profile_subdir = profile_subdir ? profile_subdir : "";
    impl_->guid = guid ? guid : "";
    // Windows: per-session profile 目录 (writer 离屏管线, 桌面名空)
    impl_->profile = office_paths::desktop_profile(impl_->profile_subdir, impl_->guid);
    std::error_code ec;
    std::filesystem::create_directories(to_path(impl_->profile), ec);
}

KernelHost::~KernelHost() {
    // Windows: 独立 soffice 进程, terminate 由会话层 (ShouldTerminateOnDestroy 门控);
    // profile 目录随 soffice 进程退出遗留 (下次 fresh), 无显式清理。
}

void KernelHost::BeginBoot() {
    // Windows: 每 session 独立 soffice 进程, 无共享内核串行需求 (经验 5 不适用)
}

void KernelHost::Release() {
    // Windows: 无引导锁
}

css::uno::Reference<css::uno::XComponentContext> KernelHost::ObtainCtx() {
    // Windows: 每 session 独立 soffice 三参 bootstrap (空桌面名, writer 离屏)
    auto ctx = BootstrapSession(GetLinkDir(), impl_->profile, "");
    if (!ctx.is())
        OfficeLogErr("[KernelHost] BootstrapSession failed");
    return ctx;
}

bool KernelHost::ShouldTerminateOnDestroy() const {
    return true;  // Windows: 每 session 独立 soffice 进程须 terminate 退出
}

}  // namespace link_utils
