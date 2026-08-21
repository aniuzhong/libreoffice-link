# 平台抽象层 (STATIC): LinkPlatform 接口 + 按环境归组的平台实现
# (linux/: Xvfb 共享内核模式; windows/: 独立进程模式 + win_office_log, 经验 39)。
# Linux 依赖 OfficeRuntime 由各 link 的最终链接解析; Windows 仅共享工具。
add_library(platform STATIC
    $<$<PLATFORM_ID:Linux>:linux/xvfb_platform.cpp>
    $<$<PLATFORM_ID:Linux>:linux/linux_platforms.cpp>
    $<$<PLATFORM_ID:Windows>:windows/win_platform.cpp>
    $<$<PLATFORM_ID:Windows>:windows/win_platforms.cpp>
    $<$<PLATFORM_ID:Windows>:windows/win_office_log.cpp>)

target_include_directories(platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(platform PUBLIC LibreOffice::Sdk)

if(UNIX AND NOT APPLE)
    target_compile_definitions(platform PUBLIC SAL_UNX UNX LINUX GCC)
    target_link_libraries(platform PUBLIC OfficeRuntime)
    target_link_libraries(platform PRIVATE Threads::Threads X11::X11)
    # 部署目录内解析 UNO 运行库 (与 links 同模式)
    target_link_libraries(platform PRIVATE -Wl,--allow-shlib-undefined)
endif()
