# 部署目录由 SDK 根统一推导 (单一构建树内不用 CACHE 变量, 避免缓存旧值)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    get_filename_component(_LO_SDK_PARENT "${LIBREOFFICE_SDK_ROOT}/.." ABSOLUTE)
    set(WRITERLINK_DEPLOY_DIR "${_LO_SDK_PARENT}/program")
else()
    set(WRITERLINK_DEPLOY_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../NovaPlayer/bin/office/program")
endif()

# writerlink = 自治 PDF 位图管线 (经验 38): 无平台层 — 无窗口/无抓帧/无 slot,
# 不实例化 LinkPlatform。链接: Linux 唯一依赖 runtime (内核/BootLock/
# OfficeLog); Windows 链 base 取共享工具 (OfficeLog/GetLinkDir/BootstrapSession,
# 静态库按需取对象, 平台层代码不落地), 链接形态同 impress (rpath + 显式
# libstdc++, 任何进程 dlopen 免 LD_LIBRARY_PATH)
add_library(WriterLink SHARED
    export.cpp
    session.cpp
    md5.cpp)
set_target_properties(WriterLink PROPERTIES OUTPUT_NAME writerlink PREFIX "")
# 构建输出目录 = 部署目录 (单副本, 防 .so 双份致 GetRuntimeDir 错位, 经验 33)
set_target_properties(WriterLink PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${WRITERLINK_DEPLOY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${WRITERLINK_DEPLOY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${WRITERLINK_DEPLOY_DIR}")

target_include_directories(WriterLink PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${CMAKE_CURRENT_SOURCE_DIR}/../runtime"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libreoffice"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
# UNO 头 + CPPU_ENV 宏 (Windows 侧同样需要)
target_link_libraries(WriterLink PRIVATE LibreOffice::Sdk)

# Windows: base 共享工具 + platform 提供 OfficeLog 实现 (win_office_log.cpp,
# 对偶 Linux 的 OfficeRuntime 角色) + frame (FramePump 实现, 对齐 Linux 链接).
# 平台层不实例化 LinkPlatform, 仅取 win_office_log.obj (静态库按需取对象).
if(WIN32)
    target_link_libraries(WriterLink PRIVATE link_include base platform)
endif()

if(UNIX AND NOT APPLE)
    target_compile_definitions(WriterLink PRIVATE SAL_UNX UNX LINUX GCC)
    # 共享内核/BootLock/OfficeLog (Linux 专属; Windows 侧会话内 bootstrap)
    target_link_libraries(WriterLink PRIVATE OfficeRuntime)
    # base (u2s/s2u/HideUiBlock/log + ABI 守卫 + FramePump; 与 calc/impress 对齐)
    target_link_libraries(WriterLink PRIVATE link_include base)
    target_link_libraries(WriterLink PRIVATE
        -Wl,--allow-shlib-undefined
        -Wl,--export-dynamic
        Threads::Threads rt dl)
    # RUNPATH 必须是 $ORIGIN (同目录) 而不是部署目录的绝对路径: 交付时整个 office/
    # 会被拷到 <bin>/office, 编译机路径随之下线, 下游 dlopen 就找不到
    # office_runtime.so —— DT_RUNPATH 不沿 dlopen 链继承, 宿主的 RPATH 救不了。
    # (同 impress/calc)
    target_link_libraries(WriterLink PRIVATE
        "-Wl,-rpath,\$ORIGIN"
        "${WRITERLINK_DEPLOY_DIR}/libstdc++.so.6")
    # 否则 CMake 把编译机的 office/program (OfficeRuntime 的目标目录) 与 office/sdk/lib
    # 也写进 RUNPATH, 同上属于交付后失效的路径
    set_target_properties(WriterLink PROPERTIES SKIP_BUILD_RPATH ON)
endif()

add_custom_command(TARGET WriterLink POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:WriterLink> "${WRITERLINK_DEPLOY_DIR}/"
    COMMENT "Deploying writerlink to ${WRITERLINK_DEPLOY_DIR}")
