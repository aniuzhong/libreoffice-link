// linux_platforms.cpp — Linux 平台工厂: 文档类型差异集中于此 (规则 + 前缀)。
// 窗口匹配规则 (实测: calc class 含 soffice/calc/office 或 name 含 Calc; impress class 'libreofficedev-impress', 兼容 soffice/office)。
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
