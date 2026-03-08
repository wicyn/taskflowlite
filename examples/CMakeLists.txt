# TaskflowLite Examples
# ======================

# Add all example executables
set(EXAMPLES
    01_basic_dag
    02_parallel
    03_loop
    04_runtime
    05_branch
    06_jump
    07_semaphore
    08_subflow
    09_pipeline
    10_dump
    main
)

foreach(example ${EXAMPLES})
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE TaskflowLite::taskflowlite tfl_internal_flags Threads::Threads)
    target_compile_features(${example} PRIVATE cxx_std_23)
endforeach()

# Group by folder in IDEs
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${EXAMPLES})
