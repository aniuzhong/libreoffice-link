cmake_minimum_required(VERSION 3.16)
project(WriterLink LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../common/cmake")
find_package(LibreOfficeSDK REQUIRED)
find_package(Threads REQUIRED)

# 部署目录由 SDK 根统一推导 (单一构建树内不用 CACHE 变量, 避免缓存旧值)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    get_filename_component(_LO_SDK_PARENT "${LIBREOFFICE_SDK_ROOT}/.." ABSOLUTE)
    set(WRITERLINK_DEPLOY_DIR "${_LO_SDK_PARENT}/program")
else()
    set(WRITERLINK_DEPLOY_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../NovaPlayer/bin/office/program")
endif()

# writerlink = 自治 PDF 位图管线 (经验 38): 无平台层 — 无窗口/无抓帧/无 slot,
# 不实例化 LinkPlatform。链接: Linux 唯一依赖 office_runtime (内核/BootLock/
# OfficeLog); Windows 链 common 取共享工具 (OfficeLog/GetLinkDir/BootstrapSession,
# 静态库按需取对象, 平台层代码不落地), 链接形态同 impress (rpath + 显式
# libstdc++, 任何进程 dlopen 免 LD_LIBRARY_PATH)
add_library(WriterLink SHARED
    writerlink.cpp
    writer_session.cpp
    md5.cpp)
set_target_properties(WriterLink PROPERTIES OUTPUT_NAME writerlink PREFIX "")
# 构建输出目录 = 部署目录 (单副本, 防 .so 双份致 GetRuntimeDir 错位, 经验 33)
set_target_properties(WriterLink PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${WRITERLINK_DEPLOY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${WRITERLINK_DEPLOY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${WRITERLINK_DEPLOY_DIR}")

target_include_directories(WriterLink PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${CMAKE_CURRENT_SOURCE_DIR}/../office_runtime"
    "${CMAKE_CURRENT_SOURCE_DIR}/../include") # spdlog / UNO IDL
# UNO 头 + CPPU_ENV 宏 (Windows 侧同样需要)
target_link_libraries(WriterLink PRIVATE LibreOffice::Sdk)

# Windows: common 共享工具 (OfficeLog/GetLinkDir/BootstrapSession, 2026-08-17;
# 平台层不实例化, 见文件头注释)
if(WIN32)
    target_link_libraries(WriterLink PRIVATE common)
endif()

if(UNIX AND NOT APPLE)
    target_compile_definitions(WriterLink PRIVATE SAL_UNX UNX LINUX GCC)
    # 共享内核/BootLock/OfficeLog (Linux 专属; Windows 侧会话内 bootstrap)
    target_link_libraries(WriterLink PRIVATE OfficeRuntime)
    # link_utils (u2s/s2u/HideUiBlock 等会话公共工具; 与 calc/impress 对齐)
    target_link_libraries(WriterLink PRIVATE common)
    target_link_libraries(WriterLink PRIVATE
        -Wl,--allow-shlib-undefined
        -Wl,--export-dynamic
        Threads::Threads rt dl)
    target_link_libraries(WriterLink PRIVATE
        "-Wl,-rpath,${WRITERLINK_DEPLOY_DIR}"
        "${WRITERLINK_DEPLOY_DIR}/libstdc++.so.6")
endif()

add_custom_command(TARGET WriterLink POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:WriterLink> "${WRITERLINK_DEPLOY_DIR}/"
    COMMENT "Deploying writerlink to ${WRITERLINK_DEPLOY_DIR}")
