# 基础工具层 (STATIC): 跨平台会话工具 (u2s/s2u/HideUiBlock/FrameFormat 常量) +
# OfficeLog 声明 (log.h) + .office-link 路径命名空间 (office_paths.h)。
# 零依赖 runtime/office_runtime; Windows 平台同样链入 (win_office_log 实现 Log)。
add_library(base STATIC link_utils.cpp)
target_include_directories(base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(base PUBLIC LibreOffice::Sdk)

if(UNIX AND NOT APPLE)
    target_compile_definitions(base PUBLIC SAL_UNX UNX LINUX GCC)
    # link_utils 的 KernelHost (G 缝) Linux 实现依赖 runtime (引导/BootLock);
    # PUBLIC 保持与旧 common 相同的链接图 (common -> OfficeRuntime 传递)
    target_link_libraries(base PUBLIC OfficeRuntime)
endif()
