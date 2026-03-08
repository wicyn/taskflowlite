/// @file 01_basic_dag.cpp
/// @brief 演示最基础的 DAG (有向无环图) 构建，展示 Lambda 捕获与 emplace 传参的混用。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <string>

int main() {
    std::cout << "=== Example 01: Basic DAG ===\n";

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    tfl::Flow flow;
    flow.name("Basic_DAG_Flow");

    std::string shared_context = "Init_Done";

    // 方式 1：Lambda 引用捕获 [&] (最符合直觉的 C++ 写法)
    auto A = flow.emplace([&shared_context] {
                     std::cout << "Task A (Init). Context: " << shared_context << "\n";
                     shared_context = "Process_Ready";
                 }).name("Task_A");

    // 方式 2：emplace 参数转发 (框架负责拷贝参数)
    auto B = flow.emplace([](int id) {
                     std::cout << "Task B (Process " << id << ")\n";
                 }, 1).name("Task_B");

    // 方式 3：Lambda 值捕获与初始化捕获 [x = ...]
    auto C = flow.emplace([id = 2, &shared_context] {
                     std::cout << "Task C (Process " << id << "). Context: " << shared_context << "\n";
                 }).name("Task_C");

    // 方式 4：混合使用
    auto D = flow.emplace([&shared_context](const std::string& msg) {
                     std::cout << "Task D (Merge): " << msg << " | Final Context: " << shared_context << "\n";
                 }, "All processes done!").name("Task_D");

    // 构建依赖关系：A -> (B, C) -> D
    A.precede(B, C);
    D.succeed(B, C);

    executor.submit(flow).start().wait();

    std::cout << "All tasks completed!\n";
    return 0;
}
