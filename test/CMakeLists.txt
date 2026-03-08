cmake_minimum_required(VERSION 3.21)
project(TaskflowLiteTest)

# 1. 创建可执行文件
add_executable(TaskflowLiteTest test_taskflowlite.cpp catch_amalgamated.cpp)

# 2. 强制指定 C++23 标准
target_compile_features(TaskflowLiteTest PRIVATE cxx_std_23)

# 3. 链接库 (使用内部标志统一管理编译选项)
target_link_libraries(TaskflowLiteTest 
    PRIVATE 
        TaskflowLite::taskflowlite 
        tfl_internal_flags 
        Threads::Threads
)

# 4. 开启 CTest 测试支持
enable_testing()
add_test(NAME TaskflowLiteTest COMMAND TaskflowLiteTest)
