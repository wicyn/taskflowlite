# ============================================================================
#  TaskflowLite 单元测试 CMake —— Catch2 v3 amalgamated
# ============================================================================
#
#  Catch2 来源策略：
#  ----------
#    1. 优先使用 TFL_CATCH2_LOCAL_PATH 指定的 amalgamated 目录。
#    2. 其次搜索 test/catch2/、test/（快、离线、可复现）。
#    3. 本地缺失 → 自动从 GitHub 下载 amalgamated 两个文件。
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
#  默认固定 Catch2 v3.15.0；自定义 ref 时须同时提供两个文件的 SHA256。
#
#  两种构建模式：
#    1. TaskflowLiteTest（单体）   ：全部 test_*.cpp 编进一个可执行文件。
#    2. tfl_test_<name>（单文件） ：每个 test_*.cpp 编成独立可执行文件。
# ============================================================================

cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(TaskflowLiteTest LANGUAGES CXX)
    include("../cmake/TflDependencies.cmake")
    include("../cmake/TflBuildOptions.cmake")
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(CMAKE_CXX_STANDARD 20)
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
# TFL_SANITIZER 由根项目或独立构建入口统一处理。

# ----------------------------------------------------------------------------
#  可选：是否生成按文件的 run_* 便捷目标（默认 OFF）
# ----------------------------------------------------------------------------
option(TFL_TEST_RUN_TARGETS
    "Generate per-file (run_test_<name>) convenience targets" OFF)

# ----------------------------------------------------------------------------
#  Catch2 v3.15.0：固定提交及下载内容，缓存不会跨版本复用。
# ----------------------------------------------------------------------------
set(TFL_CATCH2_REF "6ee0826dcae55ed1e06b2c5701981221e979e1e6" CACHE STRING "Catch2 ref")
if(TFL_CATCH2_REF STREQUAL "6ee0826dcae55ed1e06b2c5701981221e979e1e6" OR
   TFL_CATCH2_REF STREQUAL "v3.15.0")
    # 使用固定提交的原始 Git blob 哈希，不受本地 checkout 的 CRLF 转换影响。
    set(_catch_cpp_hash 2ab441b2fa0051a547e88af4ad98151c1ce1f2fbe3d5e9ad9367cfc2fd44dbf8)
    set(_catch_hpp_hash ddf4e42976dea2bbbe8e7464ad5ab156e7061cc8ccef290e6e406477283483ee)
else()
    set(_catch_cpp_hash "${TFL_CATCH2_CPP_SHA256}")
    set(_catch_hpp_hash "${TFL_CATCH2_HPP_SHA256}")
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
        "[tests] No test_*.cpp files found in ${CMAKE_CURRENT_SOURCE_DIR}/")
endif()

# ============================================================================
#  Catch2 amalgamated 来源决议：本地优先，缺失则自动下载（复用根镜像前缀）
#  搜索顺序：指定目录 → test/catch2/ → test/ → 下载
# ============================================================================
set(_local_found OFF)
set(TFL_CATCH2_LOCAL_PATH "" CACHE PATH "Local Catch2 amalgamated directory (contains catch_amalgamated.cpp/.hpp)")
# 显式路径错误须立即报告，不能悄悄回退到联网下载。
if(TFL_CATCH2_LOCAL_PATH AND
   (NOT EXISTS "${TFL_CATCH2_LOCAL_PATH}/catch_amalgamated.cpp" OR
    NOT EXISTS "${TFL_CATCH2_LOCAL_PATH}/catch_amalgamated.hpp"))
    message(FATAL_ERROR "TFL_CATCH2_LOCAL_PATH must contain catch_amalgamated.cpp and catch_amalgamated.hpp")
