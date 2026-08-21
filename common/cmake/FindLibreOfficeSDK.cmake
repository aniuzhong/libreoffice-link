# 默认定位 NovaPlayer 部署目录下的 LO SDK。
# Linux 布局: bin_<arch>_<sys>/office (kylin/uos/linux), Windows 为 bin/office。
# 默认推导依赖相对路径 (common/cmake 上溯至 NovaPlayerTools 父级), 机器布局不符时
# 须显式 -DLIBREOFFICE_SDK_ROOT=xxx 覆盖 (当前机器 NovaPlayer 在 NovaPlayerProject/ 下,
# 默认推导失效, 以显式传参为准)。
# 缓存自愈: 缓存值失效 (目录被移动/删除) 时重新推导 (模块从 calc/cmake 移到
# common/cmake 后, 旧缓存路径指向已删目录)。
if(NOT LIBREOFFICE_SDK_ROOT OR NOT EXISTS "${LIBREOFFICE_SDK_ROOT}")
    set(_NOVA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../NovaPlayer")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(_NOVA_ARCH aarch64)
        else()
            set(_NOVA_ARCH x86_64)
        endif()
        foreach(_NOVA_SYS kylin uos linux)
            set(_NOVA_CAND "${_NOVA_ROOT}/bin_${_NOVA_ARCH}_${_NOVA_SYS}/office/sdk")
            if(EXISTS "${_NOVA_CAND}")
                set(LIBREOFFICE_SDK_ROOT "${_NOVA_CAND}" CACHE PATH "" FORCE)
                break()
            endif()
        endforeach()
        if(NOT LIBREOFFICE_SDK_ROOT)
            set(LIBREOFFICE_SDK_ROOT "${_NOVA_ROOT}/bin/office/sdk" CACHE PATH "" FORCE)
        endif()
    else()
        set(LIBREOFFICE_SDK_ROOT "${_NOVA_ROOT}/bin/office/sdk" CACHE PATH "" FORCE)
    endif()
endif()

if(NOT LIBREOFFICE_UNO_INCLUDE OR NOT EXISTS "${LIBREOFFICE_UNO_INCLUDE}")
    set(LIBREOFFICE_UNO_INCLUDE
        "${CMAKE_CURRENT_LIST_DIR}/../../include"
        CACHE PATH "Full UNO IDL header tree (com/sun/star/**)" FORCE)
endif()

get_filename_component(LIBREOFFICE_HOME "${LIBREOFFICE_SDK_ROOT}/.." ABSOLUTE)
set(LIBREOFFICE_PROGRAM_HOME "${LIBREOFFICE_HOME}/program")

foreach(_dir LIBREOFFICE_SDK_ROOT LIBREOFFICE_UNO_INCLUDE LIBREOFFICE_HOME LIBREOFFICE_PROGRAM_HOME)
    if(NOT EXISTS "${${_dir}}")
        message(FATAL_ERROR "FindLibreOfficeSDK: ${_dir}=${${_dir}} does not exist. "
                            "Pass -DLIBREOFFICE_SDK_ROOT=xxx / -DLIBREOFFICE_UNO_INCLUDE=xxx to override.")
    endif()
endforeach()

if(WIN32)
    set(_LO_IMPORT_LIBS icppu icppuhelper isal isalhelper ipurpenvhelper)
    set(_LO_DEFS)
else()
    set(_LO_IMPORT_LIBS uno_cppu uno_cppuhelpergcc3 uno_sal uno_salhelpergcc3 uno_purpenvhelpergcc3)
    set(_LO_DEFS CPPU_ENV=gcc3)
endif()

if(NOT TARGET LibreOffice::Sdk)
    add_library(LibreOffice::Sdk INTERFACE IMPORTED)
    set_target_properties(LibreOffice::Sdk PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${LIBREOFFICE_SDK_ROOT}/include;${LIBREOFFICE_UNO_INCLUDE}"
        INTERFACE_LINK_DIRECTORIES "${LIBREOFFICE_SDK_ROOT}/lib")
    target_link_libraries(LibreOffice::Sdk INTERFACE ${_LO_IMPORT_LIBS})
    target_compile_definitions(LibreOffice::Sdk INTERFACE
        "LIBREOFFICE_PROGRAM_HOME=\"${LIBREOFFICE_PROGRAM_HOME}\"")
    target_compile_definitions(LibreOffice::Sdk INTERFACE ${_LO_DEFS})
endif()
