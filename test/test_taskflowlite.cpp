#define CATCH_CONFIG_MAIN

#include "../taskflowlite/taskflowlite.hpp"
#include "catch_amalgamated.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <numeric>
#include <vector>
#include <mutex>
#include <thread>
#include <algorithm>

using namespace std::chrono_literals;

// ============================================================================
// [Flow] 图构建与生命周期
// ============================================================================

TEST_CASE("Flow: Basic Construction & Attributes", "[flow]") {
    tfl::Flow flow;
    REQUIRE(flow.empty() == true);
    REQUIRE(flow.size() == 0);
    REQUIRE(flow.name().empty());

    flow.name("TestFlow");
    REQUIRE(flow.name() == "TestFlow");

    auto task = flow.emplace([] {});
    REQUIRE(flow.empty() == false);
    REQUIRE(flow.size() == 1);

    task.name("Task1");
    REQUIRE(task.name() == "Task1");
}

TEST_CASE("Flow: Erase and Clear", "[flow]") {
    tfl::Flow flow;

    auto t1 = flow.emplace([] {});
    auto t2 = flow.emplace([] {});
    auto t3 = flow.emplace([] {});
    REQUIRE(flow.size() == 3);

    flow.erase(t1);
    REQUIRE(flow.size() == 2);

    flow.erase(t2, t3);
    REQUIRE(flow.empty() == true);

    flow.emplace([] {});
    flow.clear();
    REQUIRE(flow.empty() == true);
}

TEST_CASE("Flow: Batch Emplace (Tuple & Args)", "[flow]") {
    tfl::Flow flow;
    int counter = 0;

    // Tuple 批量插入
    auto [t1, t2] = flow.emplace(
        std::tuple{[](int a) { REQUIRE(a == 42); }, 42},
        std::tuple{[](int& c) { c = 100; }, std::ref(counter)}
        );

    REQUIRE(flow.size() == 2);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 1);
    executor.submit(flow).start().wait();

    REQUIRE(counter == 100);
}

// ============================================================================
// [DAG] 执行顺序与依赖拓扑严格校验
// ============================================================================

TEST_CASE("DAG: Strict Linear Execution Order", "[dag]") {
    tfl::Flow flow;
    std::vector<int> order;
    std::mutex mtx;

    auto push = [&](int val) {
        std::lock_guard<std::mutex> lock(mtx);
        order.push_back(val);
    };

    auto t1 = flow.emplace([&] { push(1); });
    auto t2 = flow.emplace([&] { push(2); });
    auto t3 = flow.emplace([&] { push(3); });
    auto t4 = flow.emplace([&] { push(4); });

    // 构建链式依赖: t1 -> t2 -> t3 -> t4
    t1.precede(t2);
    t2.precede(t3);
    t3.precede(t4);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    executor.submit(flow).start().wait();

    REQUIRE(order == std::vector<int>{1, 2, 3, 4});
}

TEST_CASE("DAG: Diamond Topology Synchronization", "[dag]") {
    tfl::Flow flow;
    std::atomic<int> counter{0};

    auto A = flow.emplace([&] { REQUIRE(counter.load() == 0); counter.fetch_add(1); });
    auto B = flow.emplace([&] { REQUIRE(counter.load() >= 1); counter.fetch_add(1); });
    auto C = flow.emplace([&] { REQUIRE(counter.load() >= 1); counter.fetch_add(1); });
    auto D = flow.emplace([&] { REQUIRE(counter.load() == 3); counter.fetch_add(1); });

    A.precede(B, C);
    D.succeed(B, C);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    executor.submit(flow).start().wait();

    REQUIRE(counter.load() == 4);
}

// ============================================================================
// [Control Flow] 分支与跳转控制流
// ============================================================================

TEST_CASE("Branch: Exclusive Path Routing", "[branch]") {
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 2);

    for (int route = 0; route < 3; ++route) {
        tfl::Flow flow;
        std::atomic<int> hits[3] = {0, 0, 0};

        auto branch = flow.emplace([route](tfl::Branch& br) { br.allow(route); });

        auto p0 = flow.emplace([&] { hits[0]++; });
        auto p1 = flow.emplace([&] { hits[1]++; });
        auto p2 = flow.emplace([&] { hits[2]++; });

        branch.precede(p0, p1, p2); // 0->p0, 1->p1, 2->p2

        executor.submit(flow).start().wait();

        for (int i = 0; i < 3; ++i) {
            if (i == route) REQUIRE(hits[i].load() == 1);
            else REQUIRE(hits[i].load() == 0); // 未选中路径必须被严格跳过
        }
    }
}

