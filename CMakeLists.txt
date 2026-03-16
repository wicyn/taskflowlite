# ==============================================================================
# TaskflowLite (tfl) - 高性能、现代 C++23 任务并行编程库
# ==============================================================================

cmake_minimum_required(VERSION 3.21)

# 允许使用 CMAKE_MSVC_RUNTIME_LIBRARY 来控制 MSVC 运行时 (MT/MD)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 强制开启编译命令导出，方便 Clangd/VSCode 等现代化 IDE 自动提供代码提示
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ==============================================================================
# 魔法开始：从 C++ 源码中逆向提取版本号 (Source-Driven Versioning)
# ==============================================================================
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/taskflowlite/taskflowlite.hpp" _TFL_HPP_CONTENT)

string(REGEX MATCH "#define TASKFLOWLITE_VERSION_MAJOR ([0-9]+)" _ "${_TFL_HPP_CONTENT}")
set(_TFL_MAJOR ${CMAKE_MATCH_1})

string(REGEX MATCH "#define TASKFLOWLITE_VERSION_MINOR ([0-9]+)" _ "${_TFL_HPP_CONTENT}")
set(_TFL_MINOR ${CMAKE_MATCH_1})

string(REGEX MATCH "#define TASKFLOWLITE_VERSION_PATCH ([0-9]+)" _ "${_TFL_HPP_CONTENT}")
set(_TFL_PATCH ${CMAKE_MATCH_1})

project(TaskflowLite
    VERSION "${_TFL_MAJOR}.${_TFL_MINOR}.${_TFL_PATCH}"
    LANGUAGES CXX
    DESCRIPTION "A high-performance, header-only, C++23 task scheduling engine"
    HOMEPAGE_URL "https://github.com/wicyn/taskflowlite"
)

# ==============================================================================
# 构建选项与依赖
# ==============================================================================
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(TFL_IS_TOP_LEVEL ON)
else()
    set(TFL_IS_TOP_LEVEL OFF)
endif()

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

option(TASKFLOWLITE_BUILD_EXAMPLES "Build TaskflowLite examples" ${TFL_IS_TOP_LEVEL})
option(TASKFLOWLITE_BUILD_BENCHMARKS "Build TaskflowLite benchmarks" ON)
option(TASKFLOWLITE_BUILD_TESTS "Build TaskflowLite unit tests" ${TFL_IS_TOP_LEVEL})

# 智能 Sanitizer 总开关 (CI 核心)
option(TASKFLOWLITE_ENABLE_SANITIZER "Auto-enable ASan on Windows, and TSan+UBSan on Unix" OFF)

# ==============================================================================
# 核心库目标定义 (Header-Only Target)
# ==============================================================================
add_library(taskflowlite INTERFACE)
add_library(TaskflowLite::taskflowlite ALIAS taskflowlite)

target_compile_features(taskflowlite INTERFACE cxx_std_23)
target_link_libraries(taskflowlite INTERFACE Threads::Threads)

# 跨平台护航：解决 MSVC 下的 UTF-8 和标准兼容性问题
target_compile_options(taskflowlite INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,19.40>>:/std:c++latest>
)

