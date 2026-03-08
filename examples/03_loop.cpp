/// @file 03_loop.cpp
/// @brief 演示任务图循环执行。展示 mutable lambda 捕获与自定义谓词。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>

int main() {
    std::cout << "=== Example 03: Loop Execution ===\n";

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);

    // --- 模式 1：固定次数循环 (结合 mutable lambda) ---
    {
        tfl::Flow flow;
        // 使用 mutable lambda 允许修改值捕获的变量 (仅限本次运行期内有效)
        flow.emplace([c = 0]() mutable {
            std::cout << "Fixed Iteration #" << ++c << "\n";
        });

        std::cout << "--- Running flow 3 times ---\n";
        executor.submit(flow, 3ULL).start().wait();
    }

    // --- 模式 2：条件谓词循环 (外部引用捕获) ---
    {
        tfl::Flow flow;
        int state = 0; // 外部状态

        flow.emplace([&state] {
            state += 10;
            std::cout << "Predicate Loop State: " << state << "\n";
        });

        std::cout << "\n--- Running flow until state >= 30 ---\n";
        // 谓词：Lambda 引用捕获 state。只要返回 false，就会继续执行
        executor.submit(flow, [&state]() noexcept {
                    return state >= 30;
                }).start().wait();
    }

    std::cout << "Loop examples complete!\n";
    return 0;
}
