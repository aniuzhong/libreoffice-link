# 共享基础层 (STATIC): 跨平台会话工具 (link_utils/log/office_paths) +
# 统一 C ABI 声明 (abi.h/session_registry.h) + 帧泵 (frame_pump)。
# 零项目依赖 (仅 LibreOffice::Sdk); Windows 平台同样链入。
add_library(base STATIC
    link_utils.cpp
    frame_pump.cpp)
target_include_directories(base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(base PUBLIC LibreOffice::Sdk link_include)

if(UNIX AND NOT APPLE)
    target_compile_definitions(base PUBLIC SAL_UNX UNX LINUX GCC)
    # FramePump 单测 (树内编译不部署)
    add_executable(frame_pump_test frame_pump_test.cpp)
    target_link_libraries(frame_pump_test PRIVATE base Threads::Threads)
endif()
