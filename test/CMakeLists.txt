# ============================================================================
#  TaskflowLite 单元测试 CMake —— Catch2 v3 amalgamated
# ============================================================================
#
#  两种构建模式：
#  ----------
#  1. TaskflowLiteTest（单体）       ：全部 test_*.cpp 编译进一个可执行文件，
#                                      一键跑全部测试。
#  2. tfl_test_<name>（单文件）     ：每个 test_*.cpp 编译成独立的可执行文件，
#                                      可单独生成、调试、运行。
#
#  设计要点：
#  ----------
#  - catch_amalgamated.cpp 编译为 OBJECT 库，一次编译、多方链接。
#  - 单文件目标默认 EXCLUDE_FROM_ALL，不增加默认构建时间。
#  - GLOB + CONFIGURE_DEPENDS 自动发现新增测试文件。
#  - 复用上层 TaskflowLite::taskflowlite 目标的编译选项。
#  - CTest 支持标签过滤（ctest -R tfl.flow）和单文件过滤（ctest -R tfl_test.flow）。
#  - 可选 ASan / TSan 支持。
#
#  使用方式：
#  ----------
#    # 单体模式（和以前一样）
#    ./TaskflowLiteTest                         # 跑全部
#    ./TaskflowLiteTest "[flow]"                # 仅跑 [flow] 标签
#    cmake --build . --target run_all_tests     # 一键构建+测试
#    cmake --build . --target run_flow_tests    # 仅跑 [flow]
#
#    # 单文件模式（新增）
#    cmake --build . --target tfl_test_queue    # 只编译队列测试
#    ./build/bin/tfl_test_queue -s              # 只跑队列测试
#    cmake --build . --target run_test_queue    # 一键编译+跑队列测试
#
#    # CTest 过滤
#    ctest -R "^tfl_test\\."                    # 跑全部单文件测试
#    ctest -R "^tfl_test\\.queue$"              # 只跑队列单文件测试
#    ctest -L perfile                           # 跑全部单文件测试（标签过滤）
# ============================================================================

cmake_minimum_required(VERSION 3.21)

# ----------------------------------------------------------------------------
#  兼容两种使用模式：
#  - 作为子目录被上层 add_subdirectory(test) 引入（推荐）：直接复用上层 target
#  - 作为独立项目顶层构建：自动配置最小依赖
# ----------------------------------------------------------------------------
if(NOT CMAKE_PROJECT_NAME)
    project(TaskflowLiteTest LANGUAGES CXX)
endif()

# ----------------------------------------------------------------------------
#  全局 C++ 标准
# ----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ----------------------------------------------------------------------------
#  Build type 默认值
# ----------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# ----------------------------------------------------------------------------
#  可选：Sanitizer 支持
# ----------------------------------------------------------------------------
option(TFL_TEST_ENABLE_ASAN "Enable AddressSanitizer for tests" OFF)
option(TFL_TEST_ENABLE_TSAN "Enable ThreadSanitizer for tests" OFF)

# ----------------------------------------------------------------------------
#  线程库（Catch2 / 测试代码 / TaskflowLite 都需要）
# ----------------------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ----------------------------------------------------------------------------
#  自动收集测试源（GLOB + CONFIGURE_DEPENDS 自动发现新增文件）
# ----------------------------------------------------------------------------
file(GLOB TFL_TEST_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp"
)

# Catch2 amalgamated 源文件（提供 main()）
set(CATCH2_SRC "${CMAKE_CURRENT_SOURCE_DIR}/catch_amalgamated.cpp")
if(NOT EXISTS "${CATCH2_SRC}")
    message(FATAL_ERROR
        "找不到 catch_amalgamated.cpp，请将 Catch2 amalgamated 头文件与源文件放到 ${CMAKE_CURRENT_SOURCE_DIR}/")
endif()

if(NOT TFL_TEST_SOURCES)
    message(FATAL_ERROR
        "未找到任何 test_*.cpp 文件于 ${CMAKE_CURRENT_SOURCE_DIR}/")
endif()