endif()
# 优先使用显式指定的目录，其次搜索仓库内目录。
if(TFL_CATCH2_LOCAL_PATH
   AND EXISTS "${TFL_CATCH2_LOCAL_PATH}/catch_amalgamated.cpp"
   AND EXISTS "${TFL_CATCH2_LOCAL_PATH}/catch_amalgamated.hpp")
    set(TFL_CATCH2_SOURCE "local(-DTFL_CATCH2_LOCAL_PATH)")
    set(CATCH2_SRC "${TFL_CATCH2_LOCAL_PATH}/catch_amalgamated.cpp")
    set(CATCH2_INC_DIR "${TFL_CATCH2_LOCAL_PATH}")
    set(_local_found ON)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/catch2/catch_amalgamated.cpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/catch2/catch_amalgamated.hpp")
    set(TFL_CATCH2_SOURCE "local(catch2/)")
    set(CATCH2_SRC     "${CMAKE_CURRENT_SOURCE_DIR}/catch2/catch_amalgamated.cpp")
    set(CATCH2_INC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/catch2")
    set(_local_found ON)
# ② 其次：test/ 根目录
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/catch_amalgamated.cpp"
       AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/catch_amalgamated.hpp")
    set(TFL_CATCH2_SOURCE "local")
    set(CATCH2_SRC     "${CMAKE_CURRENT_SOURCE_DIR}/catch_amalgamated.cpp")
    set(CATCH2_INC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(_local_found ON)
endif()

if(_local_found)
    # ---- 本地存在：直接用 ----
    message(STATUS "[catch2] using local source (${TFL_CATCH2_SOURCE}) -> ${CATCH2_INC_DIR}")
else()
    foreach(_hash "${_catch_cpp_hash}" "${_catch_hpp_hash}")
        string(LENGTH "${_hash}" _hash_length)
        if(NOT _hash_length EQUAL 64 OR NOT _hash MATCHES "^[0-9a-fA-F]+$")
            message(FATAL_ERROR "A custom Catch2 ref requires TFL_CATCH2_CPP_SHA256 and TFL_CATCH2_HPP_SHA256")
        endif()
    endforeach()
    string(TOLOWER "${_catch_cpp_hash}" _catch_cpp_hash)
    string(TOLOWER "${_catch_hpp_hash}" _catch_hpp_hash)
    string(SHA256 _cache_key "${TFL_CATCH2_REF}|${_catch_cpp_hash}|${_catch_hpp_hash}")
    set(TFL_CATCH2_SOURCE "downloaded(${TFL_CATCH2_REF})")
    set(_dl_dir "${PROJECT_BINARY_DIR}/third_party/catch2-${_cache_key}")
    file(MAKE_DIRECTORY "${_dl_dir}")
    set(_base "${TFL_EFFECTIVE_PREFIX}https://raw.githubusercontent.com/catchorg/Catch2/${TFL_CATCH2_REF}/extras")
    tfl_download_checked("${_base}/catch_amalgamated.cpp" "${_dl_dir}/catch_amalgamated.cpp" "${_catch_cpp_hash}")
    tfl_download_checked("${_base}/catch_amalgamated.hpp" "${_dl_dir}/catch_amalgamated.hpp" "${_catch_hpp_hash}")

    set(CATCH2_SRC     "${_dl_dir}/catch_amalgamated.cpp")
    set(CATCH2_INC_DIR "${_dl_dir}")
    message(STATUS "[catch2] ready -> ${_dl_dir}")
endif()

# ============================================================================
#  Catch2 amalgamated OBJECT 库 —— 编译一次，多方链接
# ============================================================================
add_library(tfl_catch2_main OBJECT "${CATCH2_SRC}")
target_include_directories(tfl_catch2_main PUBLIC "${CATCH2_INC_DIR}")
target_compile_features(tfl_catch2_main PRIVATE cxx_std_20)

if(TARGET tfl_internal_flags)
    target_link_libraries(tfl_catch2_main PRIVATE tfl_internal_flags)
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
    target_link_libraries(${_tgt} PRIVATE tfl_catch2_main tfl_internal_flags)

    target_compile_features(${_tgt} PRIVATE cxx_std_20)
    target_include_directories(${_tgt} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    if(TARGET TaskflowLite::taskflowlite)
        target_link_libraries(${_tgt} PRIVATE
            TaskflowLite::taskflowlite
            Threads::Threads)
    else()
        message(STATUS
            "[tests] TaskflowLite::taskflowlite unavailable, ${_tgt} falls back to include-path mode")
        target_include_directories(${_tgt}
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_link_libraries(${_tgt} PRIVATE Threads::Threads)
    endif()

    # GCC/Clang 上需显式链接 libatomic 以支持 128-bit DWCAS (TaggedHead128)
    # 注意: macOS/AppleClang 无独立 libatomic，故排除 APPLE。
    if(NOT MSVC AND NOT APPLE)
        target_link_libraries(${_tgt} PRIVATE atomic)
    endif()

    if(MSVC)
        target_compile_options(${_tgt} PRIVATE /permissive- /bigobj)
    endif()

    set_target_properties(${_tgt} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
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
    COMMAND TaskflowLiteTest -d yes
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

    # EXCLUDE_FROM_ALL 的二进制默认不构建，不能注册成默认必跑项。
    if(NOT TFL_TEST_PER_FILE_DEFAULT)
        continue()
    endif()

    add_test(
        NAME tfl_test.${_test_name}
        COMMAND ${_target} -d yes
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
    WORKING_DIRECTORY ${PROJECT_BINARY_DIR}
    DEPENDS TaskflowLiteTest
    USES_TERMINAL
    VERBATIM
    COMMENT "Running all TaskflowLite unit tests..."
)
set_target_properties(run_all_tests PROPERTIES FOLDER "Run")

if(TFL_TEST_RUN_TARGETS)
    foreach(_src ${TFL_TEST_SOURCES})
        get_filename_component(_full_name "${_src}" NAME_WE)
        string(REGEX REPLACE "^test_" "" _test_name "${_full_name}")
        set(_target "tfl_test_${_test_name}")

        add_custom_target(run_test_${_test_name}
            COMMAND ${_target} -d yes
            WORKING_DIRECTORY $<TARGET_FILE_DIR:${_target}>
            DEPENDS ${_target}
            USES_TERMINAL
            VERBATIM
            COMMENT "Running standalone test: ${_test_name}"
        )
        set_target_properties(run_test_${_test_name} PROPERTIES FOLDER "Run/ByFile")
    endforeach()
endif()

# ============================================================================
#  状态摘要
# ============================================================================
list(LENGTH TFL_TEST_SOURCES _nfiles)
message(STATUS "-- TaskflowLite Tests -------------------------")
message(STATUS "  Sources       : ${_nfiles} (test_*.cpp)")
message(STATUS "  Catch2 source : ${TFL_CATCH2_SOURCE}")
message(STATUS "  Monolithic    : TaskflowLiteTest")
message(STATUS "  Per-file      : ${_nfiles} (tfl_test_*, in ALL=${TFL_TEST_PER_FILE_DEFAULT})")
message(STATUS "  C++ Standard  : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Sanitizer     : ${TFL_SANITIZER}")
message(STATUS "  Output dir    : ${PROJECT_BINARY_DIR}/bin/")
message(STATUS "-----------------------------------------------")
