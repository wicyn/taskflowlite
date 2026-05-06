/// @file test_observer.cpp
/// @brief TaskObserver 测试 —— 任务执行前后的钩子注入。
///
/// 覆盖接口：
///   - TaskObserver::on_before(WorkerView)        任务执行前回调
///   - TaskObserver::on_after(WorkerView)         任务执行后回调
///   - Task::register_observer<T>(args...)        注册（返回 shared_ptr）
///   - Task::unregister_observer(ptr)             注销
///   - DeferredAsyncTask 同样可注册
///
/// 关键不变量：
///   1. on_before 在每次任务 invoke 之前同步调用；
///   2. on_after 在每次任务 invoke 之后同步调用；
///   3. 多个 worker 可能同时回调同一 observer 实例 —— 实现必须线程安全；
///   4. 回调跑在 worker 线程，不要在里面做重活。

#include "test_common.hpp"

using tfl_test::TestEnv;

namespace {

/// @brief 简单计数 observer：on_before / on_after 各自原子自增。
struct CountingObserver : tfl::TaskObserver {
    std::atomic<int> before{0};
    std::atomic<int> after{0};

    void on_before(tfl::WorkerView) override { before.fetch_add(1); }
    void on_after(tfl::WorkerView)  override { after.fetch_add(1); }
};

}  // namespace

// ============================================================================
// SECTION 1: 基础回调 —— 一次任务一对 before/after
// ============================================================================

/// @test [observer][basic] 任务跑一次，before 和 after 各被调一次。
TEST_CASE("Observer: 单次执行 before/after 各 1 次", "[observer][basic]") {
    TestEnv env;
    tfl::Flow flow;

    auto t = flow.emplace([] {});
    auto obs = t.register_observer<CountingObserver>();

    env.executor.deferred_async(flow).start().wait();

    REQUIRE(obs->before.load() == 1);
    REQUIRE(obs->after.load() == 1);
}

/// @test [observer][repeat] deferred_async(flow, N) 跑 N 次，回调各调 N 次。
TEST_CASE("Observer: N 次执行 before/after 各 N 次", "[observer][repeat]") {
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
// SECTION 2: 多个 observer 同时挂在同一任务
// ============================================================================

/// @test [observer][multi] 同一任务挂两个 observer，都被调用。
TEST_CASE("Observer: 多个 observer 共存", "[observer][multi]") {
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
// SECTION 3: unregister
// ============================================================================

/// @test [observer][unregister] 注销后不再被回调。
TEST_CASE("Observer: unregister 后不再回调", "[observer][unregister]") {
    TestEnv env;

    SECTION("注销前后再次跑：obs1 不计数，obs2 继续") {
        tfl::Flow flow;
        auto t = flow.emplace([] {});
        auto obs1 = t.register_observer<CountingObserver>();
        auto obs2 = t.register_observer<CountingObserver>();

        // 先跑一次
        env.executor.deferred_async(flow).start().wait();
        REQUIRE(obs1->before.load() == 1);
        REQUIRE(obs2->before.load() == 1);

        // 注销 obs1
        t.unregister_observer(obs1);

        // 再跑一次 —— obs1 不应再被调
        env.executor.deferred_async(flow).start().wait();
        env.executor.wait_for_all();
        REQUIRE(obs1->before.load() == 1);  // 不变
        REQUIRE(obs2->before.load() == 2);  // +1
    }
}

// ============================================================================
// SECTION 4: DeferredAsyncTask 注册 observer
// ============================================================================

/// @test [observer][async] DeferredAsyncTask 可在 start 前注册 observer。
TEST_CASE("Observer: DeferredAsyncTask 注册", "[observer][async]") {
    TestEnv env;

    auto t = env.executor.deferred_async([] {});
    auto obs = t.register_observer<CountingObserver>();
    t.start().wait();

    REQUIRE(obs->before.load() == 1);
    REQUIRE(obs->after.load() == 1);
}
