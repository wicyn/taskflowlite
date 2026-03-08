/// @file 04_runtime.cpp
/// @brief 演示 Runtime 在执行期动态派生任务，展示动态任务如何捕获父任务上下文。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <future>
#include <string>

int main() {
    std::cout << "=== Example 04: Runtime (Dynamic Dispatch) ===\n";

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    tfl::Flow flow;

    std::string global_config = "Config_v1";

    // 父任务捕获外部变量
    flow.emplace([&global_config](tfl::Runtime& rt) {
        std::cout << "[Runtime] Task spawning sub-tasks dynamically...\n";

        // 动态任务 1: 捕获父任务所在的作用域变量
        auto fut1 = rt.async([&global_config] {
            std::cout << "  -> Async 1 reading config: " << global_config << "\n";
            return 42;
        });

        // 动态任务 2: 带参数传递 + Lambda 捕获混用
        auto fut2 = rt.async([base = 10](int multiplier) {
            std::cout << "  -> Async 2 computing...\n";
            return base * multiplier;
        }, 5);

        // 协作式等待：等待子任务完成，线程不会阻塞，会去窃取别的任务
        rt.wait_until([&] {
            return fut1.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
                   fut2.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        });

        std::cout << "[Runtime] Results collected: " << fut1.get() << ", " << fut2.get() << "\n";
    });

    executor.submit(flow).start().wait();
    return 0;
}
