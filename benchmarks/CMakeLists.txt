# ==============================================================================
# TaskflowLite Benchmarks
# ==============================================================================

# Benchmark executable
add_executable(benchmark benchmark.cpp)

# Link libraries (统一使用内部标志)
target_link_libraries(benchmark PRIVATE 
    TaskflowLite::taskflowlite 
    tfl_internal_flags 
    Threads::Threads
)

# Require C++23
target_compile_features(benchmark PRIVATE cxx_std_23)

# Group by folder in IDEs
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES benchmark.cpp)