# ============================================================================
#  Catch2 amalgamated OBJECT 库 —— 编译一次，多方链接
#
#  Why OBJECT: catch_amalgamated.cpp 提供 main() + 全部 Catch2 实现。
#  编译为 OBJECT 库后，单体外挂二进制和 N 个单文件目标都链接同一份 .obj，
#  避免对 24 个目标各编译一遍 ~459KB 的 Catch2 源码。
# ============================================================================
add_library(tfl_catch2_main OBJECT "${CATCH2_SRC}")
target_include_directories(tfl_catch2_main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(tfl_catch2_main PRIVATE cxx_std_23)

if(MSVC)
    # /bigobj: catch_amalgamated.cpp 模板展开后 obj 段较大
    # /W3: 第三方库不需要 /W4 级别警告
    target_compile_options(tfl_catch2_main PRIVATE /W3 /utf-8 /bigobj)
else()
    # 抑制第三方库内的警告
    target_compile_options(tfl_catch2_main PRIVATE -w)
endif()

# ============================================================================
#  辅助函数: tfl_setup_test_target
#
#  把编译选项、include 路径、库链接、sanitizer 等所有共享设置封装在此，
#  单体外挂二进制和单文件目标都调用它，避免配置漂移。
#
#  用法:
#    add_executable(my_target ...)
#    tfl_setup_test_target(my_target)
# ============================================================================
function(tfl_setup_test_target _tgt)
    # 链接 Catch2 OBJECT 库
    target_link_libraries(${_tgt} PRIVATE tfl_catch2_main)

    # C++23
    target_compile_features(${_tgt} PRIVATE cxx_std_23)

    # 本目录 include（test_common.hpp / Catch2 头文件）
    target_include_directories(${_tgt} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # 链接 TaskflowLite 库（优先复用上层 target，不可用时回退到 include 路径）
    if(TARGET TaskflowLite::taskflowlite)
        target_link_libraries(${_tgt} PRIVATE
            TaskflowLite::taskflowlite
            Threads::Threads)
        if(TARGET tfl_internal_flags)
            target_link_libraries(${_tgt} PRIVATE tfl_internal_flags)
        endif()
    else()
        message(STATUS
            "TaskflowLite::taskflowlite 不可用，${_tgt} 回退到 include 路径模式")
        target_include_directories(${_tgt}
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_link_libraries(${_tgt} PRIVATE Threads::Threads)
    endif()

    # 编译选项
    if(MSVC)
        target_compile_options(${_tgt} PRIVATE
            /W4 /permissive- /Zc:preprocessor /utf-8
            $<$<CONFIG:Release>:/O2>)
    else()
        target_compile_options(${_tgt} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wno-unused-parameter
            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:Debug>:-O0 -g>)
        if(TFL_TEST_ENABLE_ASAN)
            target_compile_options(${_tgt} PRIVATE
                -fsanitize=address -fno-omit-frame-pointer)
            target_link_options(${_tgt} PRIVATE -fsanitize=address)
        endif()
        if(TFL_TEST_ENABLE_TSAN)
            target_compile_options(${_tgt} PRIVATE
                -fsanitize=thread -fno-omit-frame-pointer)
            target_link_options(${_tgt} PRIVATE -fsanitize=thread)
        endif()
    endif()

    # 输出目录统一到 build/bin
    set_target_properties(${_tgt} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endfunction()

# ============================================================================
#  单体外挂二进制: TaskflowLiteTest —— 全部 test_*.cpp 编译进一个可执行文件
# ============================================================================
add_executable(TaskflowLiteTest ${TFL_TEST_SOURCES})
tfl_setup_test_target(TaskflowLiteTest)

# 单体专有: /bigobj（多个 TEST_CASE 注册符号可能导致 obj 段超限）
if(MSVC)
    target_compile_options(TaskflowLiteTest PRIVATE /bigobj)
endif()

# IDE 源码分组
source_group("Tests" REGULAR_EXPRESSION ".*test_.*\\.cpp$")
source_group("Catch2" REGULAR_EXPRESSION ".*catch_amalgamated.*")

# ============================================================================
#  单文件目标: tfl_test_<name> —— 每个 test_*.cpp 一个独立可执行文件
#
#  默认 EXCLUDE_FROM_ALL: cmake --build . 不构建它们。
#  显式构建: cmake --build . --target tfl_test_queue
#  选项 TFL_TEST_PER_FILE_DEFAULT=ON 可包含在默认构建中。
# ============================================================================
option(TFL_TEST_PER_FILE_DEFAULT
    "Build per-file test executables as part of the default build" OFF)

foreach(_src ${TFL_TEST_SOURCES})
    get_filename_component(_full_name "${_src}" NAME_WE)       # e.g. test_queue
    string(REGEX REPLACE "^test_" "" _test_name "${_full_name}") # e.g. queue
    set(_target "tfl_test_${_test_name}")

    if(TFL_TEST_PER_FILE_DEFAULT)
        add_executable(${_target} "${_src}")
    else()
        add_executable(${_target} EXCLUDE_FROM_ALL "${_src}")
    endif()

    tfl_setup_test_target(${_target})

    # VS Solution Explorer 中折叠到 PerFileTests 文件夹
    set_target_properties(${_target} PROPERTIES FOLDER "PerFileTests")
endforeach()

# ============================================================================
#  CTest 注册
# ============================================================================
include(CTest)
enable_testing()

# --- 单体主测试（不变） ---
add_test(
    NAME TaskflowLiteTest
    COMMAND TaskflowLiteTest -s -d yes
    WORKING_DIRECTORY $<TARGET_FILE_DIR:TaskflowLiteTest>
)
set_tests_properties(TaskflowLiteTest PROPERTIES
    TIMEOUT 1500
    LABELS "unit;taskflowlite"
)

# --- 标签过滤子测试（不变） ---
set(TFL_TEST_TAGS
    flow task executor runtime async semaphore
    branch jump subflow observer exception
    queue bounded unbounded notifier allocator
    pool freestack concepts future container
    integration lifecycle stress
)
foreach(tag ${TFL_TEST_TAGS})
    add_test(
        NAME tfl.${tag}
        COMMAND TaskflowLiteTest "[${tag}]" -s -d yes
        WORKING_DIRECTORY $<TARGET_FILE_DIR:TaskflowLiteTest>
    )
    set_tests_properties(tfl.${tag} PROPERTIES
        TIMEOUT 600
        LABELS "unit;tag-${tag}"
    )
endforeach()

# --- 单文件测试（新增） ---
foreach(_src ${TFL_TEST_SOURCES})
    get_filename_component(_full_name "${_src}" NAME_WE)
    string(REGEX REPLACE "^test_" "" _test_name "${_full_name}")
    set(_target "tfl_test_${_test_name}")

    add_test(
        NAME tfl_test.${_test_name}
        COMMAND ${_target} -s -d yes
        WORKING_DIRECTORY $<TARGET_FILE_DIR:${_target}>
    )
    set_tests_properties(tfl_test.${_test_name} PROPERTIES
        TIMEOUT 300
        LABELS "unit;perfile;perfile-${_test_name}"
    )
endforeach()

# ============================================================================
#  自定义构建目标: run_all_tests / run_<tag>_tests / run_test_<name>
# ============================================================================

# --- 一键全跑（不变） ---
add_custom_target(run_all_tests
    COMMAND ${CMAKE_CTEST_COMMAND}
            --output-on-failure
            -C $<CONFIG>
            -R "^TaskflowLiteTest$"
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS TaskflowLiteTest
    USES_TERMINAL
    COMMENT "运行所有 TaskflowLite 单元测试..."
)

# --- 标签一键跑（不变） ---
foreach(tag ${TFL_TEST_TAGS})
    add_custom_target(run_${tag}_tests
        COMMAND ${CMAKE_CTEST_COMMAND}
                --output-on-failure
                -C $<CONFIG>
                -R "^tfl\\.${tag}$"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        DEPENDS TaskflowLiteTest
        USES_TERMINAL
        COMMENT "运行 [${tag}] 标签的测试..."
    )
endforeach()

# --- 单文件一键跑（新增） ---
foreach(_src ${TFL_TEST_SOURCES})
    get_filename_component(_full_name "${_src}" NAME_WE)
    string(REGEX REPLACE "^test_" "" _test_name "${_full_name}")
    set(_target "tfl_test_${_test_name}")

    add_custom_target(run_test_${_test_name}
        COMMAND ${_target} -s -d yes
        WORKING_DIRECTORY $<TARGET_FILE_DIR:${_target}>
        DEPENDS ${_target}
        USES_TERMINAL
        COMMENT "Running standalone test: ${_test_name}"
    )
endforeach()

# ============================================================================
#  状态摘要
# ============================================================================
list(LENGTH TFL_TEST_SOURCES _nfiles)
message(STATUS "================ TaskflowLite Tests ================")
message(STATUS "  测试源文件数      : ${_nfiles}")
message(STATUS "  单体二进制        : TaskflowLiteTest")
message(STATUS "  单文件目标        : ${_nfiles} 个 (tfl_test_*)")
message(STATUS "  单文件默认构建    : ${TFL_TEST_PER_FILE_DEFAULT}")
message(STATUS "  C++ 标准         : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type       : ${CMAKE_BUILD_TYPE}")
if(NOT MSVC)
    message(STATUS "  ASan             : ${TFL_TEST_ENABLE_ASAN}")
    message(STATUS "  TSan             : ${TFL_TEST_ENABLE_TSAN}")
endif()
message(STATUS "  可执行输出       : ${CMAKE_BINARY_DIR}/bin/")
message(STATUS "====================================================")
