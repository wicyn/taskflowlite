# cmake_minimum_required(VERSION 3.21)
# project(TaskflowLiteTest)

# # 1. 创建可执行文件
# add_executable(TaskflowLiteTest test_taskflowlite.cpp catch_amalgamated.cpp)

# # 2. 强制指定 C++23 标准
# target_compile_features(TaskflowLiteTest PRIVATE cxx_std_23)

# # 3. 链接库 (使用内部标志统一管理编译选项)
# target_link_libraries(TaskflowLiteTest
#     PRIVATE
#         TaskflowLite::taskflowlite
#         tfl_internal_flags
#         Threads::Threads
# )

# # 4. 开启 CTest 测试支持
# enable_testing()

# # 【核心修改】：在 COMMAND 后面加上 -s 和 -d yes 参数  ---- 使用VS 需要右键项目 -> 属性 -> 调试 -> 命令参数 -> 填入 -s -d yes。显示测试结果
# add_test(NAME TaskflowLiteTest COMMAND TaskflowLiteTest -s -d yes)

# add_custom_target(run_all_tests
#     COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -C $<CONFIG>
#     WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
#     DEPENDS TaskflowLiteTest
#     USES_TERMINAL
# )


# ============================================================================
#  TaskflowLite 单元测试 CMake —— Catch2 v3 amalgamated 单二进制汇总
# ============================================================================
#
#  设计要点：
#  ----------
#  1. 所有 test_*.cpp 编译进同一可执行文件 TaskflowLiteTest，
#     Catch2 自动收集 TEST_CASE 注册符号。
#  2. 通过 GLOB 自动收集 test_*.cpp，新增测试文件无需修改 CMake。
#  3. 复用上层项目导出的 TaskflowLite::taskflowlite / tfl_internal_flags 目标，
#     若不存在则自动回退到本地包含路径模式。
#  4. CTest 默认带 `-s -d yes` 参数，IDE / CI 直接看到详细输出。
#  5. `run_all_tests` 自定义目标：一键全跑 + 失败时输出。
#  6. 按 Catch2 标签拆分子测试，可通过 ctest -R 选择性执行。
#  7. 可选支持 ASan / TSan，调试并发问题更安全。
#
#  运行方式：
#  ----------
#    ./TaskflowLiteTest                     # 跑全部
#    ./TaskflowLiteTest "[flow]"            # 仅跑 [flow] 标签
#    ./TaskflowLiteTest "[runtime]" -s      # 跑 runtime + 显示成功用例
#    ./TaskflowLiteTest -e "[stress]"       # 排除压力测试
#    ctest --output-on-failure              # 通过 CTest 统一调度
#    ctest -R tfl.flow                      # 仅跑 [flow] 子用例
#    cmake --build . --target run_all_tests # 一键构建+测试
#
#  目录结构假设：
#  --------------
#    project/
#      taskflowlite/        库头文件
#      test/
#        CMakeLists.txt     本文件
#        catch_amalgamated.hpp / .cpp
#        test_common.hpp
#        test_*.cpp
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

# Why: 没设过 build type 时给个合理默认（Release），避免 -O0 跑得过慢。
#      Catch2 + 多线程在 Debug 下能慢一个数量级。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# ----------------------------------------------------------------------------
#  线程库（Catch2 / 测试代码 / TaskflowLite 都需要）
# ----------------------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ----------------------------------------------------------------------------
#  自动收集测试源
#
#  Why: 用 CONFIGURE_DEPENDS 让 GLOB 在新增/删除文件时触发重配，
#       新增 test_xxx.cpp 不再需要手动修改 CMake。
# ----------------------------------------------------------------------------
file(GLOB TFL_TEST_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp"
)

# Catch2 amalgamated 单文件实现（提供 main()）
set(CATCH2_SRC "${CMAKE_CURRENT_SOURCE_DIR}/catch_amalgamated.cpp")
if(NOT EXISTS "${CATCH2_SRC}")
    message(FATAL_ERROR
        "找不到 catch_amalgamated.cpp，请将 Catch2 amalgamated 头文件与源文件放到 ${CMAKE_CURRENT_SOURCE_DIR}/")
endif()

if(NOT TFL_TEST_SOURCES)
    message(FATAL_ERROR
        "未找到任何 test_*.cpp 文件于 ${CMAKE_CURRENT_SOURCE_DIR}/")
endif()

list(APPEND TFL_TEST_SOURCES "${CATCH2_SRC}")

# ----------------------------------------------------------------------------
#  可执行目标
# ----------------------------------------------------------------------------
add_executable(TaskflowLiteTest ${TFL_TEST_SOURCES})

# 让 IDE 把测试源整理到分组下，便于浏览
source_group("Tests" REGULAR_EXPRESSION ".*test_.*\\.cpp$")
source_group("Catch2" REGULAR_EXPRESSION ".*catch_amalgamated.*")

# C++23 特性显式声明（即使前面 CMAKE_CXX_STANDARD 已设，再 PRIVATE 一遍更稳）
target_compile_features(TaskflowLiteTest PRIVATE cxx_std_23)

