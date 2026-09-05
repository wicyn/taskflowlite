/// @file 08_subflow.cpp
/// @brief 演示子图（Subflow）嵌套——emplace(Flow, N) 将子图作为节点挂入父图并循环 N 次。
///   展示主图与子图通过 Lambda 引用捕获共享同一变量的经典模式。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <syncstream>

int main() {
    std::osyncstream(std::cout) << "=== Example 08: Subflow (Nested Flow) ===\n";

    tfl::Executor executor(4);

    int global_record = 0; // 主图和子图共享的状态

    // 1. 构建子图
    tfl::Flow subflow;
    subflow.name("Data_Processor_SubFlow");

    // 子图内部捕获 global_record
    auto s_a = subflow.emplace([&global_record] {
        std::osyncstream(std::cout) << "    [Subflow] Step A. Record: " << ++global_record << "\n";
    });
    auto s_b = subflow.emplace([&global_record] {
        std::osyncstream(std::cout) << "    [Subflow] Step B. Record: " << ++global_record << "\n";
    });
    s_a.precede(s_b);

    // 2. 构建主图
    tfl::Flow main_flow;
    main_flow.name("Main_App");

    // 主图也捕获 global_record
    auto start = main_flow.emplace([&global_record] {
        std::osyncstream(std::cout) << "[Main] System Boot. Record: " << global_record << "\n";
    });

    // 3. 将子图挂载到主图中，循环 2 次
    auto subflow_task = main_flow.emplace(std::move(subflow), 2ULL).name("Subflow_Container");

    auto end = main_flow.emplace([&global_record] {
        std::osyncstream(std::cout) << "[Main] System Shutdown. Final Record: " << global_record << "\n";
    });

    start.precede(subflow_task);
    subflow_task.precede(end);

    executor.async(main_flow).wait();
    return 0;
}
