# ==============================================================================
# TaskflowLite Benchmarks
# ==============================================================================

# ── Taskflow 依赖（本地头文件）────────────────────────
add_library(taskflow_headers INTERFACE)
target_include_directories(taskflow_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
add_library(Taskflow::Taskflow ALIAS taskflow_headers)

# ── 原始基准（保留不动）───────────────────────────────
add_executable(benchmark benchmark.cpp)
target_link_libraries(benchmark PRIVATE
    TaskflowLite::taskflowlite
    tfl_internal_flags
    Threads::Threads
)
target_compile_features(benchmark PRIVATE cxx_std_23)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES benchmark.cpp)

# ── TaskflowLite 新基准 ───────────────────────────────
add_executable(bench_taskflowlite bench_taskflowlite.cpp)
target_link_libraries(bench_taskflowlite PRIVATE
    TaskflowLite::taskflowlite
    tfl_internal_flags
    Threads::Threads
)
target_compile_features(bench_taskflowlite PRIVATE cxx_std_23)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES bench_taskflowlite.cpp)

# ── Taskflow 对比基准 ─────────────────────────────────
add_executable(bench_taskflow bench_taskflow.cpp)
target_link_libraries(bench_taskflow PRIVATE
    Taskflow::Taskflow
    Threads::Threads
)
target_compile_features(bench_taskflow PRIVATE cxx_std_20)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES bench_taskflow.cpp)

set(BENCHMARKS benchmark bench_taskflowlite bench_taskflow)

add_custom_target(run_all_benchmarks
    COMMAND ${CMAKE_COMMAND} -E echo "Running all benchmarks..."
    USES_TERMINAL
)
foreach(b ${BENCHMARKS})
    add_dependencies(run_all_benchmarks ${b})
    add_custom_command(TARGET run_all_benchmarks POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "===== Running ${b} ====="
        COMMAND $<TARGET_FILE:${b}>
    )
endforeach()
