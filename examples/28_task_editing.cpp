/// @file 28_task_editing.cpp
/// @brief 演示 FlowBuilder 与 Task 编辑：placeholder、work、linearize、erase。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <syncstream>
int main() {
    std::osyncstream(std::cout) << "=== Example 28: Task Editing ===\n";
    tfl::Executor executor(2);
    tfl::Flow flow;
    int result = 0;
    auto first = flow.placeholder().name("first");
    auto second = flow.placeholder().name("second");
    auto last = flow.placeholder().name("last");
    flow.linearize(first, second, last);
    first.work([&] { result = 20; });
    second.work([&] { result += 22; });
    last.work([&] { std::osyncstream(std::cout) << "Result: " << result << "\n"; });
    executor.async(flow).get();
    if (result != 42) return 1;

    // 必须等待本轮完成后修改图。erase 会断开双向边，但不会自动连接两侧。
    flow.erase(second);
    second.reset();  // erase 后原句柄悬空，不得再查询节点
    first.precede(last);
    first.work([&] { result = 7; });
    executor.async(flow).get();
    return result == 7 ? 0 : 1;
}
