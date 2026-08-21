# 部署目录由 SDK 根统一推导 (单一构建树内不再用 CACHE 变量, 避免缓存旧值)
get_filename_component(OR_DEPLOY_DIR "${LIBREOFFICE_SDK_ROOT}/.." ABSOLUTE)
set(OR_DEPLOY_DIR "${OR_DEPLOY_DIR}/program")

add_library(OfficeRuntime SHARED
    office_runtime.cpp)
set_target_properties(OfficeRuntime PROPERTIES OUTPUT_NAME office_runtime PREFIX "")
# 构建输出目录 = 部署目录 (office/program): 顶层全局输出目录 (bin 根) 会致
# .so 双份存在, demo 的 LD_LIBRARY_PATH=$CURDIR 优先加载 bin 根版本 ->
# dladdr(GetRuntimeDir) 错位 -> UNO_PATH 指向 bin 根 -> bootstrap rc=4 (经验 31)
set_target_properties(OfficeRuntime PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${OR_DEPLOY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${OR_DEPLOY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${OR_DEPLOY_DIR}")

target_link_libraries(OfficeRuntime PRIVATE LibreOffice::Sdk)
target_include_directories(OfficeRuntime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${CMAKE_CURRENT_SOURCE_DIR}/../include") # spdlog / UNO IDL

# Linux: UNO SDK 平台宏
target_compile_definitions(OfficeRuntime PRIVATE SAL_UNX UNX LINUX GCC)

# UNO 库互相引用未定义符号, 需要放宽链接检查
target_link_libraries(OfficeRuntime PRIVATE
    -Wl,--allow-shlib-undefined
    -Wl,-export-dynamic
    Threads::Threads rt dl X11::X11)
# 部署在 office/program 内, 同目录解析 UNO 运行库
set_target_properties(OfficeRuntime PROPERTIES
    INSTALL_RPATH "\$ORIGIN"
    BUILD_RPATH "\$ORIGIN")

# 部署到 office/program (与 calclink.so 同处)
add_custom_command(TARGET OfficeRuntime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:OfficeRuntime> "${OR_DEPLOY_DIR}/"
    # user 模板 (仓库 templates/user 净化 xcu, 双平台共享; Linux
    # SeedKernelProfile / Windows 平台层 seed 消费; office/user 已退役)
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/../templates" "${OR_DEPLOY_DIR}/templates"
    COMMENT "Deploying office_runtime.so + user template to ${OR_DEPLOY_DIR}")

# ---- 媒体后端子模块 (ffplay.so: 给 soffice 换播放后端, SDK 模式组件) ----
# 子模块归属本目录 (伞形不感知 office_runtime 内部结构, 经验 31)
add_subdirectory(ffplay)

# ---- 单测 (统一树内编译, 不部署; 跑法见 HANDOFF 1.3) ----
add_executable(office_runtime_test
    office_runtime_test.cpp)
set_target_properties(office_runtime_test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(office_runtime_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    "${CMAKE_CURRENT_SOURCE_DIR}/../include")
target_link_libraries(office_runtime_test PRIVATE OfficeRuntime)
target_link_libraries(office_runtime_test PRIVATE LibreOffice::Sdk) # UNO 头 + CPPU_ENV
target_compile_definitions(office_runtime_test PRIVATE SAL_UNX UNX LINUX GCC)
target_link_libraries(office_runtime_test PRIVATE
    -Wl,--allow-shlib-undefined
    Threads::Threads rt dl X11::X11)
