# ==============================================================================
# TaskflowLite (tfl) - 高性能、现代 C++23 任务并行编程库
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

# C++23 硬性要求
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# IDE 友好：导出 compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ccache / sccache 加速
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER   "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache detected: enabled")
endif()

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

# MSVC 全局 UTF-8 源码解析
# Why: 项目源码/头文件含中文注释且为 UTF-8。MSVC 在中文系统(代码页 936)下
#      默认按 GBK 解析, 会把多字节中文连同后续的 }/*/ 等一起吃掉, 报出一堆
#      假的 C2059/C2143/C2086(根因是 C4819)。全局加 /utf-8 后, 所有目标
#      (含第三方 Taskflow benchmark、未来新增目标)都按 UTF-8 解析, 一劳永逸。
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ==============================================================================
# 依赖获取：GitHub 访问方式（供 benchmarks 子目录克隆 Taskflow/CGraph 用）
# 放在根 CMake 顶层 => cmake-gui 第一次 Configure 就能看到并修改这些变量，
# 不必等 benchmarks 子目录跑到克隆那一步。
# ==============================================================================
# GitHub 访问方式（cmake-gui 里是下拉框）：
#   direct        = 直连 github.com
#   ghfast.top 等 = 加速镜像（国内连不上 github 时选；改默认值可永久生效）
set(TFL_GITHUB_MIRROR "direct" CACHE STRING
    "GitHub 访问方式: direct / ghfast.top / gh-proxy.com / ghproxy.net")
set_property(CACHE TFL_GITHUB_MIRROR PROPERTY STRINGS
    direct ghfast.top gh-proxy.com ghproxy.net)

# 进阶覆盖：
#   TFL_GITHUB_PREFIX 非空时优先于下拉选择（自定义镜像前缀，拼在 github URL 前）
#   TFL_GIT_PROXY     git 走本地 HTTP(S) 代理，如 http://127.0.0.1:7890
set(TFL_GITHUB_PREFIX "" CACHE STRING "自定义 GitHub 前缀(非空时覆盖 TFL_GITHUB_MIRROR)")
set(TFL_GIT_PROXY     "" CACHE STRING "git HTTP(S) proxy, e.g. http://127.0.0.1:7890")

# partial clone：省流量但需 promisor 二次拉取；镜像下若卡 promisor 就关掉它（OFF）
option(TFL_PARTIAL_CLONE "用 --filter=blob:none 部分克隆" ON)
option(TFL_UPDATE_DEPS   "重新 configure 时把已克隆依赖更新到分支最新提交" OFF)

# 解析出最终生效前缀：自定义 PREFIX > 下拉镜像 > 直连
if(TFL_GITHUB_PREFIX)
    set(TFL_EFFECTIVE_PREFIX "${TFL_GITHUB_PREFIX}")
elseif(TFL_GITHUB_MIRROR STREQUAL "direct")
    set(TFL_EFFECTIVE_PREFIX "")
else()
    set(TFL_EFFECTIVE_PREFIX "https://${TFL_GITHUB_MIRROR}/")
endif()
message(STATUS "[deps] GitHub 访问: "
    "mirror=${TFL_GITHUB_MIRROR}, prefix='${TFL_EFFECTIVE_PREFIX}', proxy='${TFL_GIT_PROXY}'")

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

