/// @file 26_task_group.cpp
/// @brief 演示 TaskGroup：作用域等待、Future 依赖与局部异常恢复。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <stdexcept>
#include <syncstream>
int main() {
    std::osyncstream(std::cout) << "=== Example 26: TaskGroup ===\n";
    tfl::Executor executor(1);  // 一个 worker 也能通过协作等待执行子任务
    auto result = executor.async([](tfl::Runtime& rt) {
        tfl::TaskGroup group(rt);
        auto left = group.async([] { return 20; });
        auto right = group.async([] { return 22; });
        auto sum = group.async([left, right] { return left.get() + right.get(); }, left, right);
        group.wait();
        return sum.get();  // 返回值副本，不让借用 group 停止域的句柄逃逸
    });
    std::osyncstream(std::cout) << "Sum: " << result.get() << "\n";

    executor.async([](tfl::Runtime& rt) {
        try {
            tfl::TaskGroup group(rt);
            group.silent_async([] { throw std::runtime_error("local child failure"); });
        } catch (const std::exception& error) {
            std::osyncstream(std::cout) << "Recovered: " << error.what() << "\n";
        }
    }).get();
    return result.get() == 42 ? 0 : 1;
}
