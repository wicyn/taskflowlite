# ==============================================================================
# TaskflowLite (tfl) - 高性能、现代 C++20 任务并行编程库
# ==============================================================================

cmake_minimum_required(VERSION 3.21)

# 策略兼容
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)  # MSVC runtime library 控制
endif()
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)  # MSVC debug information format
endif()

# IDE 友好：启用解决方案文件夹分组
# Why: 子目录里 set_target_properties(... FOLDER ...) 才会生效；
#      VS 自带的 ALL_BUILD / ZERO_CHECK / INSTALL 也会自动归入 CMakePredefinedTargets，
#      顶层不再被一堆杂目标刷屏。设在根目录是最规范的位置。
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# C++20 硬性要求
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# IDE 友好：导出 compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ccache / sccache 加速
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "[deps] ccache enabled")
endif()

# ==============================================================================
# 魔法开始：从 C++ 源码中逆向提取版本号 (Source-Driven Versioning)
# ==============================================================================
include(cmake/TflVersion.cmake)

project(TaskflowLite
    VERSION "${TFL_VERSION}"
    LANGUAGES CXX
    DESCRIPTION "A high-performance, header-only, C++20 task scheduling engine"
    HOMEPAGE_URL "https://github.com/wicyn/taskflowlite"
)

# ==============================================================================
# 依赖获取：GitHub 访问方式
# 供 benchmarks 子目录克隆 Taskflow、test 子目录下载 Catch2 amalgamated 等复用。
# 放在根 CMake 顶层 => cmake-gui 第一次 Configure 就能看到并修改这些变量，
# 不必等子目录跑到下载那一步。
# ==============================================================================
include(cmake/TflDependencies.cmake)

# ==============================================================================
# Build options & dependencies
# ==============================================================================
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(TFL_IS_TOP_LEVEL ON)
else()
    set(TFL_IS_TOP_LEVEL OFF)
endif()

# ------------------------------------------------------------------------------
# Banner (top-level build only)
# ------------------------------------------------------------------------------
if(TFL_IS_TOP_LEVEL)
    message("***********************************")
    message("*                                 *")
    message("*      _____ _____ _              *")
    message("*     |_   _|  ___| |             *")
    message("*       | | | |_  | |             *")
    message("*       | | |  _| | |___          *")
    message("*       |_| |_|   |_____|         *")
    message("*                                 *")
    message("*      TaskflowLite  by wicyn     *")
    message("*                                 *")
    message("***********************************")
endif()

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

# --- 常用：构建哪些组件 ---
option(TFL_BUILD_EXAMPLES "[build] Build examples" ${TFL_IS_TOP_LEVEL})
option(TFL_BUILD_BENCHMARKS "[build] Build benchmarks (clones Taskflow baseline over network)" OFF)
# 默认 OFF（与 benchmarks 对称的 opt-in 闸门）：
# tests 子目录在本开关为 ON 时才被加入；其内部"本地缺失则下载 Catch2"的逻辑
# 也只在那时才会执行。这样默认 configure 不会进入 test、不会触发任何下载，
# 想跑测试时显式 -DTFL_BUILD_TESTS=ON（CI 已带该参数）。
option(TFL_BUILD_TESTS "[build] Build unit tests (downloads Catch2 if not present locally)" OFF)

option(TFL_BUILD_DOCS "[build] Generate API documentation with Doxygen" OFF)
include(cmake/TflBuildOptions.cmake)
include(GNUInstallDirs)

# ==============================================================================
# 核心库目标定义 (Header-Only Target)
# ==============================================================================
add_library(taskflowlite INTERFACE)
add_library(TaskflowLite::taskflowlite ALIAS taskflowlite)

target_compile_features(taskflowlite INTERFACE cxx_std_20)
target_link_libraries(taskflowlite INTERFACE Threads::Threads)

# GCC/Clang 上需显式链接 libatomic 以支持 128-bit DWCAS (FreeStack128)
# 注意: macOS/AppleClang 无独立 libatomic，原子操作由编译器运行时提供，
#       若链接 -latomic 会报 "ld: library 'atomic' not found"，故排除 APPLE。
if(NOT MSVC AND NOT APPLE)
    target_link_libraries(taskflowlite INTERFACE atomic)
endif()

# 跨平台护航：解决 MSVC 下的 UTF-8 和标准兼容性问题
target_compile_options(taskflowlite INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>
)

# 纯静态包含目录：对使用者极其友好
target_include_directories(taskflowlite INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

# ==============================================================================
# 子目录
# ==============================================================================
if(TFL_BUILD_TESTS)
    enable_testing()
endif()
if(TFL_BUILD_DOCS)
    add_subdirectory(documentation)
endif()

# 将内部编译 flag 传递给子模块使用 (确保子模块内的可执行文件/测试链接 tfl_internal_flags)
if(TFL_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(TFL_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(TFL_BUILD_TESTS)
    add_subdirectory(test)
endif()

# ==============================================================================
# 安装与包导出规则 (Install & Export)
# ==============================================================================
include(CMakePackageConfigHelpers)

install(DIRECTORY taskflowlite
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.hpp"
)

install(TARGETS taskflowlite
    EXPORT TaskflowLiteTargets
)

set(TFL_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/TaskflowLite")

install(EXPORT TaskflowLiteTargets
    FILE TaskflowLiteTargets.cmake
    NAMESPACE TaskflowLite::
    DESTINATION "${TFL_INSTALL_CMAKEDIR}"
)

configure_package_config_file(
    "${CMAKE_CURRENT_LIST_DIR}/cmake/TaskflowLiteConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/TaskflowLiteConfig.cmake"
    INSTALL_DESTINATION "${TFL_INSTALL_CMAKEDIR}"
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/TaskflowLiteConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/TaskflowLiteConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/TaskflowLiteConfigVersion.cmake"
    DESTINATION "${TFL_INSTALL_CMAKEDIR}"
)

# ==============================================================================
# 打印配置信息
# ==============================================================================
if(TFL_IS_TOP_LEVEL)
    message(STATUS "-- TaskflowLite Configuration -----------------")
    message(STATUS "  Version       : ${PROJECT_VERSION}")
    message(STATUS "  Compiler      : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    message(STATUS "  C++ Standard  : C++${CMAKE_CXX_STANDARD}")
    message(STATUS "  Examples      : ${TFL_BUILD_EXAMPLES}")
    message(STATUS "  Tests         : ${TFL_BUILD_TESTS}")
    message(STATUS "  Benchmarks    : ${TFL_BUILD_BENCHMARKS}")
    message(STATUS "  Docs          : ${TFL_BUILD_DOCS}")
    message(STATUS "  Native arch   : ${TFL_NATIVE_ARCH}")
    message(STATUS "  Sanitizer     : ${TFL_SANITIZER}")
    message(STATUS "-----------------------------------------------")
endif()
