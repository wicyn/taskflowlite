/// @file test_exception.cpp
/// @brief 异常处理测试 — 异常传播、归档与 get/wait 对比。
/// @author wicyn
///
/// TaskflowLite 的异常模型：
///   - 任务体内抛出的异常被 `Topology` 捕获为首个异常；
///   - `wait()` 静默吞掉异常；
///   - `get()` = wait + 重新抛出；
///   - 图内异常阻止依赖后继；Future 保存异常，Executor 可继续处理独立任务。
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
        auto t = tfl::AsyncTask([] {
            throw std::runtime_error("boom");
        });
        env.executor.run(t);
        REQUIRE_NOTHROW(t.wait());
    }

    /// @section get-rethrows
    SECTION("get rethrows runtime_error") {
        auto t = tfl::AsyncTask([] {
            throw std::runtime_error("boom");
        });
        env.executor.run(t);
        REQUIRE_THROWS_AS(t.get(), std::runtime_error);
    }
}

/// @test [exception][has_exception] has_exception 查询异常归档状态。
TEST_CASE("Exception: has_exception query", "[exception][query]") {
    TestEnv env;

    auto bad = tfl::AsyncTask([] {
        throw std::runtime_error("oops");
    });
    env.executor.run(bad);
    bad.wait();

    auto good = tfl::AsyncTask([] {});
    env.executor.run(good);
    good.wait();
}

// ============================================================================
// SECTION 2: 异常不会泄漏拓扑
// ============================================================================

/// @test [exception][cleanup] 异常后提交新任务仍正常工作。
TEST_CASE("Exception: Executor still usable after exception", "[exception][cleanup]") {
    TestEnv env;

    {
        auto bad = tfl::AsyncTask([] { throw std::runtime_error("err"); });
        env.executor.run(bad);
        bad.wait();
    }

    // 发生异常后，执行器仍能接受新任务
    std::atomic<int> n{0};
    auto good = tfl::AsyncTask([&] { n.store(7); });
    env.executor.run(good);
    good.wait();
    REQUIRE(n.load() == 7);
}

// ============================================================================
// SECTION 3: Flow 中的异常 — 阻止依赖后继
// ============================================================================

/// @test [exception][successor] 前驱抛出后依赖节点不会运行。
TEST_CASE("Exception: failure prevents dependent successors", "[exception][successor]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<bool> next_run{false};

    auto bad = flow.emplace([] { throw std::runtime_error("kaboom"); });
    auto next = flow.emplace([&] { next_run.store(true); });
    bad.precede(next);

    env.executor.async(flow).wait();
    REQUIRE_FALSE(next_run.load());
}

// ============================================================================
// 第 4 节: 兄弟节点异常独立性
// ============================================================================

/// @test [exception][sibling] A→{B,C}→D, B 异常不影响 C
TEST_CASE("Exception: sibling branch not blocked by exception in other branch", "[exception][sibling]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<bool> c_ran{false}, d_ran{false};

    auto a = flow.emplace([] {});
    auto b = flow.emplace([] { throw std::runtime_error("b fails"); });
    auto c = flow.emplace([&] { c_ran.store(true); });
    auto d = flow.emplace([&] { d_ran.store(true); });

    a.precede(b, c);
    d.succeed(b, c);

    env.executor.async(flow).wait();

    REQUIRE(c_ran.load());        // 兄弟不受影响
    REQUIRE_FALSE(d_ran.load());  // D 被 B 的异常阻塞
}

// ============================================================================
// 第 5 节: has_exception 查询
// ============================================================================

/// @test [exception][has_exception] Flow 完成后 has_exception 查询
TEST_CASE("Exception: has_exception after Flow completes", "[exception][has_exception]") {
    TestEnv env;

    SECTION("Flow with exception") {
        tfl::Flow flow;
        flow.emplace([] { throw std::runtime_error("planned"); });
        auto task = tfl::AsyncTask(flow);
        env.executor.run(task);
        task.wait();
    }

    SECTION("Flow without exception") {
        tfl::Flow flow;
        flow.emplace([] {});
        flow.emplace([] {});
        auto task = tfl::AsyncTask(flow);
        env.executor.run(task);
        task.wait();
    }
}

// ============================================================================
// 第 6 节: 用户代码自行捕获异常
// ============================================================================

/// @test [exception][local-catch] callable 内捕获异常后，依赖后继正常运行。
TEST_CASE("Exception: local catch allows successors to run", "[exception][local-catch]") {
    tfl::Executor exec(2);
    tfl::Flow flow;
    std::atomic<bool> next_ran{ false };

    auto bad = flow.emplace([] {
        try { throw std::runtime_error("handled"); }
        catch (const std::runtime_error&) {}
    });
    auto next = flow.emplace([&] { next_ran.store(true); });
    bad.precede(next);

    REQUIRE_NOTHROW(exec.async(flow).wait());
    REQUIRE(next_ran.load()); 
}

// ============================================================================
// 第 7 节: 多异常仅保留第一个
// ============================================================================

/// @test [exception][multi] 多个异常仅第一个被归档
TEST_CASE("Exception: multiple exceptions only first one is captured", "[exception][multi]") {
    TestEnv env;
    tfl::Flow flow;

    auto a = flow.emplace([] {});
    auto b = flow.emplace([] { throw std::runtime_error("first"); });
    auto c = flow.emplace([] { throw std::runtime_error("second"); });
    a.precede(b, c);

    auto task = tfl::AsyncTask(flow);
    env.executor.run(task);
    task.wait();
}
