# 平台公共层 (STATIC): 接口 (link_platform.h) + 跨平台工具 (link_utils/office_paths) +
# 平台实现按环境归组 (linux/: Xvfb 共享内核模式; windows/: 独立进程模式 +
# OfficeLog 实现 win_office_log.cpp, 经验 39)。
# STATIC 库: 链接进各 link (calclink/impresslink/writerlink); Linux 依赖
# OfficeRuntime 由各 link 的最终链接解析, Windows 仅共享工具 (平台层不落地)。
add_library(common STATIC
    link_utils.cpp
    frame_pump.cpp
    $<$<PLATFORM_ID:Linux>:linux/xvfb_platform.cpp>
    $<$<PLATFORM_ID:Linux>:linux/linux_platforms.cpp>
    $<$<PLATFORM_ID:Windows>:windows/win_platform.cpp>
    $<$<PLATFORM_ID:Windows>:windows/win_platforms.cpp>
    $<$<PLATFORM_ID:Windows>:windows/win_office_log.cpp>)

# 接口头公开给各 link (link_platform.h / link_utils.h)
target_include_directories(common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(common PUBLIC LibreOffice::Sdk)

if(UNIX AND NOT APPLE)
    target_compile_definitions(common PUBLIC SAL_UNX UNX LINUX GCC)
    # Xvfb 平台依赖 office_runtime (Xvfb/内核/slot); 依赖图: link -> common -> OfficeRuntime
    target_link_libraries(common PUBLIC OfficeRuntime)
    target_link_libraries(common PRIVATE Threads::Threads X11::X11)
    # 部署目录内解析 UNO 运行库 (与 links 同模式)
    target_link_libraries(common PRIVATE -Wl,--allow-shlib-undefined)

    # FramePump 单测 (树内编译不部署, 沿 office_runtime_test 模式)
    add_executable(frame_pump_test frame_pump_test.cpp)
    target_link_libraries(frame_pump_test PRIVATE common Threads::Threads)
endif()