# 纯静态包含目录：对使用者极其友好
target_include_directories(taskflowlite INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

# ==============================================================================
# 内部编译选项 (仅对 Examples / Benchmarks / Tests 生效)
# ==============================================================================
if(TFL_IS_TOP_LEVEL)
    add_library(tfl_internal_flags INTERFACE)

    # 1. 基础编译警告和优化配置
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(tfl_internal_flags INTERFACE
            -Wall -Wextra -Wpedantic
            -Wno-unused-parameter
            -Wno-unused-variable
            $<$<CONFIG:Release>:-O3 -march=native>
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
        target_compile_options(tfl_internal_flags INTERFACE
            /W4 /Zc:preprocessor
            /wd4819 # 屏蔽编码警告
            /wd4324 # 屏蔽 C4324: 由于 alignas 导致的结构体对齐填充警告 (重要机制，无需警告)
            /wd4100 # 屏蔽 C4100: 未引用的参数 (方便编写占位闭包)
            /wd4189 # 屏蔽 C4189: 局部变量已初始化但不引用
            $<$<CONFIG:Release>:/O2 /Ob2>
        )
    endif()

    # 2. 智能 Sanitizer 注入逻辑 (按操作系统分发)
    if(TASKFLOWLITE_ENABLE_SANITIZER)
        if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
            message(STATUS "🛠️ Auto-Sanitizer: Windows detected. Enabling AddressSanitizer (ASan).")
            if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
                target_compile_options(tfl_internal_flags INTERFACE /fsanitize=address /Zi)

                # 解决 MSVC 下 ASan 的常见冲突：移除基本运行时检查
                string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")

                # 解决 LNK4300: Debug 模式下开启 ASan 必须禁用增量链接
                target_link_options(tfl_internal_flags INTERFACE $<$<CONFIG:Debug>:/INCREMENTAL:NO>)

                # 解决 LNK4302: Release 等非 Debug 模式下开启 ASan 必须生成调试符号
                target_link_options(tfl_internal_flags INTERFACE
                    $<$<NOT:$<CONFIG:Debug>>:/DEBUG>
                    $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
                    $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>
                )

            elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang") # clang-cl
                target_compile_options(tfl_internal_flags INTERFACE -fsanitize=address -g -fno-omit-frame-pointer)
            endif()
        else()
            message(STATUS "🧵 Auto-Sanitizer: Unix-like OS detected. Enabling TSan + UBSan.")
            if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
                target_compile_options(tfl_internal_flags INTERFACE
                    -fsanitize=thread,undefined -g -fno-omit-frame-pointer)
                target_link_options(tfl_internal_flags INTERFACE
                    -fsanitize=thread,undefined)
            endif()
        endif()
    endif()
endif()

# ==============================================================================
# 子目录
# ==============================================================================
# 将内部编译 flag 传递给子模块使用 (确保子模块内的可执行文件/测试链接 tfl_internal_flags)
if(TASKFLOWLITE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(TASKFLOWLITE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(TASKFLOWLITE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# ==============================================================================
# 安装与包导出规则 (Install & Export)
# ==============================================================================
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY taskflowlite
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

install(TARGETS taskflowlite
    EXPORT TaskflowLiteTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

set(TFL_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/TaskflowLite")

install(EXPORT TaskflowLiteTargets
    FILE TaskflowLiteTargets.cmake
    NAMESPACE TaskflowLite::
    DESTINATION ${TFL_INSTALL_CMAKEDIR}
)

configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/cmake/TaskflowLiteConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/TaskflowLiteConfig.cmake"
    INSTALL_DESTINATION ${TFL_INSTALL_CMAKEDIR}
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/TaskflowLiteConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/TaskflowLiteConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/TaskflowLiteConfigVersion.cmake"
    DESTINATION ${TFL_INSTALL_CMAKEDIR}
)

# ==============================================================================
# 打印配置信息
# ==============================================================================
if(TFL_IS_TOP_LEVEL)
    message(STATUS "")
    message(STATUS "=== TaskflowLite Configuration ===")
    message(STATUS "Version:          ${PROJECT_VERSION}")
    message(STATUS "C++ Compiler:     ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    message(STATUS "C++ Standard:     ${CMAKE_CXX_STANDARD}")
    message(STATUS "Build examples:   ${TASKFLOWLITE_BUILD_EXAMPLES}")
    message(STATUS "Build tests:      ${TASKFLOWLITE_BUILD_TESTS}")
    message(STATUS "Build benchmarks: ${TASKFLOWLITE_BUILD_BENCHMARKS}")
    message(STATUS "Auto Sanitizer:   ${TASKFLOWLITE_ENABLE_SANITIZER}")
    message(STATUS "==================================")
    message(STATUS "")
endif()
