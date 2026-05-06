/// @file test_executor.cpp
/// @brief Executor 模块测试 —— DAG 调度正确性 / deferred_async 各种形式 / async / silent_async。
///
/// 覆盖接口：
///   - Executor(handler, num_workers)        构造
///   - Executor::deferred_async(Flow)                 单次提交
///   - Executor::deferred_async(Flow, callback)       带回调
///   - Executor::deferred_async(Flow, num)            固定次数循环
///   - Executor::deferred_async(Flow, num, callback)  固定次数 + 回调
///   - Executor::deferred_async(Flow, predicate)      谓词驱动循环
///   - Executor::deferred_async(callable, args...)    单基础任务（返回 AsyncTask）
///   - Executor::deferred_async(runtime_callable)     单 Runtime 任务
///   - Executor::silent_async                 fire-and-forget
///   - Executor::async                        返回 future
///   - Executor::wait_for_all                 全局等待
///
/// 关键约束：所有 deferred_async(...) 返回的是 DeferredAsyncTask，必须 .start().wait() 才会跑。

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: DAG 调度正确性
// ============================================================================

/// @test [executor][dag] 串行链 A→B→C→D 严格按顺序执行。
/// @details 验证依赖约束：每个节点开始时 step 必须已经被前驱写到对应值。
///          worker 内不能调用 REQUIRE，先用 atomic flag 收集结果，主线程统一断言。
TEST_CASE("Executor: 串行链按拓扑顺序执行", "[executor][dag][serial]") {
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

    env.executor.deferred_async(flow).start().wait();

    REQUIRE(step.load() == 4);
    REQUIRE(ok_a.load());
    REQUIRE(ok_b.load());
    REQUIRE(ok_c.load());
    REQUIRE(ok_d.load());
}

/// @test [executor][dag] 菱形 A→{B,C}→D，B 和 C 必须都在 D 之前完成。
TEST_CASE("Executor: 菱形 DAG 同步语义", "[executor][dag][diamond]") {
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

    env.executor.deferred_async(flow).start().wait();

    REQUIRE(a_done.load());
    REQUIRE(b_done.load());
    REQUIRE(c_done.load());
    REQUIRE(d_saw_b.load());
    REQUIRE(d_saw_c.load());
}

/// @test [executor][dag] 完全独立的 N 个任务并行调度。
/// @details 不验证执行顺序，只验证数量 —— 全部都被调度过。
TEST_CASE("Executor: N 独立任务并行执行", "[executor][dag][parallel]") {
    TestEnv env(4);
    tfl::Flow flow;
    constexpr int N = 64;
    std::atomic<int> hits{0};

    for (int i = 0; i < N; ++i) {
        flow.emplace([&] { hits.fetch_add(1, std::memory_order_relaxed); });
    }

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(hits.load() == N);
}

// ============================================================================
// SECTION 2: deferred_async(Flow, ...) 的所有变体
// ============================================================================

/// @test [executor][deferred_async] deferred_async(Flow, N) 执行 N 次。
TEST_CASE("Executor: deferred_async(flow, N) 固定次数", "[executor][deferred_async][repeat]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> count{0};

    flow.emplace([&] { count.fetch_add(1); });

    env.executor.deferred_async(flow, 5ULL).start().wait();
    env.executor.wait_for_all();

    REQUIRE(count.load() == 5);
}

/// @test [executor][deferred_async] deferred_async(Flow, callback) 完成时调用回调。
TEST_CASE("Executor: deferred_async(flow, callback)", "[executor][deferred_async][callback]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<bool> task_run{false};
    std::atomic<bool> cb_run{false};

    flow.emplace([&] { task_run.store(true); });

    env.executor.deferred_async(flow, [&]() noexcept { cb_run.store(true); })
                .start().wait();

    REQUIRE(task_run.load());
    REQUIRE(cb_run.load());
}

