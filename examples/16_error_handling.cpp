/// @file 16_error_handling.cpp
/// @brief 演示异常处理：AsyncFuture::get、图内传播和 TaskGroup 局部异常处理。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <stdexcept>
#include <syncstream>

int main() {
    std::osyncstream(std::cout) << "=== Example 16: Error Handling ===\n\n";
    tfl::Executor executor(4);

    // ================================================================
    // Part 1: wait 只同步，get 可重复重抛异常
    // ================================================================
    auto failed = executor.async([]() -> int {
        throw std::runtime_error("async task failed");
    });
    failed.wait();
    try {
        (void)failed.get();
    } catch (const std::exception& error) {
        std::osyncstream(std::cout) << "Caught via AsyncFuture::get(): " << error.what() << "\n";
    }

    // ================================================================
    // Part 2: 图内异常由提交句柄接收，依赖后继不会执行
    // ================================================================
    tfl::Flow flow;
    auto bad = flow.emplace([] { throw std::logic_error("graph task failed"); });
    auto downstream = flow.emplace([] {
        std::osyncstream(std::cout) << "This successor must not run\n";
    });
    bad.precede(downstream);
    try {
        executor.async(flow).get();
    } catch (const std::exception& error) {
        std::osyncstream(std::cout) << "Graph exception: " << error.what() << "\n";
    }

    // ================================================================
    // Part 3: TaskGroup 析构协作等待，在局部作用域捕获子任务异常
    // ================================================================
    executor.async([](tfl::Runtime& rt) {
        try {
            tfl::TaskGroup group(rt);
            group.silent_async([] { throw std::runtime_error("child failed"); });
            // 析构等待并重抛；不要让子任务引用超过 group 的生命周期。
        } catch (const std::exception& error) {
            std::osyncstream(std::cout) << "Recovered locally: " << error.what() << "\n";
        }
        std::osyncstream(std::cout) << "Parent can continue after local recovery\n";
    }).get();

    // ================================================================
    // Part 4: 顶层 silent_async 没有结果句柄，需在任务内部自行处理异常
    // ================================================================
    executor.silent_async([] {
        try {
            throw std::runtime_error("fire-and-forget error");
        } catch (const std::exception& error) {
            std::osyncstream(std::cout) << "Handled inside silent_async: " << error.what() << "\n";
        }
    });
    executor.wait_for_all();
    return 0;
}
