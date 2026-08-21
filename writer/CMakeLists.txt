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

# Windows: base 共享工具 (OfficeLog/GetLinkDir/BootstrapSession;
# 平台层不实例化, 见文件头注释)
if(WIN32)
    target_link_libraries(WriterLink PRIVATE base)
endif()

if(UNIX AND NOT APPLE)
    target_compile_definitions(WriterLink PRIVATE SAL_UNX UNX LINUX GCC)
    # 共享内核/BootLock/OfficeLog (Linux 专属; Windows 侧会话内 bootstrap)
    target_link_libraries(WriterLink PRIVATE OfficeRuntime)
    # abi (导出守卫) + base (u2s/s2u/HideUiBlock/log 等; 与 calc/impress 对齐)
    # + frame (FramePump)
    target_link_libraries(WriterLink PRIVATE abi base frame)
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
