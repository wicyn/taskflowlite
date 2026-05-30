/// @file 02_parallel.cpp
/// @brief 演示数据并行——通过 emplace 可变参数一次创建多个任务，利用 atomic 统计完成数。
///   展示 [&] 引用捕获与 [id, base] 值捕获混用的经典模式。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <atomic>

int main() {
    std::cout << "=== Example 02: Parallel Tasks ===\n";

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    tfl::Flow flow;

    std::atomic<int> counter{0};
    int base_value = 100;

    std::cout << "Starting parallel tasks...\n";

    // 演示：使用 Lambda 引用捕获 [&] 和 值捕获 [id, base]
    // 框架的 emplace(Ts&&...) 完美支持已绑定好上下文的闭包
    auto [p1, p2, p3, p4] = flow.emplace(
        [&counter, id=1, base_value] {
            std::cout << "Worker " << id << " computed: " << base_value + id << "\n";
            counter.fetch_add(1, std::memory_order_relaxed);
        },
        [&counter, id=2, base_value] {
            std::cout << "Worker " << id << " computed: " << base_value + id << "\n";
            counter.fetch_add(1, std::memory_order_relaxed);
        },
        [&counter, id=3, base_value] {
            std::cout << "Worker " << id << " computed: " << base_value + id << "\n";
            counter.fetch_add(1, std::memory_order_relaxed);
        },
        [&counter, id=4, base_value] {
            std::cout << "Worker " << id << " computed: " << base_value + id << "\n";
            counter.fetch_add(1, std::memory_order_relaxed);
        }
        );

    executor.async(flow).wait();

    std::cout << "Executed " << counter.load() << " parallel tasks\n";
    return 0;
}
