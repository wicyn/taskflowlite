/// @file 19_dependent_async.cpp
/// @brief 演示 AsyncTask / AsyncFuture 依赖、结果传递、句柄转换与动态任务集合。
///
/// AsyncTask<R>：先创建，通过 run() 启动，最多启动一次。
/// AsyncFuture<R>：async() 返回的共享结果句柄，也可由 AsyncTask<R> 转换得到。
///
/// async(callable, deps...) 支持混合传入 AsyncTask / AsyncFuture 前驱，
/// 前驱的结果类型可以不同；结果不会自动传给 callable，需要捕获句柄并 get()。
///
/// 同一结果类型 R 的 AsyncTask<R> / AsyncFuture<R> 可以统一保存为 AsyncFuture<R>。
/// 复制或转换句柄只共享任务，不会复制任务，也不会自动启动尚未运行的 AsyncTask。
///
/// 注意：当前 core 的 run() 内部 _start 仍约束为 AsyncTask 前驱，
///       因此本例的 run() 只传 AsyncTask，Future 或混合前驱使用 async()。

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>
#include <syncstream>
#include "../taskflowlite/taskflowlite.hpp"

int main() {
    std::osyncstream(std::cout) << "=== Example 19: Dependent Async Tasks ===\n\n";

    tfl::Executor executor(4);
    bool success = true;

    auto verify = [&success](const char* label, int actual, int expected) {
        const bool passed = actual == expected;
        success = success && passed;

        std::osyncstream(std::cout) << "  " << label << ": " << actual << " (expected " << expected << ")"
                  << (passed ? " [PASS]\n" : " [FAIL]\n");
    };

    // ================================================================
    // Part 1: run() 线性依赖链 — AsyncTask -> AsyncTask -> AsyncTask
    // ================================================================
    {
        std::osyncstream(std::cout) << "--- Part 1: Linear AsyncTask chain ---\n";

        auto task1 = tfl::AsyncTask([] {
            std::osyncstream(std::cout) << "  Task1 executing...\n";
            return 1;
        });

        auto task2 = tfl::AsyncTask([task1] {
            // task1 是已声明的前驱，进入此 callable 时已经完成。
            std::osyncstream(std::cout) << "  Task2 executing (after Task1)...\n";
            return task1.get() + 10;
        });

        auto task3 = tfl::AsyncTask([task2] {
            std::osyncstream(std::cout) << "  Task3 executing (after Task2)...\n";
            return task2.get() + 100;
        });

        // 可以先提交后继，再启动根任务。
        // 注册依赖不会自动启动尚未运行的前驱。
        executor.run(task3, task2);
        executor.run(task2, task1);
        executor.run(task1);

        verify("Chain result", task3.get(), 111);
        executor.wait_for_all();
    }

    // ================================================================
    // Part 2: run() 扇入依赖 — 多个 AsyncTask 前驱
    // ================================================================
    {
        std::osyncstream(std::cout) << "\n--- Part 2: AsyncTask fan-in ---\n";

        auto a = tfl::AsyncTask([] { return 1; });
        auto b = tfl::AsyncTask([] { return 2; });
        auto c = tfl::AsyncTask([] { return 4; });

        auto merger = tfl::AsyncTask([a, b, c] { return a.get() + b.get() + c.get(); });

        executor.run(merger, a, b, c);
        executor.run(a);
        executor.run(b);
        executor.run(c);

        verify("Fan-in result", merger.get(), 7);
        executor.wait_for_all();
    }

    // ================================================================
    // Part 3: async() 依赖链与钻石依赖 — AsyncFuture 前驱
    //
    //               -> left  --
    //   root -------           -> merged
    //               -> right --
    // ================================================================
    {
        std::osyncstream(std::cout) << "\n--- Part 3: AsyncFuture diamond ---\n";

        // async() 直接提交，返回 AsyncFuture<int>，无需再调用 run()。
        auto root = executor.async([] { return 1; });

        auto left = executor.async([root] { return root.get() + 10; }, root);

        auto right = executor.async([root] { return root.get() + 100; }, root);

        auto merged =
            executor.async([left, right] { return left.get() + right.get(); }, left, right);

        verify("Diamond result", merged.get(), 112);

        // get() 不消费结果，句柄仍然有效，可以重复读取。
        verify("Repeated get", merged.get(), 112);

        // 已经完成的 Future 仍可作为依赖，视为该依赖已经满足。
        auto next = executor.async([merged] { return merged.get() + 1; }, merged);

        verify("Completed predecessor", next.get(), 113);
        executor.wait_for_all();
    }

    // ================================================================
    // Part 4: async() 混合依赖 — AsyncTask + AsyncFuture
    // ================================================================
    {
        std::osyncstream(std::cout) << "\n--- Part 4: Mixed predecessor handles ---\n";

        auto delayed = tfl::AsyncTask([] { return 20; });

        auto immediate = executor.async([] { return 22; });

        // 从 AsyncTask<int> 隐式转换为 AsyncFuture<int>。
        // 两个句柄共享同一任务；转换本身不会启动 delayed。
        tfl::AsyncFuture<int> delayed_future = delayed;

        // 依赖列表可以直接混合 AsyncTask 和 AsyncFuture。
        auto sum = executor.async(
            [delayed_future, immediate] { return delayed_future.get() + immediate.get(); }, delayed,
            immediate);

        // 转换后的 Future 也可以直接作为前驱。
        auto after_conversion =
            executor.async([delayed_future] { return delayed_future.get() + 1; }, delayed_future);

        // 两个后继都已经提交，但 delayed 仍需显式启动。
        executor.run(delayed);

        verify("Mixed dependency result", sum.get(), 42);
        verify("Converted Future predecessor", after_conversion.get(), 21);
        executor.wait_for_all();
    }

    // ================================================================
    // Part 5: 统一 AsyncFuture — 复制、赋值、移动与共享生命周期
    // ================================================================
    {
        std::osyncstream(std::cout) << "\n--- Part 5: Shared AsyncFuture handles ---\n";

        auto task = tfl::AsyncTask([] { return 7; });

        // AsyncTask -> AsyncFuture：复制基类句柄，共享同一任务。
        tfl::AsyncFuture<int> from_task = task;

        // AsyncFuture -> AsyncFuture：复制句柄，共享同一结果。
        tfl::AsyncFuture<int> from_future = from_task;

        // 两种来源均支持赋值到 AsyncFuture。
        tfl::AsyncFuture<int> assigned_from_task;
        tfl::AsyncFuture<int> assigned_from_future;

        assigned_from_task = task;
        assigned_from_future = from_future;

        // 必须在移动掉唯一可用于启动的 AsyncTask 句柄前启动任务。
        executor.run(task);

        // AsyncFuture -> AsyncFuture：转移句柄，源 Future 变为空。
        tfl::AsyncFuture<int> moved_future = std::move(from_future);

        // 已启动的 AsyncTask -> AsyncFuture：转移基类持有的共享状态。
        // task 随后变为空，但底层任务继续执行。
        tfl::AsyncFuture<int> moved_task = std::move(task);

        verify("Copied from AsyncTask", from_task.get(), 7);
        verify("Assigned from AsyncTask", assigned_from_task.get(), 7);
        verify("Assigned from AsyncFuture", assigned_from_future.get(), 7);
        verify("Moved from AsyncFuture", moved_future.get(), 7);
        verify("Moved from AsyncTask", moved_task.get(), 7);

        const bool same_task = from_task == assigned_from_task &&
                               from_task == assigned_from_future && from_task == moved_future &&
                               from_task == moved_task;

        const bool moved_sources_empty = !task.valid() && !from_future.valid();

        std::osyncstream(std::cout) << "  Shared task identity: " << (same_task ? "PASS" : "FAIL") << "\n";
        std::osyncstream(std::cout) << "  Moved sources are empty: " << (moved_sources_empty ? "PASS" : "FAIL")
                  << "\n";

        success = success && same_task && moved_sources_empty;

        // 释放部分句柄不会影响其他共享句柄。
        from_task.reset();
        assigned_from_task.reset();
        assigned_from_future.reset();
        moved_future.reset();

        verify("Remaining shared handle", moved_task.get(), 7);
        executor.wait_for_all();
    }

    // ================================================================
    // Part 6: 动态任务集合 — 统一保存为 vector<AsyncFuture<int>>
    // ================================================================
    {
        std::osyncstream(std::cout) << "\n--- Part 6: Dynamic mixed task collection ---\n";

        constexpr int N = 6;

        std::vector<tfl::AsyncFuture<int>> workers;
        workers.reserve(N);

        for (int i = 0; i < N; ++i) {
            if (i % 2 == 0) {
                auto task = tfl::AsyncTask([i] { return i + 1; });

                // 将 AsyncTask 转为 Future 保存，仍使用 task 启动。
                workers.push_back(task);
                executor.run(task);
            } else {
                // async() 返回的 Future 直接放入同一容器。
                workers.push_back(executor.async([i] { return i + 1; }));
            }
        }

        // 当前没有依赖迭代器重载。
        // 动态数量的 Future 通过 Runtime 协作等待，再汇总结果。
        // 这不是给 async() 传入依赖列表，而是在任务内部等待集合完成。
        auto reducer = executor.async([workers](tfl::Runtime& rt) {
            rt.wait_until([&workers] {
                return std::all_of(workers.begin(), workers.end(),
                                   [](const auto& future) { return future.done(); });
            });

            int total = 0;
            for (const auto& future : workers) {
                // 上面已经确认完成；get() 用于取结果并传播异常。
                total += future.get();
            }
            return total;
        });

        verify("Dynamic collection sum", reducer.get(), N * (N + 1) / 2);
        executor.wait_for_all();
    }

    // ================================================================
    // Part 7: Flow 依赖 — AsyncTask 包装与 async() 图重载
    // ================================================================
    {
        std::osyncstream(std::cout) << "\n--- Part 7: Flow with dependencies ---\n";

        int graph_runs = 0;
        int callbacks = 0;

        auto pre = tfl::AsyncTask([] { return 10; });

        // 前驱可以具有不同的结果类型：pre 为 int，ready 为 void。
        auto ready = executor.async([] {});

        tfl::Flow flow;
        flow.name("Dependent_Flow");

        flow.emplace([&graph_runs] { ++graph_runs; }).name("FlowStep");

        auto callback = [&callbacks] { ++callbacks; };

        // 形式 1：将 Flow 包装为 AsyncTask，通过 run() 添加前驱。
        auto flow_task = tfl::AsyncTask(flow);
        executor.run(flow_task, pre);
        executor.run(pre);
        flow_task.get();
        executor.wait_for_all();

        // 后续每次都等前一次完全结束，再复用同一个 Flow。
        // 下面六种 async() 形式都支持 AsyncTask / AsyncFuture 混合前驱。

        // 形式 2：执行一次。
        executor.async(flow, pre, ready).get();
        executor.wait_for_all();

        // 形式 3：执行一次，完成后调用 callback。
        executor.async(flow, callback, pre, ready).get();
        executor.wait_for_all();

        // 形式 4：执行指定次数。
        executor.async(flow, std::uint64_t{2}, pre, ready).get();
        executor.wait_for_all();

        // 形式 5：执行指定次数，完成后调用一次 callback。
        executor.async(flow, std::uint64_t{2}, callback, pre, ready).get();
        executor.wait_for_all();

        // 形式 6：按谓词循环；返回 true 停止，返回 false 执行下一轮。
        executor.async(
                    flow, [remaining = 2]() mutable { return remaining-- == 0; }, pre, ready)
            .get();
        executor.wait_for_all();

        // 形式 7：按谓词循环，全部完成后调用一次 callback。
        executor
            .async(
                flow, [remaining = 2]() mutable { return remaining-- == 0; }, callback, pre, ready)
            .get();
        executor.wait_for_all();

        // 1 + 1 + 1 + 2 + 2 + 2 + 2 = 11。
        verify("Flow executions", graph_runs, 11);
        verify("Completion callbacks", callbacks, 3);
    }

    // ================================================================
    // Part 8: Runtime 内部的 async() 依赖与协作等待
    // ================================================================
    {
        std::osyncstream(std::cout) << "\n--- Part 8: Runtime child dependencies ---\n";

        auto parent = executor.async([](tfl::Runtime& rt) {
            auto a = rt.async([] { return 10; });

            auto b = tfl::AsyncTask([] { return 20; });

            tfl::AsyncFuture<int> b_future = b;

            // Runtime::async 同样支持 Future 前驱。
            auto merged = rt.async([a, b_future] { return a.get() + b_future.get(); }, a, b_future);

            // b_future 的转换和依赖注册都不会自动启动 b。
            rt.run(b);

            // 在 Worker 内使用协作等待，等待期间继续推进其他任务。
            // 不应在这里直接阻塞等待尚未完成的 merged。
            rt.wait();

            return merged.get();
        });

        verify("Runtime dependency result", parent.get(), 30);
        executor.wait_for_all();
    }

    std::osyncstream(std::cout) << "\n=== Dependent Async " << (success ? "Complete" : "FAILED") << " ===\n";

    return success ? 0 : 1;
}
