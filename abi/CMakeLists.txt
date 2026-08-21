# C ABI 导出层 (header-only): abi.h (LINK_API 宏 + 统一 C ABI 接口声明 + 共享回调类型/
# ABI 契约) + session_registry.h (会话生命周期守卫)。
# INTERFACE 库: 无编译产物, 仅提供 include 路径给各 link。
add_library(abi INTERFACE)
target_include_directories(abi INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