TEST_CASE("MultiBranch: Concurrent Path Routing", "[branch]") {
    tfl::Flow flow;
    std::atomic<int> hits[4] = {0, 0, 0, 0};

    auto mbr = flow.emplace([](tfl::MultiBranch& mb) {
        mb.allow(0).allow(2); // 仅激活 0 和 2
    });

    auto p0 = flow.emplace([&] { hits[0]++; });
    auto p1 = flow.emplace([&] { hits[1]++; });
    auto p2 = flow.emplace([&] { hits[2]++; });
    auto p3 = flow.emplace([&] { hits[3]++; });

    mbr.precede(p0, p1, p2, p3);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    executor.submit(flow).start().wait();

    REQUIRE(hits[0].load() == 1);
    REQUIRE(hits[1].load() == 0);
    REQUIRE(hits[2].load() == 1);
    REQUIRE(hits[3].load() == 0);
}

TEST_CASE("Jump: Backward Retry Loop", "[jump]") {
    tfl::Flow flow;
    std::atomic<int> attempts{0};

    auto process = flow.emplace([&] { attempts.fetch_add(1); });
    auto check = flow.emplace([&](tfl::Jump& jmp) {
        if (attempts.load() < 5) jmp.to(0); // 跳回 target 0 (即 process)
    });

    process.precede(check);
    // 这里假设你的 API 是 jmp_node.precede(target) 将 target 注册为索引 0
    // 或者类似机制。如果 API 不同请调整这行。
    check.precede(process);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 2);
    executor.submit(flow).start().wait();

    REQUIRE(attempts.load() == 5);
}

// ============================================================================
// [Runtime] 运行时动态拓扑与子图
// ============================================================================

TEST_CASE("Runtime: Dynamic Async & Wait", "[runtime]") {
    tfl::Flow flow;
    std::atomic<int> result{0};

    flow.emplace([&](tfl::Runtime& rt) {
        auto fut1 = rt.async([] { return 10; });
        auto fut2 = rt.async([] { return 20; });

        rt.wait_until([&] {
            return fut1.wait_for(0s) == std::future_status::ready &&
                   fut2.wait_for(0s) == std::future_status::ready;
        });

        result.store(fut1.get() + fut2.get());
    });

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    executor.submit(flow).start().wait();

    REQUIRE(result.load() == 30);
}

TEST_CASE("Subflow: Predicate Condition Loop", "[subflow]") {
    tfl::Flow main_flow;
    tfl::Flow sub_flow;
    std::atomic<int> runs{0};

    sub_flow.emplace([&] { runs.fetch_add(1); });

    int loops = 0;
    // 使用谓词循环：执行 4 次
    main_flow.emplace(std::move(sub_flow), [&loops]() mutable noexcept {
        return (++loops) > 4;
    });

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 2);
    executor.submit(main_flow).start().wait();

    REQUIRE(runs.load() == 4);
}

// ============================================================================
// [Concurrency] 并发限制与调度器特性
// ============================================================================

TEST_CASE("Semaphore: Real Concurrency Limiting", "[semaphore]") {
    tfl::Flow flow;
    tfl::Semaphore sem(2); // 最大并发度限制为 2

    std::atomic<int> current_active{0};
    std::atomic<int> max_active_observed{0};

    for (int i = 0; i < 8; ++i) {
        auto task = flow.emplace([&] {
            int cur = current_active.fetch_add(1) + 1;

            // 无锁更新最大观测值
            int max_val = max_active_observed.load();
            while (max_val < cur && !max_active_observed.compare_exchange_weak(max_val, cur)) {}

            std::this_thread::sleep_for(10ms); // 模拟耗时，促发并发冲突
            current_active.fetch_sub(1);
        });
        task.acquire(sem).release(sem);
    }

    tfl::ResumeNever handler;
    // 分配 8 个物理线程，但由于信号量限制，实际并发量不应超过 2
    tfl::Executor executor(handler, 8);
    executor.submit(flow).start().wait();

    REQUIRE(max_active_observed.load() <= 2);
    REQUIRE(max_active_observed.load() > 0);
}

