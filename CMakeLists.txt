# ==============================================================================
# TaskflowLite (tfl) - 高性能、现代 C++23 任务并行编程库
# ==============================================================================

cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

# 使用提取到的版本号定义项目！
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
option(TASKFLOWLITE_BUILD_BENCHMARKS "Build TaskflowLite benchmarks" OFF)
option(TASKFLOWLITE_BUILD_TESTS "Build TaskflowLite unit tests" ${TFL_IS_TOP_LEVEL})

# ==============================================================================
# 核心库目标定义 (Header-Only Target)
# ==============================================================================
add_library(taskflowlite INTERFACE)
add_library(TaskflowLite::taskflowlite ALIAS taskflowlite)

target_compile_features(taskflowlite INTERFACE cxx_std_23)
target_link_libraries(taskflowlite INTERFACE Threads::Threads)

# 跨平台护航 (已修复 MSVC 版本判断的生成器表达式错误)
target_compile_options(taskflowlite INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<VERSION_LESS:$<CXX_COMPILER_VERSION>,19.40>>:/std:c++latest>
)

# 纯静态包含目录：只需暴露项目根目录即可，不再有 generated 文件夹！
target_include_directories(taskflowlite INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

# ==============================================================================
# 内部编译选项 (仅对 Examples / Benchmarks / Tests 生效)
# ==============================================================================
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
            /wd4819 
            $<$<CONFIG:Release>:/O2 /Ob2>
        )
    endif()
endif()

# ==============================================================================
# 子目录
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
# ==============================================================================
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# 直接将整个纯净的 taskflowlite 源码文件夹安装到系统的 include 目录下
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
    message(STATUS "Build examples:   ${TASKFLOWLITE_BUILD_EXAMPLES}")
    message(STATUS "Build tests:      ${TASKFLOWLITE_BUILD_TESTS}")
    message(STATUS "Build benchmarks: ${TASKFLOWLITE_BUILD_BENCHMARKS}")
    message(STATUS "C++ Compiler:     ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    message(STATUS "C++ Standard:     ${CMAKE_CXX_STANDARD}")
    message(STATUS "==================================")
    message(STATUS "")
endif()