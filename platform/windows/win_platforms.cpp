// win_platforms.cpp — Windows 平台工厂。
// calc/impress 共用 WindowsPlatform (每 session 独立 soffice + 独立桌面,
// 迁移自 calc/windows/calc_platform.cpp; 文档类型差异 = profile 子目录名)。
// impress 平台已补齐 (2026-08-17, 原 stub 返回 nullptr); 会话层差异
// (calc 窗口化 / impress 全屏放映等) 在会话内, 平台层通用。
#include "win_platform.h"

LinkPlatform* CreateCalcPlatform() {
    return new WindowsPlatform("calclink");
}

LinkPlatform* CreateImpressPlatform() {
    return new WindowsPlatform("impresslink");
}
