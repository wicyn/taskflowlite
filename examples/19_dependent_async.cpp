/// @file 19_dependent_async.cpp
/// @brief 演示 AsyncTask 依赖链 —— 通过 submit(task, deps...) 动态构建多级依赖。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <atomic>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

int main() {
    std::cout << "=== Example 19: Dependent Async Tasks ===\n\n";

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);

    // ================================================================
    // Part 1: 线性依赖链 t1 -> t2 -> t3
    // ================================================================
    {
        std::cout << "--- Part 1: Linear dependency chain ---\n";

        std::atomic<int> sequence{0};

        auto task1 = tfl::NonrepeatAsyncTask([&sequence] {
            std::cout << "  Task1 executing...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            sequence.fetch_add(1, std::memory_order_relaxed);
        });

        auto task2 = tfl::NonrepeatAsyncTask([&sequence] {
            std::cout << "  Task2 executing (after Task1)...\n";
            sequence.fetch_add(10, std::memory_order_relaxed);
        });

        auto task3 = tfl::NonrepeatAsyncTask([&sequence] {
            std::cout << "  Task3 executing (after Task2)...\n";
            sequence.fetch_add(100, std::memory_order_relaxed);
        });

        executor.submit(task1);           // root task, no deps
        executor.submit(task2, task1);   // task2 depends on task1
        executor.submit(task3, task2);   // task3 depends on task2

        task3.wait();
        executor.wait_for_all();

        std::cout << "  Sequence: " << sequence.load()
                  << " (expected 111: 1+10+100)\n";
    }

    // ================================================================
    // Part 2: 扇入依赖 — 多个前驱完成后才执行
    // ================================================================
    {
        std::cout << "\n--- Part 2: Fan-in (multiple predecessors) ---\n";

        std::atomic<int> result{0};

        auto a = tfl::NonrepeatAsyncTask([&result] {
            std::cout << "  Branch A...\n";
            result.fetch_add(1, std::memory_order_relaxed);
        });
        auto b = tfl::NonrepeatAsyncTask([&result] {
            std::cout << "  Branch B...\n";
            result.fetch_add(2, std::memory_order_relaxed);
        });
        auto c = tfl::NonrepeatAsyncTask([&result] {
            std::cout << "  Branch C...\n";
            result.fetch_add(4, std::memory_order_relaxed);
        });

        // merger waits for a, b, c to all complete
        auto merger = tfl::NonrepeatAsyncTask([&result] {
            std::cout << "  Merge: all three branches done, sum="
                      << result.load() << "\n";
        });

        executor.submit(a);
        executor.submit(b);
        executor.submit(c);
        executor.submit(merger, a, b, c);

        merger.wait();
        executor.wait_for_all();
    }

    // ================================================================
    // Part 3: 迭代器范围 — 动态数量的依赖任务
    // ================================================================
    {
        std::cout << "\n--- Part 3: Iterator-range dependencies ---\n";

        constexpr int N = 5;
        std::vector<tfl::NonrepeatAsyncTask> workers;
        workers.reserve(N);

        std::atomic<int> worker_done{0};

        for (int i = 0; i < N; ++i) {
            workers.push_back(tfl::NonrepeatAsyncTask([&worker_done, i] {
                std::cout << "  Worker " << i << " working...\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                worker_done.fetch_add(1, std::memory_order_relaxed);
            }));
        }

        // Submit all workers, then reducer dependent on them all
        for (auto& w : workers) executor.submit(w);

        auto reducer = tfl::NonrepeatAsyncTask([&worker_done, N] {
            std::cout << "  Reducer: all " << N << " workers done ("
                      << worker_done.load() << ")\n";
        });

        executor.submit(reducer, workers.begin(), workers.end());

        reducer.wait();
        executor.wait_for_all();
    }

    // ================================================================
    // Part 4: 钻石依赖 (Diamond DAG)
    //   t1 -> t2 -> t4
    //   t1 -> t3 -> t4
    // ================================================================
    {
        std::cout << "\n--- Part 4: Diamond dependency ---\n";

        std::atomic<int> diamond_counter{0};

        auto t1 = tfl::NonrepeatAsyncTask([&diamond_counter] {
            diamond_counter.store(1, std::memory_order_relaxed);
        });
        auto t2 = tfl::NonrepeatAsyncTask([&diamond_counter] {
            diamond_counter.fetch_add(10, std::memory_order_relaxed);
        });
        auto t3 = tfl::NonrepeatAsyncTask([&diamond_counter] {
            diamond_counter.fetch_add(100, std::memory_order_relaxed);
        });
        auto t4 = tfl::NonrepeatAsyncTask([&diamond_counter] {
            std::cout << "  Diamond complete: counter="
                      << diamond_counter.load() << " (expected 111)\n";
        });

        executor.submit(t1);
        executor.submit(t2, t1);
        executor.submit(t3, t1);
        executor.submit(t4, t2, t3);

        t4.wait();
        executor.wait_for_all();
    }

    // ================================================================
    // Part 5: Flow 依赖 AsyncTask
    // ================================================================
    {
        std::cout << "\n--- Part 5: Flow dependent on AsyncTask ---\n";

        std::atomic<int> pre_work_done{0};

        auto pre = tfl::NonrepeatAsyncTask([&pre_work_done] {
            std::cout << "  Pre-work executing...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            pre_work_done.store(1, std::memory_order_relaxed);
        });

        tfl::Flow flow;
        flow.name("Dependent_Flow");
        flow.emplace([&pre_work_done] {
            std::cout << "  Flow step: pre_work_done="
                      << pre_work_done.load() << "\n";
        }).name("FlowStep");

        // Flow as NonrepeatAsyncTask, depending on pre
        executor.submit(pre);
        auto flow_task = tfl::NonrepeatAsyncTask(flow);
        executor.submit(flow_task, pre);

        flow_task.wait();
        executor.wait_for_all();
    }

    std::cout << "\n=== Dependent Async Complete ===\n";
    return 0;
}
