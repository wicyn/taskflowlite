# ============================================================================
#  TaskflowLite 单元测试 CMake —— Catch2 v3 amalgamated
# ============================================================================
#
#  Catch2 来源策略：
#  ----------
#    1. 本地存在 test/catch_amalgamated.cpp/.hpp → 用本地（快、离线、可复现）。
#    2. 本地缺失 → 自动从 GitHub 下载 amalgamated 两个文件。
#       下载地址自动复用根 CMake 已选的镜像前缀 (TFL_EFFECTIVE_PREFIX) 与
#       代理 (TFL_GIT_PROXY)，无需在此重复配置——在 cmake-gui 顶层的
#       TFL_GITHUB_MIRROR 下拉框里选一次，Catch2 下载就跟着走。
#
#  → 本子目录仅在 -DTFL_BUILD_TESTS=ON 时被加入；该开关默认 OFF，
#    所以默认 configure 不会进入这里、不会触发任何下载（与 benchmarks 对称）。
#
#  说明：镜像前缀这套原本用于 git clone。file(DOWNLOAD) 拉的是
#        raw.githubusercontent.com，部分镜像不一定代理 raw；若下载失败，
#        换个镜像重试，或手动把两个文件放到 test/ 走本地路径。
#
#  下载 ref（默认 devel）：非 GUI 变量，需要时 -DTFL_CATCH2_REF=v3.15.0 覆盖。
#
#  两种构建模式：
#    1. TaskflowLiteTest（单体）   ：全部 test_*.cpp 编进一个可执行文件。
#    2. tfl_test_<name>（单文件） ：每个 test_*.cpp 编成独立可执行文件。
# ============================================================================

cmake_minimum_required(VERSION 3.21)

if(NOT CMAKE_PROJECT_NAME)
    project(TaskflowLiteTest LANGUAGES CXX)
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
#  可选：是否生成按文件的 run_* 便捷目标（默认 OFF）
# ----------------------------------------------------------------------------
option(TFL_TEST_RUN_TARGETS
    "Generate per-file (run_test_<name>) convenience targets" OFF)

# ----------------------------------------------------------------------------
#  Catch2 下载 ref —— 非 CACHE 变量，不出现在 cmake-gui；可用 -D 覆盖。
# ----------------------------------------------------------------------------
if(NOT DEFINED TFL_CATCH2_REF)
    set(TFL_CATCH2_REF "devel")
endif()

# 独立构建 test（不经根 CMake）时的兜底，避免引用未定义变量。
if(NOT DEFINED TFL_EFFECTIVE_PREFIX)
    set(TFL_EFFECTIVE_PREFIX "")
endif()
if(NOT DEFINED TFL_GIT_PROXY)
    set(TFL_GIT_PROXY "")
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ----------------------------------------------------------------------------
#  自动收集测试源
# ----------------------------------------------------------------------------
file(GLOB TFL_TEST_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp"
)
if(NOT TFL_TEST_SOURCES)
    message(FATAL_ERROR
        "[tests] 未找到任何 test_*.cpp 文件于 ${CMAKE_CURRENT_SOURCE_DIR}/")
endif()

# ============================================================================
#  Catch2 amalgamated 来源决议：本地优先，缺失则自动下载（复用根镜像前缀）
# ============================================================================
set(_local_src "${CMAKE_CURRENT_SOURCE_DIR}/catch_amalgamated.cpp")
set(_local_hdr "${CMAKE_CURRENT_SOURCE_DIR}/catch_amalgamated.hpp")

