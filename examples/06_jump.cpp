/// @file 06_jump.cpp
/// @brief 演示 Jump 节点破坏静态图并回退，展示基于捕获的状态重试。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>

int main() {
    std::cout << "=== Example 06: Jump (Retry Loop) ===\n";

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 2);
    tfl::Flow flow;

    int attempts = 0;
    constexpr int maxAttempts = 3;

    auto init = flow.emplace([] { std::cout << "Initializing system...\n"; });

    // 捕获 attempts 变量
    auto process = flow.emplace([&attempts] {
        std::cout << "Processing (attempt " << ++attempts << ")...\n";
    });

    // Jump 节点：通过引用捕获外部变量判断逻辑
    auto check = flow.emplace([&attempts, maxAttempts](tfl::Jump& jmp) {
        if (attempts < maxAttempts) {
            std::cout << "[Check] Failed. Jumping back to Process!\n";
            // 假设框架提供 jmp.to(Task) 或是基于依赖图的前向寻找机制
            // (依据具体框架 API 适配，这里以伪代码说明意图)
            jmp.to(0); // 假定 0 索引指向 process
        } else {
            std::cout << "[Check] Passed!\n";
        }
    });

    auto success = flow.emplace([] { std::cout << "Operation succeeded!\n"; });

    init.precede(process);
    process.precede(check);
    check.precede(success);

    // 假设绑定目标为 process
    // check.set_target(process);

    executor.submit(flow).start().wait();
    return 0;
}
