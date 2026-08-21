cmake_minimum_required(VERSION 3.16)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(
    NovaLibreOfficePlayer # libreoffice-link
    VERSION 0.1
    LANGUAGES CXX C
)

# 单一构建树: 依赖发现统一在根完成, 子树只声明 target (经验 31)。
# LibreOffice::Sdk / Threads 双平台都需要; X11 仅 Linux (runtime 专属,
# Windows 走独立 soffice 不编, 经验 39)。
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/base/cmake")
find_package(LibreOfficeSDK REQUIRED)
find_package(Threads REQUIRED)

# MSVC 变量须在 project() 之后才定义 (编译器检测); 之前判断恒真会把 -fPIC
# 传给 MSVC (D9002 警告)
if(NOT MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
endif()

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR "In-source builds not allowed.")
endif()

if(MSVC)
    # UTF-8 源码 (中文注释/字符串; 防 GBK 码页误读致注释吞换行, C4819);
    # /EHsc 异常展开 (UNO 调用遍布 try/catch);
    # _CRT_SECURE_NO_WARNINGS (跨平台代码惯例 getenv/strtoul 等)
    add_compile_options(/utf-8 /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# runtime (共享内核/Xvfb/slot/ffplay 单测) 与 X11 为 Linux 专属;
# Windows 走每 session 独立 soffice, 不编 runtime (经验 39)。
# 双平台共享模块统一列出, 平台差异收敛到下方 if(NOT WIN32) 一块。
if(NOT WIN32)
    find_package(X11 REQUIRED COMPONENTS Xfixes Xinerama Xmu)
    add_subdirectory(runtime)      # OfficeRuntime -> office_runtime.so (+ ffplay + 单测)
endif()

add_subdirectory(abi)        # C ABI 导出层 (header-only: LINK_API 宏/session_registry)
add_subdirectory(base)       # 基础工具 (link_utils/log/office_paths)
add_subdirectory(platform)   # 平台抽象 (LinkPlatform 接口/linux/xvfb + windows/win 实现)
add_subdirectory(frame)      # 帧泵 (FramePump, 经验 42)
add_subdirectory(calc)       # CalcLink      -> calclink.so
add_subdirectory(impress)    # ImpressLink   -> impresslink.so
add_subdirectory(writer)     # WriterLink    -> writerlink.so (自治 PDF 位图, 经验 38)

option(BUILD_TOOLS "Build probes (Linux only, requires deploy done)" OFF)
if(BUILD_TOOLS)
    add_subdirectory(tools)
endif()
