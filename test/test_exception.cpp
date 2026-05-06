/// @file test_exception.cpp
/// @brief 异常处理测试 —— ResumeNever 策略 / 异常归档 / get vs wait。
///
/// TaskflowLite 的异常模型：
///   - 任务体抛出的异常被 `Topology` 截留为首个异常；
///   - `wait()` 静默吞掉异常；
///   - `get()` = wait + 重抛；
///   - WorkerHandler 决定 worker 在异常时是否继续工作（ResumeNever 直接停）。
///
/// 覆盖：
///   - 任务抛异常后，wait() 不抛、get() 抛
///   - 异常发生时框架仍能正确清理 topology
///   - has_exception() 查询

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: 异常归档 —— wait vs get
// ============================================================================

/// @test [exception][wait-vs-get] wait 不重抛，get 重抛。
TEST_CASE("Exception: wait 不重抛，get 重抛", "[exception][wait-vs-get]") {
    TestEnv env;

    SECTION("wait 静默") {
        auto t = env.executor.deferred_async([] {
            throw std::runtime_error("boom");
        });
        t.start();
        REQUIRE_NOTHROW(t.wait());
    }

    SECTION("get 重抛 runtime_error") {
        auto t = env.executor.deferred_async([] {
            throw std::runtime_error("boom");
        });
        t.start();
        REQUIRE_THROWS_AS(t.get(), std::runtime_error);
    }
}

/// @test [exception][has_exception] has_exception 查询异常归档状态。
TEST_CASE("Exception: has_exception 查询", "[exception][query]") {
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
// SECTION 2: 异常发生不会泄漏 topology
// ============================================================================

/// @test [exception][cleanup] 异常发生后再提交新任务仍正常工作。
TEST_CASE("Exception: 异常后 Executor 仍可用", "[exception][cleanup]") {
    TestEnv env;

    {
        auto bad = env.executor.deferred_async([] { throw std::runtime_error("err"); });
        bad.start().wait();
    }

    // 异常之后 executor 仍然可以接受新任务
    std::atomic<int> n{0};
    auto good = env.executor.deferred_async([&] { n.store(7); });
    good.start().wait();
    REQUIRE(n.load() == 7);
}

// ============================================================================
// SECTION 3: Flow 内异常 —— ResumeNever 不再调度后续节点
// ============================================================================

/// @test [exception][resume-never] ResumeNever 策略下，前驱抛异常后驱动的节点不会运行。
TEST_CASE("Exception: ResumeNever 阻止后续节点", "[exception][resume-never]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<bool> next_run{false};

    auto bad = flow.emplace([] { throw std::runtime_error("kaboom"); });
    auto next = flow.emplace([&] { next_run.store(true); });
    bad.precede(next);

    env.executor.deferred_async(flow).start().wait();
    REQUIRE_FALSE(next_run.load());
}
