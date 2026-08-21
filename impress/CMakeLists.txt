cmake_minimum_required(VERSION 3.16)
project(ImpressLink LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../common/cmake")
find_package(LibreOfficeSDK REQUIRED)
find_package(Threads REQUIRED)
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
endif()

# 部署目录由 SDK 根统一推导 (单一构建树内不用 CACHE 变量, 避免缓存旧值)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    get_filename_component(_LO_SDK_PARENT "${LIBREOFFICE_SDK_ROOT}/.." ABSOLUTE)
    set(IMPRESSLINK_DEPLOY_DIR "${_LO_SDK_PARENT}/program")
else()
    set(IMPRESSLINK_DEPLOY_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../NovaPlayer/bin/office/program")
endif()

add_library(ImpressLink SHARED
    impresslink.cpp
    impress_session.cpp)
# 统一产物名: Linux 为 impresslink.so (LibreOfficeImpressManager dlopen 路径);
# PREFIX "" 去掉 Linux 的 lib 前缀
set_target_properties(ImpressLink PROPERTIES OUTPUT_NAME impresslink PREFIX "")
# 构建输出目录 = 部署目录 (同 office_runtime, 防 .so 双份致 GetRuntimeDir 错位)
set_target_properties(ImpressLink PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${IMPRESSLINK_DEPLOY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${IMPRESSLINK_DEPLOY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${IMPRESSLINK_DEPLOY_DIR}")
# 平台公共层 (link_platform 接口/link_utils/common 平台实现; common 链
# OfficeRuntime -> DT_NEEDED=office_runtime.so 运行时经 RUNPATH 解析, 经验 31)
target_link_libraries(ImpressLink PRIVATE common)
target_include_directories(ImpressLink PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../office_runtime")
target_compile_options(ImpressLink PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_include_directories(ImpressLink PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${CMAKE_CURRENT_SOURCE_DIR}/../include") # UNO API 头

# Linux: UNO SDK 平台宏 (SAL_UNX/UNX/LINUX/GCC, 与 calclink 一致)
if(UNIX AND NOT APPLE)
    target_compile_definitions(ImpressLink PRIVATE SAL_UNX UNX LINUX GCC)
    # UNO 库互相引用未定义符号, 需要放宽链接检查
    target_link_libraries(ImpressLink PRIVATE
        -Wl,--allow-shlib-undefined
        -Wl,-export-dynamic
        Threads::Threads rt dl X11::X11 X11::Xext)
    # 组件自包含: RUNPATH 指向部署目录(program), 并显式链接 LO 自带的 libstdc++
    # (GLIBCXX 版本高于系统); 使任何进程 dlopen impresslink.so 都不依赖 LD_LIBRARY_PATH
    target_link_libraries(ImpressLink PRIVATE
        "-Wl,-rpath,${IMPRESSLINK_DEPLOY_DIR}"
        "${IMPRESSLINK_DEPLOY_DIR}/libstdc++.so.6")
    # 运行时 UNO 库 (libuno_*.so.3) 在 office/program, 由宿主进程的
    # LD_LIBRARY_PATH 提供 (与 NovaLibreOfficePlayer/run.sh 同一模式)
endif()

add_custom_command(TARGET ImpressLink POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ImpressLink> "${IMPRESSLINK_DEPLOY_DIR}/"
    COMMENT "Deploying impresslink to ${IMPRESSLINK_DEPLOY_DIR}")