option(TASKFLOWLITE_BUILD_EXAMPLES "Build examples" ${TFL_IS_TOP_LEVEL})
option(TASKFLOWLITE_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(TASKFLOWLITE_BUILD_TESTS "Build unit tests" ${TFL_IS_TOP_LEVEL})

# Sanitizer 三态开关：OFF / ASAN / TSAN (CI 核心)
# Why: ASan 和 TSan 不能同时启用 (运行时互斥)，且各自捕获完全不同类别的 bug，
#      所以必须独立成 job 才能完整覆盖。
set(TASKFLOWLITE_SANITIZER "OFF" CACHE STRING "Sanitizer mode: OFF, ASAN, TSAN")
set_property(CACHE TASKFLOWLITE_SANITIZER PROPERTY STRINGS OFF ASAN TSAN)

# 向后兼容旧的布尔开关 TASKFLOWLITE_ENABLE_SANITIZER
# Why: 避免本地脚本和外部 superbuild 突然炸掉，给一个 deprecation 过渡期。
if(DEFINED TASKFLOWLITE_ENABLE_SANITIZER)
    message(WARNING
        "TASKFLOWLITE_ENABLE_SANITIZER is deprecated. "
        "Use -DTASKFLOWLITE_SANITIZER=ASAN|TSAN|OFF instead.")
    if(TASKFLOWLITE_ENABLE_SANITIZER)
        if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
            set(TASKFLOWLITE_SANITIZER "ASAN")
        else()
            set(TASKFLOWLITE_SANITIZER "TSAN")
        endif()
    endif()
endif()

# 规范化用户输入 (允许 asan / Asan / ASAN 都生效)
string(TOUPPER "${TASKFLOWLITE_SANITIZER}" TASKFLOWLITE_SANITIZER)
if(NOT TASKFLOWLITE_SANITIZER MATCHES "^(OFF|ASAN|TSAN)$")
    message(FATAL_ERROR
        "TASKFLOWLITE_SANITIZER must be one of: OFF, ASAN, TSAN "
        "(got: ${TASKFLOWLITE_SANITIZER})")
endif()

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

    # 2. Sanitizer 注入逻辑 (按值分发：ASAN / TSAN)
    # Why: ASan 抓 use-after-free / heap-buffer-overflow / stack-overflow，
    #      TSan 抓 data race / lock order inversion。两者必须分开 job 跑，
    #      运行时不能共存。
    if(TASKFLOWLITE_SANITIZER STREQUAL "ASAN")
        message(STATUS "🛠️  Sanitizer: AddressSanitizer (ASan) enabled.")

        if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
            target_compile_options(tfl_internal_flags INTERFACE /fsanitize=address /Zi)

            # 解决 MSVC 下 ASan 的常见冲突：移除基本运行时检查
            string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")

            # 解决 LNK4300: Debug 模式下开启 ASan 必须禁用增量链接
            target_link_options(tfl_internal_flags INTERFACE
                $<$<CONFIG:Debug>:/INCREMENTAL:NO>)

            # 解决 LNK4302: Release 等非 Debug 模式下开启 ASan 必须生成调试符号
            target_link_options(tfl_internal_flags INTERFACE
                $<$<NOT:$<CONFIG:Debug>>:/DEBUG>
                $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
                $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>
            )

        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            # Unix (含 clang-cl) 上 ASan 顺便带 UBSan，两者运行时兼容
            target_compile_options(tfl_internal_flags INTERFACE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
                -fno-sanitize-recover=all
                -g)
            target_link_options(tfl_internal_flags INTERFACE
                -fsanitize=address,undefined)
        endif()

    elseif(TASKFLOWLITE_SANITIZER STREQUAL "TSAN")
        message(STATUS "🧵 Sanitizer: ThreadSanitizer (TSan) enabled.")

        if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
            message(FATAL_ERROR
                "ThreadSanitizer is not supported on MSVC. "
                "Use TASKFLOWLITE_SANITIZER=ASAN on Windows, "
                "or switch to clang-cl / a Unix host for TSan coverage.")
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(tfl_internal_flags INTERFACE
                -fsanitize=thread,undefined
                -fno-omit-frame-pointer
                -g
                -O1
                -Wno-tsan) # 不支持独立 atomic_thread_fence，压制已知警告
            target_link_options(tfl_internal_flags INTERFACE
                -fsanitize=thread,undefined)
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
    message(STATUS "Sanitizer:        ${TASKFLOWLITE_SANITIZER}")
    message(STATUS "==================================")
    message(STATUS "")
endif()