/// @test [executor][deferred_async] deferred_async(Flow, N, callback) 跑 N 次后调一次回调。
TEST_CASE("Executor: deferred_async(flow, N, callback)", "[executor][deferred_async][repeat][callback]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> count{0};
    std::atomic<int> cb_count{0};

    flow.emplace([&] { count.fetch_add(1); });

    env.executor.deferred_async(flow, 3ULL, [&]() noexcept { cb_count.fetch_add(1); })
                .start().wait();

    REQUIRE(count.load() == 3);
    REQUIRE(cb_count.load() == 1);  // callback 只调一次
}

/// @test [executor][deferred_async] deferred_async(Flow, predicate) 由谓词驱动循环。
TEST_CASE("Executor: deferred_async(flow, predicate) 谓词循环", "[executor][deferred_async][predicate]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> count{0};

    flow.emplace([&] { count.fetch_add(1); });

    int runs = 0;
    env.executor.deferred_async(flow, [&runs]() mutable noexcept {
        return runs++ >= 4;  // 4 轮后停止
    }).start().wait();

    REQUIRE(count.load() == 4);
}

// ============================================================================
// SECTION 3: deferred_async(callable, args...) —— 独立任务
// ============================================================================

/// @test [executor][deferred_async] deferred_async(基础任务) 返回 DeferredAsyncTask。
TEST_CASE("Executor: deferred_async 单基础任务", "[executor][deferred_async][standalone]") {
    TestEnv env;
    std::atomic<int> v{0};

    auto t = env.executor.deferred_async([&] { v.store(42); });
    t.start().wait();

    REQUIRE(v.load() == 42);
}

/// @test [executor][deferred_async] deferred_async(callable, args...) 风格转发。
TEST_CASE("Executor: deferred_async 单任务带参", "[executor][deferred_async][standalone][args]") {
    TestEnv env;
    int v = 0;
    auto t = env.executor.deferred_async([](int& r) { r = 99; }, std::ref(v));
    t.start().wait();
    REQUIRE(v == 99);
}

/// @test [executor][deferred_async] deferred_async(Runtime callable) 单 Runtime 任务。
TEST_CASE("Executor: deferred_async 单 Runtime 任务", "[executor][deferred_async][runtime]") {
    TestEnv env;
    std::atomic<int> v{0};

    auto t = env.executor.deferred_async([&](tfl::Runtime&) { v.store(7); });
    t.start().wait();

    REQUIRE(v.load() == 7);
}

// ============================================================================
// SECTION 4: silent_async / async
// ============================================================================

/// @test [executor][silent_async] silent_async fire-and-forget。
TEST_CASE("Executor: silent_async 不返回句柄", "[executor][silent_async]") {
    TestEnv env;
    std::atomic<int> n{0};

    for (int i = 0; i < 16; ++i) {
        env.executor.silent_async([&] { n.fetch_add(1); });
    }
    env.executor.wait_for_all();

    REQUIRE(n.load() == 16);
}

/// @test [executor][async] async 返回 future，可拿到结果。
TEST_CASE("Executor: async 返回 future<int>", "[executor][async]") {
    TestEnv env;

    auto fut = env.executor.async([] { return 21 * 2; });
    REQUIRE(fut.get() == 42);
}

/// @test [executor][async] async 带参传递。
TEST_CASE("Executor: async 带参", "[executor][async][args]") {
    TestEnv env;

    auto fut = env.executor.async([](int a, int b) { return a + b; }, 10, 32);
    REQUIRE(fut.get() == 42);
}

/// @test [executor][async] async(Runtime) 也合法。
TEST_CASE("Executor: async Runtime 任务", "[executor][async][runtime]") {
    TestEnv env;
    auto fut = env.executor.async([](tfl::Runtime&) -> int { return 100; });
    REQUIRE(fut.get() == 100);
}

// ============================================================================
// SECTION 5: 压力测试 —— 大规模并发
// ============================================================================

/// @test [executor][stress] 10K silent_async 任务全部完成。
TEST_CASE("Executor: 大规模 silent_async 压力", "[executor][stress]") {
    TestEnv env(8);
    std::atomic<int> n{0};
    constexpr int N = 10'000;

    for (int i = 0; i < N; ++i) {
        env.executor.silent_async([&] { n.fetch_add(1, std::memory_order_relaxed); });
    }
    env.executor.wait_for_all();

    REQUIRE(n.load() == N);
}