# ----------------------------------------------------------------------------
#  链接 TaskflowLite 库
#
#  优先复用上层项目导出的 imported target；否则回退到 include 路径模式。
# ----------------------------------------------------------------------------
if(TARGET TaskflowLite::taskflowlite)
    # 上层项目模式：直接复用统一编译选项
    target_link_libraries(TaskflowLiteTest
        PRIVATE
            TaskflowLite::taskflowlite
            Threads::Threads
    )
    # tfl_internal_flags 是项目内部统一管理编译选项的 INTERFACE target，
    # 存在则链接（统一警告/优化级别），不存在则忽略
    if(TARGET tfl_internal_flags)
        target_link_libraries(TaskflowLiteTest PRIVATE tfl_internal_flags)
    endif()
else()
    # 独立构建模式：靠 include 路径定位 header-only 库
    message(STATUS
        "TaskflowLite::taskflowlite target 不可用，回退到 include 路径模式")
    target_include_directories(TaskflowLiteTest
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/..     # taskflowlite/ 头文件
    )
    target_link_libraries(TaskflowLiteTest PRIVATE Threads::Threads)
endif()

# 始终需要的本目录 include 路径（test_common.hpp / Catch2）
target_include_directories(TaskflowLiteTest
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

# ----------------------------------------------------------------------------
#  编译选项 —— 测试目标专属（不污染主库）
# ----------------------------------------------------------------------------
if(MSVC)
    target_compile_options(TaskflowLiteTest PRIVATE
        /W4
        /permissive-
        /Zc:preprocessor       # 标准遵从的预处理器（C++23 必需）
        /utf-8                 # 统一 UTF-8，避免中文注释乱码
        /bigobj                # Catch2 + 大量 TEST_CASE 容易超过 obj 段限制
        $<$<CONFIG:Release>:/O2>
    )
else()
    target_compile_options(TaskflowLiteTest PRIVATE
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter  # 测试中 visitor 经常忽略形参
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:Debug>:-O0 -g>
    )
    # 可选：地址消毒器（调试并发问题特别有用）
    option(TFL_TEST_ENABLE_ASAN "Enable AddressSanitizer for tests" OFF)
    option(TFL_TEST_ENABLE_TSAN "Enable ThreadSanitizer for tests" OFF)
    if(TFL_TEST_ENABLE_ASAN)
        target_compile_options(TaskflowLiteTest PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(TaskflowLiteTest PRIVATE -fsanitize=address)
    endif()
    if(TFL_TEST_ENABLE_TSAN)
        target_compile_options(TaskflowLiteTest PRIVATE
            -fsanitize=thread -fno-omit-frame-pointer)
        target_link_options(TaskflowLiteTest PRIVATE -fsanitize=thread)
    endif()
endif()

# 输出目录统一到 build/bin，便于 IDE 找可执行文件
set_target_properties(TaskflowLiteTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ----------------------------------------------------------------------------
#  CTest 注册
#
#  -s     : 显示成功用例（默认只显示失败）
#  -d yes : 显示每个用例的执行时长，便于发现慢测试
#
#  VS 用户提示：
#    右键项目 -> 属性 -> 调试 -> 命令参数 -> 填入 "-s -d yes"
# ----------------------------------------------------------------------------
include(CTest)
enable_testing()

add_test(
    NAME TaskflowLiteTest
    COMMAND TaskflowLiteTest -s -d yes
    WORKING_DIRECTORY $<TARGET_FILE_DIR:TaskflowLiteTest>
)

# 给慢测试足够余量（默认 1500s，stress 用例预计远低于此）
set_tests_properties(TaskflowLiteTest PROPERTIES
    TIMEOUT 1500
    LABELS "unit;taskflowlite"
)

# ----------------------------------------------------------------------------
#  按 Catch2 标签拆分子测试 —— 便于 ctest -R 选择性执行
#
#  示例：
#    ctest -R tfl.flow      # 仅跑 [flow] 用例
#    ctest -R tfl.runtime   # 仅跑 [runtime] 用例
# ----------------------------------------------------------------------------
set(TFL_TEST_TAGS
    flow task executor runtime async semaphore
    branch jump subflow observer exception
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

# ----------------------------------------------------------------------------
#  自定义构建目标 —— 一键 build + 跑测试
#
#  使用：
#    cmake --build . --target run_all_tests
#    cmake --build . --target run_flow_tests   (仅跑 [flow])
# ----------------------------------------------------------------------------
add_custom_target(run_all_tests
    COMMAND ${CMAKE_CTEST_COMMAND}
            --output-on-failure
            -C $<CONFIG>
            -R "^TaskflowLiteTest$"   # 只跑总用例（避免标签子用例重复跑）
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS TaskflowLiteTest
    USES_TERMINAL
    COMMENT "运行所有 TaskflowLite 单元测试..."
)

# 每个标签也提供一个快捷目标
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

# ----------------------------------------------------------------------------
#  状态摘要 —— 配置完成时打印关键信息
# ----------------------------------------------------------------------------
list(LENGTH TFL_TEST_SOURCES _nfiles)
math(EXPR _nfiles "${_nfiles} - 1")  # 减去 catch_amalgamated.cpp
message(STATUS "================ TaskflowLite Tests ================")
message(STATUS "  测试源文件数      : ${_nfiles}")
message(STATUS "  C++ 标准         : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type       : ${CMAKE_BUILD_TYPE}")
if(NOT MSVC)
    message(STATUS "  ASan             : ${TFL_TEST_ENABLE_ASAN}")
    message(STATUS "  TSan             : ${TFL_TEST_ENABLE_TSAN}")
endif()
message(STATUS "  可执行输出       : ${CMAKE_BINARY_DIR}/bin/TaskflowLiteTest")
message(STATUS "====================================================")
