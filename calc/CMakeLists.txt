# 部署目录由 SDK 根统一推导 (单一构建树内不用 CACHE 变量, 避免缓存旧值)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    get_filename_component(_LO_SDK_PARENT "${LIBREOFFICE_SDK_ROOT}/.." ABSOLUTE)
    set(CALCLINK_DEPLOY_DIR "${_LO_SDK_PARENT}/program")
else()
    set(CALCLINK_DEPLOY_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../NovaPlayer/bin/office/program")
endif()

add_library(CalcLink SHARED
    export.cpp
    session.cpp)
set_target_properties(CalcLink PROPERTIES OUTPUT_NAME calclink PREFIX "")
# 构建输出目录 = 部署目录 (同 runtime, 防 .so 双份致 GetRuntimeDir 错位)
set_target_properties(CalcLink PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CALCLINK_DEPLOY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CALCLINK_DEPLOY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CALCLINK_DEPLOY_DIR}")
# abi (导出宏/守卫) + base (link_utils/log) + platform (LinkPlatform 实现) +
# frame (FramePump); platform 链 OfficeRuntime -> DT_NEEDED=office_runtime.so
# 运行时经 RUNPATH 解析, 经验 31
target_link_libraries(CalcLink PRIVATE link_include base platform)
target_include_directories(CalcLink PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../runtime")
target_compile_options(CalcLink PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_include_directories(CalcLink PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libreoffice"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")

if(UNIX AND NOT APPLE)
    target_compile_definitions(CalcLink PRIVATE SAL_UNX UNX LINUX GCC)
    # UNO 库互相引用未定义符号, 需要放宽链接检查
    target_link_libraries(CalcLink PRIVATE
        -Wl,--allow-shlib-undefined
        -Wl,-export-dynamic
        Threads::Threads rt dl X11::X11 X11::Xext)
    # 组件自包含: RUNPATH 指向同目录 ($ORIGIN = 部署后的 office/program), 并显式链接
    # LO 自带的 libstdc++ (GLIBCXX 版本高于系统); 使任何进程 dlopen calclink.so
    # 都不依赖 LD_LIBRARY_PATH。
    # 不能写部署目录的绝对路径 (${CALCLINK_DEPLOY_DIR}): 交付时整个 office/ 会被拷到
    # <bin>/office, 编译机路径随之下线, 下游 dlopen 就找不到 office_runtime.so —— 而
    # DT_RUNPATH 不沿 dlopen 链继承, 宿主进程自己的 RPATH 救不了。
    # UNO 运行库 (libuno_*.so.3) 同在 office/program, $ORIGIN 一并覆盖。
    target_link_libraries(CalcLink PRIVATE
        "-Wl,-rpath,\$ORIGIN"
        "${CALCLINK_DEPLOY_DIR}/libstdc++.so.6")
    # 否则 CMake 把编译机的 office/sdk/lib (LibreOffice::Sdk 的链接目录) 也写进 RUNPATH,
    # 同上属于交付后失效的路径
    set_target_properties(CalcLink PROPERTIES SKIP_BUILD_RPATH ON)
endif()

add_custom_command(TARGET CalcLink POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:CalcLink> "${CALCLINK_DEPLOY_DIR}/"
    COMMENT "Deploying calclink to ${CALCLINK_DEPLOY_DIR}")
