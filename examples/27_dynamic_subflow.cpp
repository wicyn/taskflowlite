/// @file 27_dynamic_subflow.cpp
/// @brief 演示真正的动态 SubFlow：执行期间构图、显式 run、wait 与父图复用。

#include "../taskflowlite/taskflowlite.hpp"
#include <atomic>
#include <iostream>
#include <syncstream>
int main() {
    std::osyncstream(std::cout) << "=== Example 27: Dynamic SubFlow ===\n";
    tfl::Executor executor(2);
    tfl::Flow flow;
    std::atomic<int> total{0};
    auto dynamic = flow.emplace([&](tfl::SubFlow& sf) {
        auto [a, b] = sf.emplace([&] { total.fetch_add(20); }, [&] { total.fetch_add(22); });
        auto finish = sf.emplace([] { std::osyncstream(std::cout) << "  Dynamic children completed\n"; });
        finish.succeed(a, b);
        sf.run();  // 只有构图而没有 run 时，子节点不会执行
        sf.wait(); // 可选：在本 callable 内继续读取子图结果时使用
    }).name("dynamic");
    flow.emplace([&] { std::osyncstream(std::cout) << "Total: " << total.load() << "\n"; }).succeed(dynamic);
    executor.async(flow, 3ULL).get();  // 每轮自动清空并重建动态子图
    return total.load() == 126 ? 0 : 1;
}
