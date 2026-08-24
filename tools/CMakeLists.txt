# tools/CMakeLists.txt — Linux 探针编译 (作为根 CMake 的子模块)
#
# 由根 CMakeLists.txt 的条件构建控制: -DBUILD_TOOLS=ON 时进入。
# 探针链接部署后的 .so (calclink.so/impresslink.so/office_runtime.so/writerlink.so),
# 模拟 NovaPlayerDemo 外部调用方, 测真实 ABI。编译发生在 deploy 之后。
#
# 用法:
#   cmake -S . -B build -DBUILD_TOOLS=ON -DLIBREOFFICE_SDK_ROOT=/path/to/office/sdk
#   cmake --build build --target probes
#   或单探针: cmake --build build --target impress_nextpage_probe

if(WIN32)
    return()
endif()

# ---- 部署目录推导 (复用根 CMake 的 LIBREOFFICE_SDK_ROOT) ----
get_filename_component(_LO_SDK_PARENT "${LIBREOFFICE_SDK_ROOT}/.." ABSOLUTE)
set(DEPLOY_DIR "${_LO_SDK_PARENT}/program")
set(SDK_LIB_DIR "${LIBREOFFICE_SDK_ROOT}/lib")

# ---- NovaPlayer 根目录 (ffmpeg/SDL2 头) ----
get_filename_component(NOVAPLAYER_DLL_DIRECTORY "${LIBREOFFICE_SDK_ROOT}/../.." ABSOLUTE)
set(FFMPEG_INC_DIR "${NOVAPLAYER_DLL_DIRECTORY}/../include/ffmpeg")
set(SDL2_INC_DIR "${NOVAPLAYER_DLL_DIRECTORY}/../include/SDL2")

