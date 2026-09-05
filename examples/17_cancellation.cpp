/// @file 17_cancellation.cpp
/// @brief 演示协作式取消：通过 request_stop() 提前终止任务。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <syncstream>

int main() {
    std::osyncstream(std::cout) << "=== Example 17: Cooperative Cancellation ===\n\n";

    // ================================================================
    // Part 1: 取消长时间运行的 Flow
    // ================================================================
    {
        std::osyncstream(std::cout) << "--- Part 1: Cancel long-running task graph ---\n";

        tfl::Executor executor(4);

        tfl::Flow flow;
        flow.name("Long_Running_Job");

        std::atomic<int> counter{0};

        for (int i = 0; i < 10; ++i) {
            flow.emplace([&counter, i] {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                counter.fetch_add(1, std::memory_order_relaxed);
                std::osyncstream(std::cout) << "  Task " << i << " done, counter="
                          << counter.load() << "\n";
            }).name("Step_" + std::to_string(i));
        }

        auto task = tfl::AsyncTask(flow, 100ULL);
        executor.run(task);

        // 等一小会然后取消
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::osyncstream(std::cout) << "  Requesting stop...\n";
        task.request_stop();

        task.wait();
        executor.wait_for_all();

        std::osyncstream(std::cout) << "  Completed " << counter.load()
                  << " tasks before cancellation\n";
    }

    // ================================================================
    // Part 2: 选择性取消独立异步任务
    // ================================================================
    {
        std::osyncstream(std::cout) << "\n--- Part 2: Selective task cancellation ---\n";

        tfl::Executor executor(4);

        std::atomic<int> a_done{0}, b_done{0}, c_done{0};

        auto task_a = tfl::AsyncTask([&a_done] {
            for (int i = 0; i < 100; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                a_done.fetch_add(1, std::memory_order_relaxed);
            }
        });
        tfl::AsyncTask<void> task_b;
        task_b = tfl::AsyncTask([&b_done, &task_b] {
            for (int i = 0; i < 100 && !task_b.stop_requested(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                b_done.fetch_add(1, std::memory_order_relaxed);
            }
        });
        auto task_c = tfl::AsyncTask([&c_done] {
            for (int i = 0; i < 100; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                c_done.fetch_add(1, std::memory_order_relaxed);
            }
        });

        executor.run(task_a);
        executor.run(task_b);
        executor.run(task_c);

        // 只请求 task_b 停止；其 callable 主动查询句柄状态后退出。
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        task_b.request_stop();

        task_a.wait();
        task_b.wait();
        task_c.wait();
        executor.wait_for_all();

        std::osyncstream(std::cout) << "  Task A: " << a_done.load() << " iterations\n";
        std::osyncstream(std::cout) << "  Task B: " << b_done.load()
                  << " iterations (cancelled early)\n";
        std::osyncstream(std::cout) << "  Task C: " << c_done.load() << " iterations\n";
    }

    // ================================================================
    // Part 3: 图中子循环的停止检测
    // ================================================================
    {
        std::osyncstream(std::cout) << "\n--- Part 3: Stop-detection in loop ---\n";

        tfl::Executor executor(4);

        tfl::Flow flow;
        flow.name("Stopable_Loop");

        std::atomic<int> loop_count{0};
        constexpr int max_loops = 1000;

        flow.emplace([&loop_count] {
            loop_count.fetch_add(1, std::memory_order_relaxed);
        }).name("LoopBody");

        auto pred = [&loop_count, max_loops]() mutable noexcept {
            return loop_count.load(std::memory_order_relaxed) >= max_loops;
        };

        executor.async(flow, pred).wait();
        executor.wait_for_all();

        std::osyncstream(std::cout) << "  Loop completed: " << loop_count.load()
                  << " iterations (target: " << max_loops << ")\n";
    }

    return 0;
}
