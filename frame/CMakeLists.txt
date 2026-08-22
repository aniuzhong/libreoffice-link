# 帧泵 (STATIC): FramePump 统一帧泵 (经验 42), 三链 (impress/calc/writer) 共用。
add_library(frame STATIC frame_pump.cpp)
target_include_directories(frame PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# FramePump 单测 (树内编译不部署, 沿 office_runtime_test 模式)
if(UNIX AND NOT APPLE)
    add_executable(frame_pump_test frame_pump_test.cpp)
    target_link_libraries(frame_pump_test PRIVATE frame Threads::Threads)
endif()
