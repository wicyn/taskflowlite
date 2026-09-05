/// @file 03_loop.cpp
/// @brief 演示任务图循环执行——async(flow, N) 固定次数循环与 async(flow, predicate) 条件谓词循环。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <syncstream>

int main() {
    std::osyncstream(std::cout) << "=== Example 03: Loop Execution ===\n";
    tfl::Executor executor(4);

    // 模式 1：固定次数循环
    {
        tfl::Flow flow;
        flow.emplace([c = 0]() mutable {
            std::osyncstream(std::cout) << "Fixed Iteration #" << ++c << "\n";
        });

        std::osyncstream(std::cout) << "--- Running flow 3 times ---\n";
        executor.async(flow, 3ULL).wait();
    }

    // 模式 2：条件谓词循环
    {
        tfl::Flow flow;
        int state = 0;

        flow.emplace([&state] {
            state += 10;
            std::osyncstream(std::cout) << "Predicate Loop State: " << state << "\n";
        });

        std::osyncstream(std::cout) << "\n--- Running flow until state >= 30 ---\n";
        executor.async(flow, [&state]() noexcept {
            return state >= 30;
        }).wait();
    }

    std::osyncstream(std::cout) << "Loop examples complete!\n";
    return 0;
}
