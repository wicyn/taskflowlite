/// @file test_executor.cpp
/// @brief Executor 模块测试 —— DAG 调度正确性 / async 各种形式 / async(single) / detach。
///
/// 覆盖的接口：
///   - Executor(handler, num_workers)             构造
///   - Executor::async(Flow)                      单次提交
///   - Executor::async(Flow, callback)            带回调
///   - Executor::async(Flow, num)                 固定次数循环
///   - Executor::async(Flow, num, callback)       固定次数 + 回调
///   - Executor::async(Flow, predicate)           谓词驱动循环
///   - tfl::NonrepeatAsyncTask(callable, args...)        独立异步任务
///   - Executor::submit / AsyncTask::wait         独立任务提交与等待
///   - Executor::detach                           即发即忘
///   - Executor::async(single)                    返回 Future
///   - Executor::wait_for_all                     全局等待

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: DAG 调度正确性
// ============================================================================

/// @test [executor][dag] 串行链 A→B→C→D 按严格顺序执行。
/// @details 验证依赖约束：每个节点启动时，step 必须已被其前驱节点写入正确的值。
///          REQUIRE 不能在 worker 内部调用；使用原子标志收集结果并在主线程中断言。
TEST_CASE("Executor: serial chain executes in topological order", "[executor][dag][serial]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> step{0};
    std::atomic<bool> ok_a{false}, ok_b{false}, ok_c{false}, ok_d{false};

    auto a = flow.emplace([&] { ok_a.store(step.exchange(1) == 0); });
    auto b = flow.emplace([&] { ok_b.store(step.exchange(2) == 1); });
    auto c = flow.emplace([&] { ok_c.store(step.exchange(3) == 2); });
    auto d = flow.emplace([&] { ok_d.store(step.exchange(4) == 3); });

    a.precede(b);
    b.precede(c);
    c.precede(d);

    env.executor.async(flow).wait();

    REQUIRE(step.load() == 4);
    REQUIRE(ok_a.load());
    REQUIRE(ok_b.load());
    REQUIRE(ok_c.load());
    REQUIRE(ok_d.load());
}

/// @test [executor][dag] 菱形 A→{B,C}→D，B 和 C 必须在 D 之前全部完成。
TEST_CASE("Executor: diamond DAG synchronization semantics", "[executor][dag][diamond]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<bool> a_done{false}, b_done{false}, c_done{false};
    std::atomic<bool> d_saw_b{false}, d_saw_c{false};

    auto a = flow.emplace([&] { a_done.store(true); });
    auto b = flow.emplace([&] { b_done.store(true); });
    auto c = flow.emplace([&] { c_done.store(true); });
    auto d = flow.emplace([&] {
        d_saw_b.store(b_done.load());
        d_saw_c.store(c_done.load());
    });

    a.precede(b, c);
    d.succeed(b, c);

    env.executor.async(flow).wait();

    REQUIRE(a_done.load());
    REQUIRE(b_done.load());
    REQUIRE(c_done.load());
    REQUIRE(d_saw_b.load());
    REQUIRE(d_saw_c.load());
}

/// @test [executor][dag] N 个完全独立的任务并行调度。
/// @details 不验证执行顺序，仅验证计数 —— 所有任务必须已被调度。
TEST_CASE("Executor: N independent tasks run in parallel", "[executor][dag][parallel]") {
    TestEnv env(4);
    tfl::Flow flow;
    constexpr int N = 64;
    std::atomic<int> hits{0};

    for (int i = 0; i < N; ++i) {
        flow.emplace([&] { hits.fetch_add(1, std::memory_order_relaxed); });
    }

    env.executor.async(flow).wait();
    REQUIRE(hits.load() == N);
}

// ============================================================================
// SECTION 2: async(Flow, ...) 的所有变体
// ============================================================================

/// @test [executor][async] async(Flow, N) 执行 N 次。
TEST_CASE("Executor: async(flow, N) fixed count", "[executor][async][repeat]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> count{0};

    flow.emplace([&] { count.fetch_add(1); });

    env.executor.async(flow, 5ULL).wait();
    env.executor.wait_for_all();

    REQUIRE(count.load() == 5);
}

/// @test [executor][async] async(Flow, callback) 在完成时调用回调。
TEST_CASE("Executor: async(flow, callback)", "[executor][async][callback]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<bool> task_run{false};
    std::atomic<bool> cb_run{false};

    flow.emplace([&] { task_run.store(true); });

    env.executor.async(flow, [&]() noexcept { cb_run.store(true); })
                .wait();

    REQUIRE(task_run.load());
    REQUIRE(cb_run.load());
}

