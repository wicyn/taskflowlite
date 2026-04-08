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
    11_flow_emplace
    12_loop_workflow
    main
)

foreach(example ${EXAMPLES})
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE TaskflowLite::taskflowlite tfl_internal_flags Threads::Threads)
    target_compile_features(${example} PRIVATE cxx_std_23)
endforeach()

# Group by folder in IDEs
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${EXAMPLES})

add_custom_target(run_all_examples
    COMMAND ${CMAKE_COMMAND} -E echo "Running all examples..."
    USES_TERMINAL
)
foreach(example ${EXAMPLES})
    add_dependencies(run_all_examples ${example})
    add_custom_command(TARGET run_all_examples POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "===== Running ${example} ====="
        COMMAND $<TARGET_FILE:${example}>
    )
endforeach()
