cmake_minimum_required(VERSION 3.13)

set(CMAKE_CXX_STANDARD 17)

project(
    NovaLibreOfficePlayer
    VERSION 0.1
    LANGUAGES CXX
)

# MSVC 变量须在 project() 之后才定义 (编译器检测); 之前判断恒真会把 -fPIC
# 传给 MSVC (D9002 警告, 2026-08-18 探针回归时发现)
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

# office_runtime (共享内核/Xvfb/slot/ffplay 单测) 与 X11 为 Linux 专属;
# Windows 走每 session 独立 soffice, 不编 office_runtime (经验 39)。
# 双平台共享模块统一列出, 平台差异收敛到下方 if(NOT WIN32) 一块。
if(NOT WIN32)
    find_package(PkgConfig REQUIRED)
    find_package(Threads REQUIRED)
    find_package(X11 REQUIRED COMPONENTS Xfixes Xinerama Xmu)
    add_subdirectory(office_runtime)   # OfficeRuntime -> office_runtime.so (+ ffplay + 单测)
endif()

add_subdirectory(common)     # 平台公共层 (LinkPlatform 接口/工具/Xvfb/win 实现)
add_subdirectory(calc)       # CalcLink      -> calclink.so
add_subdirectory(impress)    # ImpressLink   -> impresslink.so
add_subdirectory(writer)     # WriterLink    -> writerlink.so (自治 PDF 位图, 经验 38)
