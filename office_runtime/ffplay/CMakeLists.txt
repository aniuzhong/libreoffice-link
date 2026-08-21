# ---- ffmpeg SDK (NovaPlayer 自带, 与库版本精确匹配 58.76.100/4.4.1) ----
# 统一树内 NOVAPLAYER_DLL_DIRECTORY 由顶层推导 (bin_<arch>_<sys>);
# 独立使用时回退到 SDK 根推导。
if(NOT NOVAPLAYER_DLL_DIRECTORY)
    get_filename_component(NOVAPLAYER_DLL_DIRECTORY "${LIBREOFFICE_SDK_ROOT}/../.." ABSOLUTE)
endif()
set(FFMPEG_INC_DIR "${NOVAPLAYER_DLL_DIRECTORY}/../include/ffmpeg") # NovaPlayer/include/ffmpeg
if(NOT EXISTS "${FFMPEG_INC_DIR}/libavformat/avformat.h")
    message(FATAL_ERROR "FFplay: ffmpeg headers not found at ${FFMPEG_INC_DIR}")
endif()

# ---- 部署目录 (与 links 同处: office/program) ----
get_filename_component(FFPLAY_DEPLOY_DIR "${LIBREOFFICE_SDK_ROOT}/.." ABSOLUTE)
set(FFPLAY_DEPLOY_DIR "${FFPLAY_DEPLOY_DIR}/program")

add_library(FFplay SHARED
    ffplay_manager.cxx
    ffplay_player.cxx
    ffplay_window.cxx
    # ffplay 嵌入引擎: 上游定制 ffplay.c + cmdutils (FFPLAY_EMBED, 经验 34)
    compat/ffplay_embed.c
    compat/cmdutils.c)
# 产物名 ffplay.so (unorc 注册), PREFIX "" 去掉 Linux 的 lib 前缀
set_target_properties(FFplay PROPERTIES OUTPUT_NAME ffplay PREFIX "")
# 构建输出目录 = 部署目录 (同 office_runtime, 防 .so 双份致 GetRuntimeDir 错位)
set_target_properties(FFplay PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${FFPLAY_DEPLOY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${FFPLAY_DEPLOY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${FFPLAY_DEPLOY_DIR}")

# UNO 类型 (LibreOffice::Sdk 自带 include 与 CPPU_ENV 宏) + ffmpeg 头 + X11
target_link_libraries(FFplay PRIVATE LibreOffice::Sdk)
target_include_directories(FFplay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/compat          # 引擎头 (ffplay_engine.h/config.h/cmdutils.h)
    ${CMAKE_CURRENT_SOURCE_DIR}/compat/compat   # va_copy.h (上游 compat 惯例)
    ${FFMPEG_INC_DIR}
    "${FFMPEG_INC_DIR}/libavutil"               # 内部头相对 include (libm.h 等)
    "${NOVAPLAYER_DLL_DIRECTORY}/../include/SDL2"  # Nova SDL2 头 (2.32.8, 与库匹配)
    "${CMAKE_CURRENT_SOURCE_DIR}/../../include")   # spdlog (header-only, ffplay_log.h 依赖)
target_compile_definitions(FFplay PRIVATE SAL_UNX UNX LINUX GCC)
# 嵌入模式 + kylin 兼容: sys/time.h 不含 struct tm, 预包含 time.h (经验 34)
target_compile_definitions(FFplay PRIVATE FFPLAY_EMBED _GNU_SOURCE)
target_compile_options(FFplay PRIVATE "-include;/usr/include/time.h")

# 组件在 soffice.bin (共享内核进程) 内被 dlopen:
#   - UNO 符号 (libuno_*.so.3) 由内核进程已加载库解析 -> 放宽链接检查
#   - ffmpeg 库在 bin_<arch>_<sys> 根, 即部署目录 (office/program) 上两级
#     -> RUNPATH $ORIGIN/../.. 自足; 运行时 soffice 脚本自设的
#     LD_LIBRARY_PATH 也含 bin 根 (双保险, 2026-08-14 审查修正)
#   - 引擎 (ffplay_embed.c, 经验 34) 真实调用 ffmpeg/SDL2, 由 NEEDED 带入 soffice 进程
target_link_libraries(FFplay PRIVATE
    -Wl,--allow-shlib-undefined
    -Wl,-export-dynamic
    "-Wl,-rpath,\$ORIGIN:\$ORIGIN/..:\$ORIGIN/../.."
    -Wl,--no-as-needed # SDL2/avfilter/avdevice/postproc 的 NEEDED 须全部保留,
                       # 缺一个即 soffice dlopen undefined symbol (经验 34)
    "${NOVAPLAYER_DLL_DIRECTORY}/libavformat.so.58"
    "${NOVAPLAYER_DLL_DIRECTORY}/libavcodec.so.58"
    "${NOVAPLAYER_DLL_DIRECTORY}/libavutil.so.56"
    "${NOVAPLAYER_DLL_DIRECTORY}/libswscale.so.5"
    "${NOVAPLAYER_DLL_DIRECTORY}/libswresample.so.3"
    "${NOVAPLAYER_DLL_DIRECTORY}/libavfilter.so.7"   # 引擎滤镜路径 (ffplay.c CONFIG_AVFILTER)
    "${NOVAPLAYER_DLL_DIRECTORY}/libavdevice.so.58"  # avdevice_register_all
    "${NOVAPLAYER_DLL_DIRECTORY}/libpostproc.so.55"  # cmdutils 版本打印
    "${NOVAPLAYER_DLL_DIRECTORY}/libSDL2-2.0.so.0"   # 引擎渲染/音频 (ffplay_embed, 经验 34)
    -Wl,--as-needed
    Threads::Threads X11::X11)

# 部署 ffplay.so + ffplay.rdb (unorc 已注册, 见 office/program/unorc)
add_custom_command(TARGET FFplay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:FFplay> "${FFPLAY_DEPLOY_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/ffplay.rdb" "${FFPLAY_DEPLOY_DIR}/"
    COMMENT "Deploying ffplay.so + ffplay.rdb to ${FFPLAY_DEPLOY_DIR}")
