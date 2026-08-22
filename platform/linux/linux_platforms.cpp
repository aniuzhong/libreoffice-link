// linux_platforms.cpp — Linux 平台工厂: 文档类型差异集中于此 (规则 + 前缀)。
// 窗口匹配规则沿袭原 calc/linux 与 impress/linux 的 IsCalcWindow/IsImpressWindow
// (实测: calc 窗口 class 含 soffice/calc/office 或 name 含 Calc; impress 窗口
// class 实测为 'libreofficedev-impress', 兼容 soffice/office)。
#include "xvfb_platform.h"

namespace {

const WindowMatchRule kCalcRule = {
    {"soffice", "calc", "office"}, {"Calc"}, 640, 480,
};
const WindowMatchRule kImpressRule = {
    {"impress", "soffice", "office"}, {}, 640, 480,
};

}  // namespace

LinkPlatform* CreateCalcPlatform() {
    return new XvfbSessionPlatform(kCalcRule, "calc");
}

LinkPlatform* CreateImpressPlatform() {
    return new XvfbSessionPlatform(kImpressRule, "impress");
}
