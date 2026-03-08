# ==============================================================================
# TaskflowLite (tfl) - 高性能、现代 C++23 任务并行编程库
# ==============================================================================

cmake_minimum_required(VERSION 3.21)

# 设置 C++ 策略（推荐，显式指定以避免隐式依赖）
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)  # MSVC 运行时库通过 generator expression 设置
endif()

# 定义项目名称、版本号和使用的语言
project(TaskflowLite 
    VERSION 1.0.0 
    LANGUAGES CXX
    DESCRIPTION "A high-performance, header-only, C++23 task scheduling engine"
    HOMEPAGE_URL "https://github.com/WiCyn/TaskflowLite"
)

# ==============================================================================
# MSVC C++23 支持
# ==============================================================================
if(MSVC)
    add_compile_options(/std:c++latest)
endif()

# ==============================================================================
# 构建选项 (Options)
# ==============================================================================
# 如果当前项目是顶层项目（而不是被别人 add_subdirectory 引入），默认开启示例构建
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(TFL_IS_TOP_LEVEL ON)
else()
    set(TFL_IS_TOP_LEVEL OFF)
endif()

# 查找 Threads 库（必需）
find_package(Threads REQUIRED)

# 默认开启测试和示例（顶层项目）
option(TASKFLOWLITE_BUILD_EXAMPLES "Build TaskflowLite examples" ${TFL_IS_TOP_LEVEL})
option(TASKFLOWLITE_BUILD_BENCHMARKS "Build TaskflowLite benchmarks" OFF)
option(TASKFLOWLITE_BUILD_TESTS "Build TaskflowLite unit tests" ${TFL_IS_TOP_LEVEL})

# ==============================================================================
# 核心库目标定义 (Header-Only Target)
# ==============================================================================
# 纯头文件库必须使用 INTERFACE 关键字
add_library(taskflowlite INTERFACE)

# 创建一个 Alias，方便在本项目内部的其他目标（如 examples）中像外部用户一样链接它
add_library(TaskflowLite::taskflowlite ALIAS taskflowlite)

# 强制要求 C++23 标准
target_compile_features(taskflowlite INTERFACE cxx_std_23)

# 设置包含目录（Include Directories）
# $<BUILD_INTERFACE:...> : 库自身编译（或内部示例编译）时使用的路径
# $<INSTALL_INTERFACE:...> : 库被安装后，外部用户引用时使用的相对路径
target_include_directories(taskflowlite INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

# ==============================================================================
# 内部编译选项 (仅对 Examples / Benchmarks 生效，不污染外部使用者)
# ==============================================================================
# Why: 绝对不要把 -O3 或 -Wall 绑定到 INTERFACE 库上，否则所有引入你库的人都会被迫使用这些参数。
# 我们专门创建一个内部的 INTERFACE 目标，用来给测试和示例统一注入优化参数。
if(TFL_IS_TOP_LEVEL)
    add_library(tfl_internal_flags INTERFACE)
    
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(tfl_internal_flags INTERFACE
            -Wall -Wextra -Wpedantic
            $<$<CONFIG:Release>:-O3 -march=native>
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
        target_compile_options(tfl_internal_flags INTERFACE
            /W4 /Zc:preprocessor
            $<$<CONFIG:Release>:/O2 /Ob2>
        )
    endif()
endif()

# ==============================================================================
# 子目录 (示例、基准测试)
# ==============================================================================
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
# 这部分允许别人通过 find_package(TaskflowLite) 来优雅地使用你的库
# ==============================================================================
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# 1. 安装头文件目录
install(DIRECTORY taskflowlite
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

# 2. 安装库目标
install(TARGETS taskflowlite
    EXPORT TaskflowLiteTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# 3. 导出 Targets 文件
set(TFL_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/TaskflowLite")

install(EXPORT TaskflowLiteTargets
    FILE TaskflowLiteTargets.cmake
    NAMESPACE TaskflowLite::
    DESTINATION ${TFL_INSTALL_CMAKEDIR}
)

# 4. 生成并安装 Config 和 Version 文件
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
    message(STATUS "Build examples:  ${TASKFLOWLITE_BUILD_EXAMPLES}")
    message(STATUS "Build tests:     ${TASKFLOWLITE_BUILD_TESTS}")
    message(STATUS "Build benchmarks: ${TASKFLOWLITE_BUILD_BENCHMARKS}")
    message(STATUS "C++ Compiler:    ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    message(STATUS "C++ Standard:    ${CMAKE_CXX_STANDARD}")
    message(STATUS "==================================")
    message(STATUS "")
endif()