# 部署目录由 SDK 根统一推导 (单一构建树内不用 CACHE 变量, 避免缓存旧值)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    get_filename_component(_LO_SDK_PARENT "${LIBREOFFICE_SDK_ROOT}/.." ABSOLUTE)
    set(CALCLINK_DEPLOY_DIR "${_LO_SDK_PARENT}/program")
else()
    set(CALCLINK_DEPLOY_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../NovaPlayer/bin/office/program")
endif()

add_library(CalcLink SHARED
    calclink.cpp
    calc_session.cpp)
set_target_properties(CalcLink PROPERTIES OUTPUT_NAME calclink PREFIX "")
# 构建输出目录 = 部署目录 (同 office_runtime, 防 .so 双份致 GetRuntimeDir 错位)
set_target_properties(CalcLink PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CALCLINK_DEPLOY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CALCLINK_DEPLOY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CALCLINK_DEPLOY_DIR}")
# 平台公共层 (link_platform 接口/link_utils/common 平台实现; common 链
# OfficeRuntime -> DT_NEEDED=office_runtime.so 运行时经 RUNPATH 解析, 经验 31)
target_link_libraries(CalcLink PRIVATE common)
target_include_directories(CalcLink PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../office_runtime")
target_compile_options(CalcLink PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_include_directories(CalcLink PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")

if(UNIX AND NOT APPLE)
    target_compile_definitions(CalcLink PRIVATE SAL_UNX UNX LINUX GCC)
    # UNO 库互相引用未定义符号, 需要放宽链接检查
    target_link_libraries(CalcLink PRIVATE
        -Wl,--allow-shlib-undefined
        -Wl,-export-dynamic
        Threads::Threads rt dl X11::X11 X11::Xext)
    # 组件自包含: RUNPATH 指向部署目录(program), 并显式链接 LO 自带的 libstdc++
    # (GLIBCXX 版本高于系统); 使任何进程 dlopen calclink.so 都不依赖 LD_LIBRARY_PATH
    target_link_libraries(CalcLink PRIVATE
        "-Wl,-rpath,${CALCLINK_DEPLOY_DIR}"
        "${CALCLINK_DEPLOY_DIR}/libstdc++.so.6")
    # 运行时 UNO 库 (libuno_*.so.3) 在 office/program, 由宿主进程的
    # LD_LIBRARY_PATH 提供 (与 NovaLibreOfficePlayer/run.sh 同一模式)
endif()

add_custom_command(TARGET CalcLink POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:CalcLink> "${CALCLINK_DEPLOY_DIR}/"
    COMMENT "Deploying calclink to ${CALCLINK_DEPLOY_DIR}")
