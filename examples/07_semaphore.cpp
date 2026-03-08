/// @file 07_semaphore.cpp
/// @brief 演示 Semaphore 限制任务的最大并发度，展示值捕获与引用捕获的经典混用。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>

int main() {
    std::cout << "=== Example 07: Semaphore (Concurrency Limit) ===\n";

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4); // 4 个物理线程

    tfl::Semaphore dbLimit(2); // 限制只能 2 个任务同时访问
    std::atomic<int> active{0};
    std::mutex coutMutex;

    tfl::Flow flow;

    for (int i = 0; i < 5; ++i) {
        // 核心：引用捕获共享状态 [&active, &coutMutex]，值捕获循环变量 [i]
        auto task = flow.emplace([&active, &coutMutex, i] {
            int current = active.fetch_add(1) + 1;
            {
                std::lock_guard lock(coutMutex);
                std::cout << "[Worker] Task " << i << " executing. Active DB connections: " << current << "\n";
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            active.fetch_sub(1);
        });

        // 声明该任务需要消耗 dbLimit 配额
        task.acquire(dbLimit).release(dbLimit);
    }

    std::cout << "Submitting 5 tasks to 4 threads, but Semaphore limit is 2...\n";
    executor.submit(flow).start().wait();
    return 0;
}
