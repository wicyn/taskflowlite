# ==============================================================================
# TaskflowLite Benchmarks -- TaskflowLite vs Taskflow 对比
# 本地路径优先；否则稀疏检出 Taskflow v4.1.0 的固定提交。
# 两个可执行文件使用相同 C++ 标准、优化、指令集和 sanitizer 选项。
# ==============================================================================

set(TFL_TASKFLOW_COMMIT "45366fe5bc4f2f8ec9aa590b40c504e296886865" CACHE STRING
    "Taskflow commit (full 40-character SHA)")
set(TASKFLOW_LOCAL_PATH "" CACHE PATH "Local Taskflow include root (must contain taskflow/taskflow.hpp)")

set(taskflow_SOURCE_DIR "")
set(TFL_TASKFLOW_SOURCE "")

# 显式路径错误须立即报告，不回退到联网下载。
if(TASKFLOW_LOCAL_PATH AND NOT EXISTS "${TASKFLOW_LOCAL_PATH}/taskflow/taskflow.hpp")
    message(FATAL_ERROR "TASKFLOW_LOCAL_PATH must contain taskflow/taskflow.hpp")
endif()
# ① 显式指定：其下须有 taskflow/taskflow.hpp
if(TASKFLOW_LOCAL_PATH AND EXISTS "${TASKFLOW_LOCAL_PATH}/taskflow/taskflow.hpp")
    set(taskflow_SOURCE_DIR "${TASKFLOW_LOCAL_PATH}")
    set(TFL_TASKFLOW_SOURCE "local(-DTASKFLOW_LOCAL_PATH)")
endif()

# ② benchmarks/taskflow/ —— 该目录本身就是 <taskflow/>，故 include 根为 benchmarks/ (= 本目录)
if(NOT taskflow_SOURCE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/taskflow/taskflow.hpp")
    set(taskflow_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    set(TFL_TASKFLOW_SOURCE "local(benchmarks/taskflow/)")
endif()

# ③ 仓库同级 ../taskflow —— 其下有 taskflow/taskflow.hpp，include 根为 ..
if(NOT taskflow_SOURCE_DIR AND EXISTS "${PROJECT_SOURCE_DIR}/../taskflow/taskflow/taskflow.hpp")
    set(taskflow_SOURCE_DIR "${PROJECT_SOURCE_DIR}/../taskflow")
    set(TFL_TASKFLOW_SOURCE "local(../taskflow)")
endif()

if(taskflow_SOURCE_DIR)
    message(STATUS "[taskflow] using local source -> ${taskflow_SOURCE_DIR}/taskflow/")
else()
    string(LENGTH "${TFL_TASKFLOW_COMMIT}" _commit_length)
    if(NOT _commit_length EQUAL 40 OR NOT TFL_TASKFLOW_COMMIT MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR "TFL_TASKFLOW_COMMIT must be a full 40-character commit SHA")
    endif()
    string(TOLOWER "${TFL_TASKFLOW_COMMIT}" TFL_TASKFLOW_COMMIT)
    set(TFL_TASKFLOW_SOURCE "pinned(${TFL_TASKFLOW_COMMIT})")
    set(taskflow_SOURCE_DIR "${PROJECT_BINARY_DIR}/third_party/taskflow-${TFL_TASKFLOW_COMMIT}")
    tfl_fetch_taskflow("${taskflow_SOURCE_DIR}" "${TFL_TASKFLOW_COMMIT}")
endif()

# Taskflow 是 header-only: 直接造 INTERFACE 目标 (不走它自己的 CMake, 不编 examples/tests)
if(NOT TARGET Taskflow::Taskflow)
    add_library(Taskflow INTERFACE)
    add_library(Taskflow::Taskflow ALIAS Taskflow)
    target_include_directories(Taskflow INTERFACE "${taskflow_SOURCE_DIR}")
    target_compile_features(Taskflow INTERFACE cxx_std_20)
    find_package(Threads REQUIRED)
    target_link_libraries(Taskflow INTERFACE Threads::Threads)
endif()

# INTERFACE 库在 VS 里不生成项目, 看不到头文件。
# 造一个纯浏览用的假目标(不参与构建), 仅为在 IDE 里能点开 taskflow/ 头文件。
file(GLOB_RECURSE TASKFLOW_HEADERS CONFIGURE_DEPENDS
    "${taskflow_SOURCE_DIR}/taskflow/*.hpp"
    "${taskflow_SOURCE_DIR}/taskflow/*.h")
if(TASKFLOW_HEADERS AND NOT TARGET taskflow_headers)
    add_custom_target(taskflow_headers SOURCES ${TASKFLOW_HEADERS})
    source_group(TREE "${taskflow_SOURCE_DIR}/taskflow" FILES ${TASKFLOW_HEADERS})
    set_target_properties(taskflow_headers PROPERTIES FOLDER "ThirdParty")
endif()

# ──────────────────────────────────────────────────────────────────────────────
# bench_taskflowlite -- TaskflowLite benchmarks (20 scheduling scenarios)
# ──────────────────────────────────────────────────────────────────────────────
add_executable(bench_taskflowlite bench_taskflowlite.cpp bench_common.hpp)
target_link_libraries(bench_taskflowlite PRIVATE
    TaskflowLite::taskflowlite
    tfl_internal_flags
    Threads::Threads
)
target_compile_features(bench_taskflowlite PRIVATE cxx_std_20)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES bench_taskflowlite.cpp)

# ──────────────────────────────────────────────────────────────────────────────
# bench_taskflow -- Taskflow comparison benchmarks
# ──────────────────────────────────────────────────────────────────────────────
add_executable(bench_taskflow bench_taskflow.cpp bench_common.hpp)
target_link_libraries(bench_taskflow PRIVATE
    Taskflow::Taskflow
    tfl_internal_flags
    Threads::Threads
)
target_compile_features(bench_taskflow PRIVATE cxx_std_20)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES bench_taskflow.cpp)