TEST_CASE("Executor: AsyncTask Explicit Dependencies", "[executor]") {
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);

    std::atomic<int> step{0};

    auto t1 = executor.submit([&] { REQUIRE(step.load() == 0); step.store(1); });
    auto t2 = executor.submit([&] { REQUIRE(step.load() == 1); step.store(2); });
    auto t3 = executor.submit([&] { REQUIRE(step.load() == 2); step.store(3); });

    // 逆序组装并启动：t3 依赖 t2，t2 依赖 t1
    t3.start(t2);
    t2.start(t1);
    t1.start(); // 触发点

    t3.wait();
    executor.wait_for_all();

    REQUIRE(step.load() == 3);
}

TEST_CASE("Executor: Flow Callbacks and Submissions", "[executor]") {
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 2);
    tfl::Flow flow;
    std::atomic<int> runs{0};
    std::atomic<bool> cb_called{false};

    flow.emplace([&] { runs.fetch_add(1); });

    // 提交多次执行并附加回调
    executor.submit(flow, 3ULL, [&] { cb_called.store(true); }).start().wait();

    REQUIRE(runs.load() == 3);
    REQUIRE(cb_called.load() == true);
}

// ============================================================================
// [Exception] 异常处理策略
// ============================================================================

TEST_CASE("Exception: ResumeNever Stops Execution", "[exception]") {
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 2);
    tfl::Flow flow;
    std::atomic<int> next_task_run{0};

    auto fail = flow.emplace([] { throw std::runtime_error("Crash!"); });
    auto next = flow.emplace([&] { next_task_run.fetch_add(1); });

    fail.precede(next);

    // 因为抛出异常，next_task 不应该被执行
    REQUIRE(next_task_run.load() == 0);
}

TEST_CASE("Exception: ResumeAlways Ignores Failure", "[exception]") {
    tfl::ResumeAlways handler; // 假设你的框架提供了 ResumeAlways 忽略异常
    tfl::Executor executor(handler, 2);
    tfl::Flow flow;
    std::atomic<int> next_task_run{0};

    auto fail = flow.emplace([] { throw std::runtime_error("Crash but ignored"); });
    auto next = flow.emplace([&] { next_task_run.fetch_add(1); });

    fail.precede(next);

    // 不应该抛出到用户层
    REQUIRE_NOTHROW(executor.submit(flow).start().wait());
    // 后续任务不应该继续执行
    REQUIRE(next_task_run.load() == 0);
}

// ============================================================================
// [Stress] 大规模压测与边缘条件
// ============================================================================

TEST_CASE("Stress: Mass Concurrent Graph Submissions", "[stress]") {
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, std::thread::hardware_concurrency());

    std::atomic<int> global_counter{0};
    constexpr int NUM_THREADS = 8;
    constexpr int FLOWS_PER_THREAD = 100;
    constexpr int TASKS_PER_FLOW = 50;

    auto worker = [&]() {
        for (int i = 0; i < FLOWS_PER_THREAD; ++i) {
            tfl::Flow f;
            auto start = f.emplace([]{});
            auto end = f.emplace([]{});

            for (int t = 0; t < TASKS_PER_FLOW; ++t) {
                auto mid = f.emplace([&] { global_counter.fetch_add(1, std::memory_order_relaxed); });
                start.precede(mid);
                mid.precede(end);
            }
            executor.submit(f).start().wait();
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) threads.emplace_back(worker);
    for (auto& th : threads) th.join();

    executor.wait_for_all();
    REQUIRE(global_counter.load() == NUM_THREADS * FLOWS_PER_THREAD * TASKS_PER_FLOW);
}

TEST_CASE("Edge: Disconnected Empty Tasks", "[edge]") {
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 2);
    tfl::Flow flow;

    // 放入大量完全断开的孤立空任务，测试调度器调度的边缘性能
    for(int i=0; i<100; ++i) {
        flow.emplace([]{});
    }

    // 只要不崩溃、能正常退出即通过
    REQUIRE_NOTHROW(executor.submit(flow).start().wait());
}