# ---- 探针公共头路径 (仓库相对, 消除硬编码 /home/hido/...) ----
# NLO_ROOT: tools 的父目录 (NovaLibreOfficePlayer); 兼容两种配置方式 —
# 从 NovaLibreOfficePlayer 根单独配置 (-DBUILD_TOOLS=ON) 或统一树 (NovaPlayerTools)。
get_filename_component(NLO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(TOOLS_INCLUDE_DIRS
    "${NLO_ROOT}"                 # abi/abi.h (统一 C ABI 接口)
    "${NLO_ROOT}/base"        # link_utils.h/log.h/office_paths.h
    "${NLO_ROOT}/runtime"      # runtime.h
    "${NLO_ROOT}/third_party/libreoffice"
    "${NLO_ROOT}/third_party")    # spdlog/scope_guard

# ---- 探针公共编译定义 ----
set(TOOLS_DEFINES SAL_UNX UNX LINUX GCC)

# ---- 素材路径 (运行时通过 argv 传入) ----
set(TEST_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data"
    CACHE PATH "Test data directory for probes")

# ============================================================
# 链接组函数 (替代 build_probes.sh 的 LINK 数组)
# ============================================================

# --- calc 组: 链接 calclink.so ---
function(add_calc_tools NAME)
    add_executable(${NAME} ${ARGN})
    target_include_directories(${NAME} PRIVATE ${TOOLS_INCLUDE_DIRS})
    target_compile_definitions(${NAME} PRIVATE ${TOOLS_DEFINES})
    target_link_libraries(${NAME} PRIVATE
        "${DEPLOY_DIR}/calclink.so"
        "${DEPLOY_DIR}/libstdc++.so.6"
        -Wl,-rpath,${DEPLOY_DIR}
        Threads::Threads)
endfunction()

# --- impress 组: 链接 impresslink.so + X11 ---
function(add_impress_tools NAME)
    add_executable(${NAME} ${ARGN})
    target_include_directories(${NAME} PRIVATE ${TOOLS_INCLUDE_DIRS})
    target_compile_definitions(${NAME} PRIVATE ${TOOLS_DEFINES})
    target_link_libraries(${NAME} PRIVATE
        "${DEPLOY_DIR}/impresslink.so"
        "${DEPLOY_DIR}/libstdc++.so.6"
        X11::X11 X11::Xext
        -Wl,-rpath,${DEPLOY_DIR})
endfunction()

# --- multi 组: 链接 calclink.so + impresslink.so + X11 ---
function(add_multi_tools NAME)
    add_executable(${NAME} ${ARGN})
    target_include_directories(${NAME} PRIVATE ${TOOLS_INCLUDE_DIRS})
    target_compile_definitions(${NAME} PRIVATE ${TOOLS_DEFINES})
    target_link_libraries(${NAME} PRIVATE
        "${DEPLOY_DIR}/calclink.so"
        "${DEPLOY_DIR}/impresslink.so"
        "${DEPLOY_DIR}/libstdc++.so.6"
        X11::X11 X11::Xext
        -Wl,-rpath,${DEPLOY_DIR}
        Threads::Threads)
endfunction()

# --- office 组: 链接 office_runtime.so + UNO SDK 库 ---
# 注意: office_runtime.h 引用了 <com/sun/star/...> UNO 头, 需 SDK include
function(add_office_tools NAME)
    add_executable(${NAME} ${ARGN})
    target_include_directories(${NAME} PRIVATE
        ${TOOLS_INCLUDE_DIRS}
        "${LIBREOFFICE_SDK_ROOT}/include")  # UNO 头 (com/sun/star/...)
    target_compile_definitions(${NAME} PRIVATE ${TOOLS_DEFINES})
    target_link_libraries(${NAME} PRIVATE
        "${DEPLOY_DIR}/office_runtime.so"
        "${DEPLOY_DIR}/libstdc++.so.6"
        "${SDK_LIB_DIR}/libuno_cppu.so"
        "${SDK_LIB_DIR}/libuno_cppuhelpergcc3.so"
        "${SDK_LIB_DIR}/libuno_sal.so"
        -Wl,-rpath,${DEPLOY_DIR}:${SDK_LIB_DIR}
        Threads::Threads rt dl X11::X11 X11::Xext)
endfunction()

# --- writer 组: 链接 writerlink.so ---
function(add_writer_tools NAME)
    add_executable(${NAME} ${ARGN})
    target_include_directories(${NAME} PRIVATE ${TOOLS_INCLUDE_DIRS})
    target_compile_definitions(${NAME} PRIVATE ${TOOLS_DEFINES})
    target_link_libraries(${NAME} PRIVATE
        "${DEPLOY_DIR}/writerlink.so"
        "${DEPLOY_DIR}/office_runtime.so"  # KernelHost 实现在 runtime (ld 不自动拉 DT_NEEDED)
        "${DEPLOY_DIR}/libstdc++.so.6"
        -Wl,-rpath,${DEPLOY_DIR})
endfunction()

# --- engine 组: ffplay 嵌入引擎 (需 gcc 编译 C 源 + ffmpeg/SDL2) ---
function(add_engine_tools NAME)
    # 编译 C 源 (ffplay_embed.c + cmdutils.c, 含 C 关键字 'class', 必须 gcc)
    set(FFPLAY_COMPAT_DIR "${NLO_ROOT}/runtime/ffplay/compat")
    set(COMPAT_OBJS)
    foreach(cs_src ffplay_embed.c cmdutils.c)
        string(REGEX REPLACE "\\.c$" "" cs_base "${cs_src}")
        set(co "${CMAKE_CURRENT_BINARY_DIR}/${cs_base}.o")
        add_custom_command(OUTPUT ${co}
            COMMAND "${CMAKE_C_COMPILER}" -O2 -DFFPLAY_EMBED -D_GNU_SOURCE
                -include /usr/include/time.h
                -I${FFPLAY_COMPAT_DIR}
                -I${FFPLAY_COMPAT_DIR}/compat
                -I${FFMPEG_INC_DIR}
                -I${FFMPEG_INC_DIR}/libavutil
                -I${SDL2_INC_DIR}
                -c ${FFPLAY_COMPAT_DIR}/${cs_src} -o ${co}
            DEPENDS ${FFPLAY_COMPAT_DIR}/${cs_src}
            COMMENT "Compiling ${cs_src} (gcc, FFPLAY_EMBED)")
        list(APPEND COMPAT_OBJS ${co})
    endforeach()

    add_executable(${NAME} ${ARGN} ${COMPAT_OBJS})
    target_include_directories(${NAME} PRIVATE
        ${TOOLS_INCLUDE_DIRS}
        ${FFPLAY_COMPAT_DIR}
        ${FFMPEG_INC_DIR}
        ${FFMPEG_INC_DIR}/libavutil
        ${SDL2_INC_DIR})
    target_compile_definitions(${NAME} PRIVATE ${TOOLS_DEFINES})
    # FFPLAY_EMBED + _GNU_SOURCE 会覆盖 time.h 符号, 需预包含系统 time.h
    target_compile_options(${NAME} PRIVATE "-include;/usr/include/time.h")
    target_link_libraries(${NAME} PRIVATE
        "${NOVAPLAYER_DLL_DIRECTORY}/libavformat.so.58"
        "${NOVAPLAYER_DLL_DIRECTORY}/libavcodec.so.58"
        "${NOVAPLAYER_DLL_DIRECTORY}/libavutil.so.56"
        "${NOVAPLAYER_DLL_DIRECTORY}/libswscale.so.5"
        "${NOVAPLAYER_DLL_DIRECTORY}/libswresample.so.3"
        "${NOVAPLAYER_DLL_DIRECTORY}/libavdevice.so.58"
        "${NOVAPLAYER_DLL_DIRECTORY}/libavfilter.so.7"
        "${NOVAPLAYER_DLL_DIRECTORY}/libpostproc.so.55"
        "${NOVAPLAYER_DLL_DIRECTORY}/libSDL2-2.0.so.0"
        -Wl,-rpath,${NOVAPLAYER_DLL_DIRECTORY}
        X11::X11 pthread m)
endfunction()

# --- dlopen 组: 仅链接 dl (word_core_probe 用) ---
function(add_dlopen_tools NAME)
    add_executable(${NAME} ${ARGN})
    target_include_directories(${NAME} PRIVATE ${TOOLS_INCLUDE_DIRS})
    target_compile_definitions(${NAME} PRIVATE ${TOOLS_DEFINES})
    target_link_libraries(${NAME} PRIVATE dl)
endfunction()

# ============================================================
# 探针登记 (target 名 = 探针原名, 不加 _tools 后缀)
# ============================================================
# 分组规则:
#   calc       -> add_calc_tools
#   impress    -> add_impress_tools
#   impress_multi -> add_multi_tools
#   office     -> add_office_tools
#   xvfb_stress -> add_office_tools (含 X11)
#   writer     -> add_writer_tools
#   engine     -> add_engine_tools
#   attack_new -> add_multi_tools
#   dlopen     -> add_dlopen_tools

# ---- 承重回归探针 (HANDOFF 1.2 登记 9 个) ----
add_impress_tools(impress_nextpage_probe linux/impress_nextpage_probe.cpp)
add_multi_tools(impress_multi_probe linux/impress_multi_probe.cpp)
add_impress_tools(media_green_probe linux/media_green_probe.cpp)
add_office_tools(ffplay_inject_probe linux/ffplay_inject_probe.cpp)
add_engine_tools(ffplay_engine_probe linux/ffplay_engine_probe.cpp)
add_office_tools(xvfb_stress_probe linux/xvfb_stress_probe.cpp)
add_office_tools(pdf_render_probe linux/pdf_render_probe.cpp)
add_writer_tools(writer_probe linux/writer_probe.cpp)
add_dlopen_tools(word_core_probe linux/word_core_probe.cpp)

# ---- 攻击探针 (attack_new 组) ----
add_multi_tools(attack_uaf_probe linux/attack_uaf_probe.cpp)
add_multi_tools(attack_resize_probe linux/attack_resize_probe.cpp)
add_multi_tools(attack_pagenav_probe linux/attack_pagenav_probe.cpp)
add_multi_tools(attack_mute_teardown_probe linux/attack_mute_teardown_probe.cpp)
add_multi_tools(attack_lock_inversion linux/attack_lock_inversion.cpp)
add_multi_tools(attack_cb_join_self linux/attack_cb_join_self.cpp)
add_multi_tools(attack_race_abi linux/attack_race_abi.cpp)
add_multi_tools(attack_abi_sequence linux/attack_abi_sequence.cpp)
add_multi_tools(attack_cb_reenter linux/attack_cb_reenter.cpp)
add_multi_tools(attack_cb_destroy linux/attack_cb_destroy.cpp)
add_multi_tools(attack_shm_race_probe linux/attack_shm_race_probe.cpp)
add_multi_tools(attack_bootlock_storm_probe linux/attack_bootlock_storm_probe.cpp)
add_multi_tools(attack_deep_impress_probe linux/attack_deep_impress_probe.cpp)
add_multi_tools(attack_triple_storm_probe linux/attack_triple_storm_probe.cpp)
add_multi_tools(attack_fork_bomb_probe linux/attack_fork_bomb_probe.cpp)
add_multi_tools(attack_ultimate_storm_probe linux/attack_ultimate_storm_probe.cpp)
add_multi_tools(attack_combo_storm_probe linux/attack_combo_storm_probe.cpp)

# ---- 攻击探针 (其他组) ----
add_multi_tools(attack_race_probe linux/attack_race_probe.cpp)
add_multi_tools(attack_resource_probe linux/attack_resource_probe.cpp)
add_multi_tools(attack_protocol_probe linux/attack_protocol_probe.cpp)
add_multi_tools(attack_boundary_probe linux/attack_boundary_probe.cpp)
add_multi_tools(attack_deadlock_probe linux/attack_deadlock_probe.cpp)
add_multi_tools(attack_exhaustion_probe linux/attack_exhaustion_probe.cpp)
add_multi_tools(attack_state_corruption_probe linux/attack_state_corruption_probe.cpp)
add_multi_tools(attack_cross_process_probe linux/attack_cross_process_probe.cpp)

# ---- 辅助探针 ----
add_engine_tools(ffplay_window_size_probe linux/ffplay_window_size_probe.cpp)
add_multi_tools(ui_1px_probe linux/ui_1px_probe.cpp)
add_multi_tools(bleed_probe linux/bleed_probe.cpp)
target_link_libraries(bleed_probe PRIVATE dl) # dlsym 动态加载 libXcomposite (无 dev 包)
add_multi_tools(composite_redirect_probe linux/composite_redirect_probe.cpp)
target_link_libraries(composite_redirect_probe PRIVATE dl X11::Xext) # dlsym + XShm
add_office_tools(minimal_load_probe linux/minimal_load_probe.cpp)
add_impress_tools(iframe_probe linux/iframe_probe.cpp)
add_impress_tools(mute_isolation_probe linux/mute_isolation_probe.cpp)

# lockfile 探针: 文档锁文件缺陷复现/验证 (引用 tools/data/locks 真实残留锁)。
# 注入 LOCKS_DATA_DIR 宏 = 源码仓库备份锁目录, 探针运行时按文档 basename 装机同名锁。
add_office_tools(lockfile_probe linux/lockfile_probe.cpp)
add_office_tools(lockfile_fix_probe linux/lockfile_fix_probe.cpp)
foreach(_lockp lockfile_probe lockfile_fix_probe)
    target_compile_definitions(${_lockp} PRIVATE
        LOCKS_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/locks")
endforeach()

# ---- 旧探针 (过时, 保留不删, 标记 OBSOLETE) ----
add_calc_tools(static_race_probe linux/static_race_probe.cpp)
set_target_properties(static_race_probe PROPERTIES
    COMPILE_FLAGS "-DOBSOLETE_PROBE")
add_calc_tools(shm_leak_probe linux/shm_leak_probe.cpp)
set_target_properties(shm_leak_probe PROPERTIES
    COMPILE_FLAGS "-DOBSOLETE_PROBE")
add_calc_tools(uno_null_probe linux/uno_null_probe.cpp)
set_target_properties(uno_null_probe PROPERTIES
    COMPILE_FLAGS "-DOBSOLETE_PROBE")

# ============================================================
# 自定义 target
# ============================================================

# probes target: 编译所有探针
add_custom_target(probes ALL
    COMMENT "All probes built (requires deploy done)")

add_dependencies(probes
    impress_nextpage_probe
    impress_multi_probe
    media_green_probe
    ffplay_inject_probe
    ffplay_engine_probe
    xvfb_stress_probe
    pdf_render_probe
    writer_probe
    word_core_probe
    attack_uaf_probe
    attack_resize_probe
    attack_pagenav_probe
    attack_mute_teardown_probe
    attack_lock_inversion
    attack_cb_join_self
    attack_race_abi
    attack_abi_sequence
    attack_cb_reenter
    attack_cb_destroy
    attack_shm_race_probe
    attack_bootlock_storm_probe
    attack_deep_impress_probe
    attack_triple_storm_probe
    attack_fork_bomb_probe
    attack_ultimate_storm_probe
    attack_combo_storm_probe
    attack_race_probe
    attack_resource_probe
    attack_protocol_probe
    attack_boundary_probe
    attack_deadlock_probe
    attack_exhaustion_probe
    attack_state_corruption_probe
    attack_cross_process_probe
    ffplay_window_size_probe
    ui_1px_probe
    minimal_load_probe
    iframe_probe
    static_race_probe
    shm_leak_probe
    uno_null_probe)