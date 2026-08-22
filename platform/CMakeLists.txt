# 平台抽象层 (STATIC): LinkPlatform 接口 + 按环境归组的平台实现
# (linux/: Xvfb 共享内核模式 + KernelHost 实现; windows/: 独立进程模式 +
# win_office_log + KernelHost 实现, 经验 39)。平台差异的家 (HANDOFF 3.3 三原则)。
# Linux 依赖 OfficeRuntime (共享内核) + base (link_utils 工具); Windows 仅依赖 base。
add_library(platform STATIC
    $<$<PLATFORM_ID:Linux>:linux/xvfb_platform.cpp>
    $<$<PLATFORM_ID:Linux>:linux/linux_platforms.cpp>
    $<$<PLATFORM_ID:Windows>:windows/win_platform.cpp>
    $<$<PLATFORM_ID:Windows>:windows/win_platforms.cpp>
    $<$<PLATFORM_ID:Windows>:windows/win_office_log.cpp>
    $<$<PLATFORM_ID:Windows>:windows/kernel_host.cpp>)

target_include_directories(platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(platform PUBLIC LibreOffice::Sdk link_include)
# link_utils (GetLinkDir/BootstrapSession/to_path) + log 声明 (win_office_log 实现)
target_link_libraries(platform PUBLIC base)

if(UNIX AND NOT APPLE)
    target_compile_definitions(platform PUBLIC SAL_UNX UNX LINUX GCC)
    # 共享内核 (kernel_host/xvfb_platform 使用) + Threads/X11
    target_link_libraries(platform PUBLIC OfficeRuntime)
    target_link_libraries(platform PRIVATE Threads::Threads X11::X11)
    # 部署目录内解析 UNO 运行库 (与 links 同模式)
    target_link_libraries(platform PRIVATE -Wl,--allow-shlib-undefined)
endif()