# 已建出的 benchmark 目标(给 run_all_benchmarks 用)
set(BENCHMARKS bench_taskflowlite bench_taskflow)
if(TFL_BUILD_TESTS)
    enable_testing()
    foreach(b ${BENCHMARKS})
        add_test(NAME benchmark.${b}.smoke COMMAND $<TARGET_FILE:${b}> --smoke)
        set_tests_properties(benchmark.${b}.smoke PROPERTIES
            TIMEOUT 120 LABELS "benchmark;smoke")
    endforeach()
endif()

# ──────────────────────────────────────────────────────────────────────────────
# run_all_benchmarks -- one-command execution of all benchmark targets
# ──────────────────────────────────────────────────────────────────────────────
add_custom_target(run_all_benchmarks
    COMMAND ${CMAKE_COMMAND} -E echo "Running all benchmarks..."
    USES_TERMINAL
    VERBATIM
)
foreach(b ${BENCHMARKS})
    add_dependencies(run_all_benchmarks ${b})
    add_custom_command(TARGET run_all_benchmarks POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "===== Running ${b} ====="
        COMMAND $<TARGET_FILE:${b}>
        VERBATIM
    )
endforeach()

# ──────────────────────────────────────────────────────────────────────────────
# IDE 解决方案文件夹分组 (依赖根 CMakeLists 的 USE_FOLDERS ON)
# ──────────────────────────────────────────────────────────────────────────────
foreach(b ${BENCHMARKS})
    set_target_properties(${b} PROPERTIES FOLDER "Benchmarks")
endforeach()
set_target_properties(run_all_benchmarks PROPERTIES FOLDER "Run")

# ──────────────────────────────────────────────────────────────────────────────
# 状态摘要
# ──────────────────────────────────────────────────────────────────────────────
message(STATUS "-- TaskflowLite Benchmarks --------------------")
message(STATUS "  Targets       : bench_taskflowlite / bench_taskflow")
message(STATUS "  Taskflow src  : ${TFL_TASKFLOW_SOURCE}")
message(STATUS "  Taskflow path : ${taskflow_SOURCE_DIR}")
message(STATUS "  Run all       : run_all_benchmarks")
message(STATUS "-----------------------------------------------")
