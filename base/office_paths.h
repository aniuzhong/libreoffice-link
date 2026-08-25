// office_paths.h — .office-link 路径命名空间统一 (双平台整合)。
// 此前路径拼接散落 4 个文件 (office_runtime/xvfb_platform/win_platform/
// writer_session) 各自 getenv/SHGetFolderPath, Windows 侧基目录分裂
// (profile 用 Roaming, 日志/缓存用 Local)。
// 本模块为纯函数 header-only: office_runtime 不链 common (经验 32), inline
// 实现零链接依赖, 任何调用方 include 即用 —— 单点持有基目录决策与子路径派生。
// 决策 (定稿):
//   - Linux 基目录保持 ~/.office-link (fallback /tmp/.office-link): 零迁移
//     成本, 已部署机器日志/缓存/内核 profile 不失效; 播放器为固定部署环境,
//     不引入 XDG 迁移
//   - Windows 基目录统一 %LOCALAPPDATA%\office-link (全 Local): 内容均本机
//     数据 (日志/缓存/会话 profile), 无域漫游需求 (原 profile 在 Roaming)
//   - ORT_HOME 环境变量覆盖基目录 (沿用 ORT_LOG 先例; 探针/测试/多实例隔离)
//   - 输出统一 forward slashes (Windows 路径 API 兼容, 消灭分隔符分裂)
// 边界 (依赖单向纪律, 经验 38③): NPOfficeCache 是 NovaOfficeCore 缩略图链的
// 命名空间 (非本模块), writer 只读消费; 统一方向只能是上层提供查询接口。
#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // 防 windows.h min/max 宏污染 UNO 头
#endif
#include <shlobj.h>
#include <windows.h>
#endif

namespace office_paths {

// 基目录 (forward slashes)。
inline std::string home() {
    const char* ov = getenv("ORT_HOME");
    if (ov && ov[0]) {
        std::string s = ov;
        std::replace(s.begin(), s.end(), '\\', '/');
        return s;
    }
#ifdef _WIN32
    char buf[MAX_PATH] = { 0 };
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf))) {
        std::string s = std::string(buf) + "\\office-link";
        std::replace(s.begin(), s.end(), '\\', '/');
        return s;
    }
    return "C:/office-link";
#else
    const char* home = getenv("HOME");
    return (home && home[0]) ? std::string(home) + "/.office-link"
                             : "/tmp/.office-link";
#endif
}

// 日志目录 (OfficeLog 落位; Linux 实现 office_runtime spdlog, Windows 实现
// common win_office_log)
inline std::string logs_dir() {
    return home() + "/logs";
}

// writer PDF 内容缓存目录 (经验 38③, 键 = 源文件 MD5)
inline std::string writer_cache_dir() {
    return home() + "/writer_cache";
}

// per-session 工作 profile (Windows: 每 session 独立 soffice + CreateDesktop,
// 名字直指桌面机制; -env:UserInstallation 消费, session 创建时从模板 fresh
// copy 初始化, 会话期临时用后即弃)。Linux 无消费方 (共享内核走 xvfb_profile)。
inline std::string desktop_profile(const std::string& link,
                                   const std::string& guid) {
    std::string p = home() + "/desktops/" + link;
    if (!guid.empty())
        p += "/" + guid;
    return p;
}

// 共享内核工作 profile (Linux: 内核跑在 Xvfb 上, 名字直指虚拟屏机制;
// 经验 27 隔离语义不变,  由 player/ 更名 + 引导时从模板 fresh
// copy 初始化 — 与 Windows 同一规则, 频率为每内核引导一次)。
inline std::string xvfb_profile() {
    return home() + "/xvfb";
}

// user 模板目录 (相对部署 program 目录; 模板 = 仓库 templates/user 随构建
// 部署, git 管理, 双平台共享同一份; 消费方按需组合:
// Linux GetRuntimeDir()+user_template(), Windows GetLinkDir()+user_template())
inline std::string user_template() {
    return "/templates/user";
}

}  // namespace office_paths
