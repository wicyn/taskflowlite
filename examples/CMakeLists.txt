# ============================================================================
#  TaskflowLite Examples
# ============================================================================
#
#  设计要点：
#  ----------
#  - GLOB + CONFIGURE_DEPENDS 自动发现所有 *.cpp，新增示例无需改 CMake。
#  - 目标名加前缀 tfl_ex_<name> 防止与全局其它目标冲突（尤其 main）；
#    OUTPUT_NAME 保留干净原名，可执行文件仍是 01_basic_dag(.exe) / main(.exe)。
#  - 统一 tfl_setup_example_target() 封装链接/编译选项，避免配置漂移
#    （与 test/CMakeLists.txt 的 tfl_setup_test_target 对称）。
#  - 所有示例目标折进 VS 解决方案文件夹 "Examples/"；run_all_examples 折进 "Run/"。
#  - source_group(TREE ... FILES <真实文件路径>) ，IDE 分组真正生效。
#
#  常用：
#  ----------
#    cmake --build . --target tfl_ex_01_basic_dag   # 只编某个示例
#    cmake --build . --target run_all_examples       # 构建并依次跑全部
#    -DTFL_EXAMPLES_RUN_TARGETS=ON                   # 额外生成逐例 run_example_<name>
#    -DTFL_EXAMPLES_EXCLUDE_FROM_ALL=ON              # 示例不纳入默认构建
# ============================================================================

cmake_minimum_required(VERSION 3.21)

# VS 解决方案启用文件夹分组（全局属性）
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ----------------------------------------------------------------------------
#  选项
# ----------------------------------------------------------------------------
option(TFL_EXAMPLES_RUN_TARGETS
    "Generate per-example run_example_<name> convenience targets" OFF)
option(TFL_EXAMPLES_EXCLUDE_FROM_ALL
    "Exclude examples from the default (ALL) build" OFF)

# ----------------------------------------------------------------------------
#  自动收集示例源（GLOB + CONFIGURE_DEPENDS 自动发现新增文件）
# ----------------------------------------------------------------------------
file(GLOB TFL_EXAMPLE_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
list(SORT TFL_EXAMPLE_SOURCES)

if(NOT TFL_EXAMPLE_SOURCES)
    message(FATAL_ERROR "未找到任何 *.cpp 示例于 ${CMAKE_CURRENT_SOURCE_DIR}/")
endif()

# ----------------------------------------------------------------------------
#  辅助函数: tfl_setup_example_target
#  把链接、编译选项、IDE 文件夹、输出目录等共享设置封装在此。
# ----------------------------------------------------------------------------
function(tfl_setup_example_target _tgt)
    target_link_libraries(${_tgt} PRIVATE
        TaskflowLite::taskflowlite
        Threads::Threads)
    if(TARGET tfl_internal_flags)
        target_link_libraries(${_tgt} PRIVATE tfl_internal_flags)
    endif()

    target_compile_features(${_tgt} PRIVATE cxx_std_23)

    set_target_properties(${_tgt} PROPERTIES
        FOLDER "Examples"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/examples")
endfunction()

# ----------------------------------------------------------------------------
#  为每个 *.cpp 生成一个可执行目标
# ----------------------------------------------------------------------------
set(_example_targets "")
foreach(_src ${TFL_EXAMPLE_SOURCES})
    get_filename_component(_name "${_src}" NAME_WE)   # e.g. 01_basic_dag / main
    set(_tgt "tfl_ex_${_name}")                       # 前缀防撞

    if(TFL_EXAMPLES_EXCLUDE_FROM_ALL)
        add_executable(${_tgt} EXCLUDE_FROM_ALL "${_src}")
    else()
        add_executable(${_tgt} "${_src}")
    endif()

    # 可执行文件名保留干净原名（01_basic_dag.exe，而非 tfl_ex_01_basic_dag.exe）
    set_target_properties(${_tgt} PROPERTIES OUTPUT_NAME "${_name}")

    tfl_setup_example_target(${_tgt})
    list(APPEND _example_targets ${_tgt})
endforeach()

# IDE 源码分组（用真实文件路径，分组才有效）
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${TFL_EXAMPLE_SOURCES})

# ----------------------------------------------------------------------------
#  run_all_examples —— 构建并依次运行全部示例
#  注意: 它会逐个运行所有示例可执行文件。若某些示例是长时间运行/需要交互的，
#        按需在 EXCLUDE 列表里排除，或改用逐例 run_example_<name>。
# ----------------------------------------------------------------------------
add_custom_target(run_all_examples
    COMMAND ${CMAKE_COMMAND} -E echo "Running all examples..."
    USES_TERMINAL
    COMMENT "构建并依次运行所有示例"
)
set_target_properties(run_all_examples PROPERTIES FOLDER "Run")

foreach(_tgt ${_example_targets})
    add_dependencies(run_all_examples ${_tgt})
    add_custom_command(TARGET run_all_examples POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "===== Running ${_tgt} ====="
        COMMAND $<TARGET_FILE:${_tgt}>
    )
endforeach()

# ----------------------------------------------------------------------------
#  逐例一键跑（可选，默认 OFF；折进 Run/Examples）
# ----------------------------------------------------------------------------
if(TFL_EXAMPLES_RUN_TARGETS)
    foreach(_src ${TFL_EXAMPLE_SOURCES})
        get_filename_component(_name "${_src}" NAME_WE)
        set(_tgt "tfl_ex_${_name}")

        add_custom_target(run_example_${_name}
            COMMAND $<TARGET_FILE:${_tgt}>
            DEPENDS ${_tgt}
            USES_TERMINAL
            COMMENT "Running example: ${_name}"
        )
        set_target_properties(run_example_${_name} PROPERTIES FOLDER "Run/Examples")
    endforeach()
endif()

# ----------------------------------------------------------------------------
#  状态摘要
# ----------------------------------------------------------------------------
list(LENGTH TFL_EXAMPLE_SOURCES _n)
message(STATUS "================ TaskflowLite Examples ================")
message(STATUS "  示例源文件数    : ${_n} (自动发现 *.cpp)")
message(STATUS "  目标命名        : tfl_ex_<name> (输出名保留原名)")
message(STATUS "  纳入默认构建    : NOT ${TFL_EXAMPLES_EXCLUDE_FROM_ALL}")
if(TFL_EXAMPLES_RUN_TARGETS)
    message(STATUS "  逐例 run 目标   : run_example_<name> (已生成)")
else()
    message(STATUS "  逐例 run 目标   : 关闭 (-DTFL_EXAMPLES_RUN_TARGETS=ON 开启)")
endif()
message(STATUS "  可执行输出      : ${CMAKE_BINARY_DIR}/bin/examples/")
message(STATUS "=======================================================")