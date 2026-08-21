cmake_minimum_required(VERSION 3.13)

set(CMAKE_CXX_STANDARD 17)

# ---- Project (构建伞: 旧方案 target + 新链路四件套) ----
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

# ---- Include guards ----
if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(
        FATAL_ERROR
        "In-source builds not allowed."
    )
endif()

if(MSVC)
    # UTF-8 源码 (中文注释/字符串; 防 GBK 码页误读致注释吞换行, C4819);
    # /EHsc 异常展开 (UNO 调用遍布 try/catch);
    # _CRT_SECURE_NO_WARNINGS (跨平台代码惯例 getenv/strtoul 等)
    add_compile_options(/utf-8 /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# ============================================================
# Windows 分支: 每 session 独立 soffice + 独立桌面, 不参与 office_runtime
# (X11 共享内核/slot/ffplay 为 Linux 专属)。
# 与 Linux 同一棵树的同一批模块 CMakeLists — 依赖图双平台一致, 防再次漂移
# (经验 31 单一构建树哲学; 2026-08-17 重构后曾因无 Windows 入口而无法编译)。
# 部署目录: 各模块 CMakeLists 按平台推导 (Windows: NovaPlayer/bin/office/program)。
# 构建: cmake -S NovaLibreOfficePlayer -B <build_dir> -A x64
# 旧独立进程方案 (source/ + exe + vcxproj) 已于 2026-08-17 全平台清理。
# ============================================================
if(WIN32)
    add_subdirectory(common)
    add_subdirectory(calc)
    add_subdirectory(impress)
    add_subdirectory(writer)

    # user 模板 (仓库 templates/user 净化 xcu, 双平台共享, 经验 40):
    # 与 Linux 统一 —— 模板随部署目录 (office/program/templates) 由打包流程
    # (CopyFile.bat 等) 拷贝, 不再由 CMake 硬编码路径部署 (2026-08-18 定调)。
    # WindowsPlatform::PrepareEnvironment seed 从部署副本复制; 开发环境需
    # 手动同步 (或由 NovaPlayer 打包脚本保证)。
    return()
endif()

# ============================================================
# 构建伞组成 (单一构建树):
#   OfficeRuntime / CalcLink / ImpressLink / WriterLink / FFplay
#       (office_runtime.so + calclink.so + impresslink.so + writerlink.so + ffplay.so)
# 统一构建入口: NovaPlayerTools/build_in_linux.sh (ABI 同步由 cmake 依赖图承接)
# 旧独立进程方案 (source/ + NovaLibreOfficePlayerDeprecated/exe + 旧 Manager)
# 已于 2026-08-17 清理: 零实例化 (PptCoreExport 全走新链/图片模式), git 可恢复
# ============================================================

# ---- Dependency check
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(X11 REQUIRED COMPONENTS Xfixes Xinerama Xmu)

# ============================================================
# 新链路 (共享内核方案; 各子目录自带 CMakeLists)
#   依赖图: CalcLink/ImpressLink -> OfficeRuntime (target 链接, ABI 同步机制化)
#   部署: 各 POST_BUILD -> bin_<arch>_<sys>/office/program (LIBREOFFICE_SDK_ROOT 推导)
# ============================================================
add_subdirectory(office_runtime)       # OfficeRuntime -> office_runtime.so (+ ffplay 子模块 + 单测)
add_subdirectory(common)               # common        -> 平台公共层 (接口/工具/Xvfb/win 实现)
add_subdirectory(calc)                 # CalcLink      -> calclink.so
add_subdirectory(impress)              # ImpressLink   -> impresslink.so
add_subdirectory(writer)               # WriterLink    -> writerlink.so (自治 PDF 位图, 经验 38)