/// @test [executor][async] async(Flow, N, callback) 运行 N 次后调用一次回调。
TEST_CASE("Executor: async(flow, N, callback)", "[executor][async][repeat][callback]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> count{0};
    std::atomic<int> cb_count{0};

    flow.emplace([&] { count.fetch_add(1); });

    env.executor.async(flow, 3ULL, [&]() noexcept { cb_count.fetch_add(1); })
                .wait();

    REQUIRE(count.load() == 3);
    REQUIRE(cb_count.load() == 1);  // 回调仅被调用一次
}

/// @test [executor][async] async(Flow, predicate) 谓词驱动循环。
TEST_CASE("Executor: async(flow, predicate) predicate loop", "[executor][async][predicate]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> count{0};

    flow.emplace([&] { count.fetch_add(1); });

    int runs = 0;
    env.executor.async(flow, [&runs]() mutable noexcept {
        return runs++ >= 4;  // 执行 4 轮后停止
    }).wait();

    REQUIRE(count.load() == 4);
}

// ============================================================================
// SECTION 3: AsyncTask<>(callable, args...) + submit —— 独立任务
// ============================================================================

/// @test [executor][async] AsyncTask<>(基本任务) + submit 提交执行。
TEST_CASE("Executor: AsyncTask single basic task", "[executor][async][standalone]") {
    TestEnv env;
    std::atomic<int> v{0};

    auto t = tfl::NonrepeatAsyncTask([&] { v.store(42); });
    env.executor.submit(t); t.wait();

    REQUIRE(v.load() == 42);
}

/// @test [executor][async] AsyncTask<>(callable, args...) 参数转发 + submit。
TEST_CASE("Executor: AsyncTask single task with arguments", "[executor][async][standalone][args]") {
    TestEnv env;
    int v = 0;
    auto t = tfl::NonrepeatAsyncTask([](int& r) { r = 99; }, std::ref(v));
    env.executor.submit(t); t.wait();
    REQUIRE(v == 99);
}

/// @test [executor][async] AsyncTask<>(Runtime 可调用对象) + submit。
TEST_CASE("Executor: AsyncTask single Runtime task", "[executor][async][runtime]") {
    TestEnv env;
    std::atomic<int> v{0};

    auto t = tfl::NonrepeatAsyncTask([&](tfl::Runtime&) { v.store(7); });
    env.executor.submit(t); t.wait();

    REQUIRE(v.load() == 7);
}

// ============================================================================
// SECTION 4: detach / async
// ============================================================================

/// @test [executor][detach] detach 即发即忘。
TEST_CASE("Executor: detach returns no handle", "[executor][detach]") {
    TestEnv env;
    std::atomic<int> n{0};

    for (int i = 0; i < 16; ++i) {
        env.executor.detach([&] { n.fetch_add(1); });
    }
    env.executor.wait_for_all();

    REQUIRE(n.load() == 16);
}

/// @test [executor][async] async 返回 future，可获取结果。
TEST_CASE("Executor: async returns future<int>", "[executor][async]") {
    TestEnv env;

    auto fut = env.executor.async([] { return 21 * 2; });
    REQUIRE(fut.get() == 42);
}

/// @test [executor][async] async 带参数转发。
TEST_CASE("Executor: async with arguments", "[executor][async][args]") {
    TestEnv env;

    auto fut = env.executor.async([](int a, int b) { return a + b; }, 10, 32);
    REQUIRE(fut.get() == 42);
}

/// @test [executor][async] async(Runtime) 同样有效。
TEST_CASE("Executor: async Runtime task", "[executor][async][runtime]") {
    TestEnv env;
    auto fut = env.executor.async([](tfl::Runtime&) -> int { return 100; });
    REQUIRE(fut.get() == 100);
}

// ============================================================================
// SECTION 5: 压力测试 —— 大规模并发
// ============================================================================

/// @test [executor][stress] 10K detach 任务全部完成。
TEST_CASE("Executor: large-scale detach stress", "[executor][stress]") {
    TestEnv env(8);
    std::atomic<int> n{0};
    constexpr int N = 10'000;

    for (int i = 0; i < N; ++i) {
        env.executor.detach([&] { n.fetch_add(1, std::memory_order_relaxed); });
    }
    env.executor.wait_for_all();

    REQUIRE(n.load() == N);
}