if(EXISTS "${_local_src}" AND EXISTS "${_local_hdr}")
    # ---- 本地存在：直接用 ----
    set(TFL_CATCH2_SOURCE "local")
    set(CATCH2_SRC     "${_local_src}")
    set(CATCH2_INC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
else()
    # ---- 本地缺失：自动下载，复用根 CMake 已选的镜像前缀 ----
    set(TFL_CATCH2_SOURCE "downloaded(${TFL_CATCH2_REF})")
    set(_dl_dir "${CMAKE_BINARY_DIR}/third_party/catch2")
    file(MAKE_DIRECTORY "${_dl_dir}")

    set(_raw "https://raw.githubusercontent.com/catchorg/Catch2/${TFL_CATCH2_REF}/extras")
    set(_base "${TFL_EFFECTIVE_PREFIX}${_raw}")

    message(STATUS "[catch2] 本地缺失，自动下载 amalgamated (ref=${TFL_CATCH2_REF}, prefix='${TFL_EFFECTIVE_PREFIX}')")

    foreach(_f catch_amalgamated.cpp catch_amalgamated.hpp)
        if(NOT EXISTS "${_dl_dir}/${_f}")
            message(STATUS "[catch2] 下载 ${_f} ...")
            file(DOWNLOAD
                "${_base}/${_f}"
                "${_dl_dir}/${_f}"
                STATUS  _dl_status
                TLS_VERIFY ON
                INACTIVITY_TIMEOUT 30
            )
            list(GET _dl_status 0 _dl_code)
            if(NOT _dl_code EQUAL 0)
                list(GET _dl_status 1 _dl_msg)
                file(REMOVE "${_dl_dir}/${_f}")
                message(FATAL_ERROR
                    "[catch2] 下载失败: ${_f}\n"
                    "  URL : ${_base}/${_f}\n"
                    "  错误: ${_dl_msg}\n"
                    "  解决: 换个 TFL_GITHUB_MIRROR 重试，或手动把 "
                    "catch_amalgamated.cpp/.hpp 放到 ${CMAKE_CURRENT_SOURCE_DIR}/")
            endif()
        endif()
    endforeach()

    set(CATCH2_SRC     "${_dl_dir}/catch_amalgamated.cpp")
    set(CATCH2_INC_DIR "${_dl_dir}")
    message(STATUS "[catch2] 就绪 → ${_dl_dir}")
endif()

# ============================================================================
#  Catch2 amalgamated OBJECT 库 —— 编译一次，多方链接
# ============================================================================
add_library(tfl_catch2_main OBJECT "${CATCH2_SRC}")
target_include_directories(tfl_catch2_main PUBLIC "${CATCH2_INC_DIR}")
target_compile_features(tfl_catch2_main PRIVATE cxx_std_23)

if(TARGET tfl_internal_flags)
    target_link_libraries(tfl_catch2_main PRIVATE tfl_internal_flags)
endif()
if(MSVC AND TFL_SANITIZER STREQUAL "ASAN")
    target_compile_options(tfl_catch2_main PRIVATE /fsanitize=address /Zi)
endif()
if(MSVC)
    target_compile_options(tfl_catch2_main PRIVATE /W3 /utf-8 /bigobj)
else()
    target_compile_options(tfl_catch2_main PRIVATE -w)
endif()

# IDE 分组：Catch2 是第三方，与 benchmarks 的 taskflow_headers 同归 ThirdParty/
set_target_properties(tfl_catch2_main PROPERTIES FOLDER "ThirdParty")

# ============================================================================
#  辅助函数: tfl_setup_test_target
# ============================================================================
function(tfl_setup_test_target _tgt)
    target_link_libraries(${_tgt} PRIVATE tfl_catch2_main)

    target_compile_features(${_tgt} PRIVATE cxx_std_23)
    target_include_directories(${_tgt} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    if(TARGET TaskflowLite::taskflowlite)
        target_link_libraries(${_tgt} PRIVATE
            TaskflowLite::taskflowlite
            Threads::Threads)
        if(TARGET tfl_internal_flags)
            target_link_libraries(${_tgt} PRIVATE tfl_internal_flags)
        endif()
    else()
        message(STATUS
            "[tests] TaskflowLite::taskflowlite 不可用，${_tgt} 回退到 include 路径模式")
        target_include_directories(${_tgt}
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_link_libraries(${_tgt} PRIVATE Threads::Threads)
    endif()

    # GCC/Clang 上需显式链接 libatomic 以支持 128-bit DWCAS (FreeStack128)
    # 注意: macOS/AppleClang 无独立 libatomic，故排除 APPLE。
    if(NOT MSVC AND NOT APPLE)
        target_link_libraries(${_tgt} PRIVATE atomic)
    endif()

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

    set_target_properties(${_tgt} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endfunction()

# ============================================================================
#  单体外挂二进制: TaskflowLiteTest
# ============================================================================
add_executable(TaskflowLiteTest ${TFL_TEST_SOURCES})
tfl_setup_test_target(TaskflowLiteTest)

if(MSVC)
    target_compile_options(TaskflowLiteTest PRIVATE /bigobj)
endif()

# IDE 分组：单体主测试归 Tests/ 根（单文件目标归 Tests/PerFile 子组）
set_target_properties(TaskflowLiteTest PROPERTIES FOLDER "Tests")

source_group("Tests" REGULAR_EXPRESSION ".*test_.*\\.cpp$")
source_group("Catch2" REGULAR_EXPRESSION ".*catch_amalgamated.*")

# ============================================================================
#  单文件目标: tfl_test_<name>
# ============================================================================
option(TFL_TEST_PER_FILE_DEFAULT
    "Build per-file test executables as part of the default build" OFF)

foreach(_src ${TFL_TEST_SOURCES})
    get_filename_component(_full_name "${_src}" NAME_WE)
    string(REGEX REPLACE "^test_" "" _test_name "${_full_name}")
    set(_target "tfl_test_${_test_name}")

    if(TFL_TEST_PER_FILE_DEFAULT)
        add_executable(${_target} "${_src}")
    else()
        add_executable(${_target} EXCLUDE_FROM_ALL "${_src}")
    endif()

    tfl_setup_test_target(${_target})
    set_target_properties(${_target} PROPERTIES FOLDER "Tests/PerFile")
endforeach()

# ============================================================================
#  CTest 注册
# ============================================================================
enable_testing()

add_test(
    NAME TaskflowLiteTest
    COMMAND TaskflowLiteTest -s -d yes
    WORKING_DIRECTORY $<TARGET_FILE_DIR:TaskflowLiteTest>
)
set_tests_properties(TaskflowLiteTest PROPERTIES
    TIMEOUT 1500
    LABELS "unit;taskflowlite"
)

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
#  便捷构建目标
# ============================================================================
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
set_target_properties(run_all_tests PROPERTIES FOLDER "Run")

if(TFL_TEST_RUN_TARGETS)
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
        set_target_properties(run_test_${_test_name} PROPERTIES FOLDER "Run/ByFile")
    endforeach()
endif()

# ============================================================================
#  状态摘要
# ============================================================================
list(LENGTH TFL_TEST_SOURCES _nfiles)
message(STATUS "── TaskflowLite Tests ─────────────────────────")
message(STATUS "  源文件        : ${_nfiles} 个 (test_*.cpp)")
message(STATUS "  Catch2 来源   : ${TFL_CATCH2_SOURCE}")
message(STATUS "  单体二进制    : TaskflowLiteTest")
message(STATUS "  单文件目标    : ${_nfiles} 个 (tfl_test_*, 默认构建=${TFL_TEST_PER_FILE_DEFAULT})")
message(STATUS "  C++ 标准      : C++${CMAKE_CXX_STANDARD}")
if(NOT MSVC)
    message(STATUS "  ASan / TSan   : ${TFL_TEST_ENABLE_ASAN} / ${TFL_TEST_ENABLE_TSAN}")
endif()
message(STATUS "  可执行输出    : ${CMAKE_BINARY_DIR}/bin/")
message(STATUS "───────────────────────────────────────────────")