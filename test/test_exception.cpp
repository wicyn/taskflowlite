/// @file test_exception.cpp
/// @brief 异常处理测试 — ResumeNever 策略 / 异常归档 / get 与 wait 对比。
///
/// TaskflowLite 的异常模型：
///   - 任务体内抛出的异常被 `Topology` 捕获为首个异常；
///   - `wait()` 静默吞掉异常；
///   - `get()` = wait + 重新抛出；
///   - WorkerHandler 决定异常后工作线程是否继续（ResumeNever 立即停止）。
///
/// 覆盖点：
///   - 任务抛出后，wait() 不抛、get() 抛
///   - 框架在异常后仍能正确清理拓扑
///   - has_exception() 查询

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: 异常归档 — wait 与 get 对比
// ============================================================================

/// @test [exception][wait-vs-get] wait 不重新抛出，get 重新抛出。
TEST_CASE("Exception: wait does not rethrow, get does", "[exception][wait-vs-get]") {
    TestEnv env;

    /// @section wait-is-silent
    SECTION("wait is silent") {
        auto t = env.executor.deferred_async([] {
            throw std::runtime_error("boom");
        });
        t.start();
        REQUIRE_NOTHROW(t.wait());
    }

    /// @section get-rethrows
    SECTION("get rethrows runtime_error") {
        auto t = env.executor.deferred_async([] {
            throw std::runtime_error("boom");
        });
        t.start();
        REQUIRE_THROWS_AS(t.get(), std::runtime_error);
    }
}

/// @test [exception][has_exception] has_exception 查询异常归档状态。
TEST_CASE("Exception: has_exception query", "[exception][query]") {
    TestEnv env;

    auto bad = env.executor.deferred_async([] {
        throw std::runtime_error("oops");
    });
    bad.start();
    bad.wait();
    REQUIRE(bad.has_exception());

    auto good = env.executor.deferred_async([] {});
    good.start().wait();
    REQUIRE_FALSE(good.has_exception());
}

// ============================================================================
// SECTION 2: 异常不会泄漏拓扑
// ============================================================================

/// @test [exception][cleanup] 异常后提交新任务仍正常工作。
TEST_CASE("Exception: Executor still usable after exception", "[exception][cleanup]") {
    TestEnv env;

    {
        auto bad = env.executor.deferred_async([] { throw std::runtime_error("err"); });
        bad.start().wait();
    }

    // 发生异常后，执行器仍能接受新任务
    std::atomic<int> n{0};
    auto good = env.executor.deferred_async([&] { n.store(7); });
    good.start().wait();
    REQUIRE(n.load() == 7);
}

// ============================================================================
// SECTION 3: Flow 中的异常 — ResumeNever 阻止后续节点
// ============================================================================

/// @test [exception][resume-never] 在 ResumeNever 策略下，前驱抛出后依赖节点不会运行。
TEST_CASE("Exception: ResumeNever prevents subsequent nodes", "[exception][resume-never]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<bool> next_run{false};

    auto bad = flow.emplace([] { throw std::runtime_error("kaboom"); });
    auto next = flow.emplace([&] { next_run.store(true); });
    bad.precede(next);

    env.executor.deferred_async(flow).start().wait();
    REQUIRE_FALSE(next_run.load());
}
