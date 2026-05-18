/// @file test_observer.cpp
/// @brief TaskObserver 测试 — 任务执行前后的钩子注入。
///
/// 覆盖的接口：
///   - TaskObserver::on_before(WorkerView)        任务执行前回调
///   - TaskObserver::on_after(WorkerView)         任务执行后回调
///   - Task::register_observer<T>(args...)        注册（返回 shared_ptr）
///   - Task::unregister_observer(ptr)             取消注册
///   - DeferredAsyncTask 同样支持注册
///
/// 关键不变量：
///   1. on_before 在每次任务调用前同步执行；
///   2. on_after 在每次任务调用后同步执行；
///   3. 多个 worker 可能并发调用同一 observer 实例 — 实现必须线程安全；
///   4. 回调在 worker 线程上运行；不要在其中执行重负载工作。

#include "test_common.hpp"

using tfl_test::TestEnv;

namespace {

/// @brief 简单计数 observer：on_before / on_after 各原子递增。
struct CountingObserver : tfl::TaskObserver {
    std::atomic<int> before{0};
    std::atomic<int> after{0};

    void on_before(tfl::WorkerView) override { before.fetch_add(1); }
    void on_after(tfl::WorkerView)  override { after.fetch_add(1); }
};

}  // namespace

// ============================================================================
// SECTION 1: 基础回调 — 每次任务执行对应一对 before/after
// ============================================================================

/// @section single-execution-before-after-once
/// @test [observer][basic] 任务运行一次，before 和 after 各调用一次。
TEST_CASE("Observer: single execution before/after each called once", "[observer][basic]") {
    TestEnv env;
    tfl::Flow flow;

    auto t = flow.emplace([] {});
    auto obs = t.register_observer<CountingObserver>();

    env.executor.deferred_async(flow).start().wait();

    REQUIRE(obs->before.load() == 1);
    REQUIRE(obs->after.load() == 1);
}

/// @section n-executions-before-after-n-times
/// @test [observer][repeat] deferred_async(flow, N) 运行 N 次，回调各调用 N 次。
TEST_CASE("Observer: N executions before/after each called N times", "[observer][repeat]") {
    TestEnv env;
    tfl::Flow flow;
    constexpr int N = 5;

    auto t = flow.emplace([] {});
    auto obs = t.register_observer<CountingObserver>();

    env.executor.deferred_async(flow, static_cast<std::uint64_t>(N)).start().wait();
    env.executor.wait_for_all();

    REQUIRE(obs->before.load() == N);
    REQUIRE(obs->after.load() == N);
}

// ============================================================================
// SECTION 2: 同一任务上注册多个 observer
// ============================================================================

/// @section multiple-observers-coexist
/// @test [observer][multi] 同一任务上的两个 observer 均被调用。
TEST_CASE("Observer: multiple observers coexist", "[observer][multi]") {
    TestEnv env;
    tfl::Flow flow;

    auto t = flow.emplace([] {});
    auto obs_a = t.register_observer<CountingObserver>();
    auto obs_b = t.register_observer<CountingObserver>();

    env.executor.deferred_async(flow).start().wait();

    REQUIRE(obs_a->before.load() == 1);
    REQUIRE(obs_a->after.load() == 1);
    REQUIRE(obs_b->before.load() == 1);
    REQUIRE(obs_b->after.load() == 1);
}

// ============================================================================
// SECTION 3: 取消注册
// ============================================================================

/// @section unregister-stops-callbacks
/// @test [observer][unregister] 取消注册后不再被调用。
TEST_CASE("Observer: no longer called after unregister", "[observer][unregister]") {
    TestEnv env;

    /// @section rerun-after-unregister
    SECTION("Run again after unregister: obs1 not counted, obs2 continues") {
        tfl::Flow flow;
        auto t = flow.emplace([] {});
        auto obs1 = t.register_observer<CountingObserver>();
        auto obs2 = t.register_observer<CountingObserver>();

        // 运行一次
        env.executor.deferred_async(flow).start().wait();
        REQUIRE(obs1->before.load() == 1);
        REQUIRE(obs2->before.load() == 1);

        // 取消注册 obs1
        t.unregister_observer(obs1);

        // 再次运行 — obs1 不应再被调用
        env.executor.deferred_async(flow).start().wait();
        env.executor.wait_for_all();
        REQUIRE(obs1->before.load() == 1);  // 未变
        REQUIRE(obs2->before.load() == 2);  // +1
    }
}

// ============================================================================
// SECTION 4: 在 DeferredAsyncTask 上注册 observer
// ============================================================================

/// @section deferred-async-task-registration
/// @test [observer][async] DeferredAsyncTask 可在 start 前注册 observer。
TEST_CASE("Observer: DeferredAsyncTask registration", "[observer][async]") {
    TestEnv env;

    auto t = env.executor.deferred_async([] {});
    auto obs = t.register_observer<CountingObserver>();
    t.start().wait();

    REQUIRE(obs->before.load() == 1);
    REQUIRE(obs->after.load() == 1);
}
