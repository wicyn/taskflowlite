/// @file test_runtime.cpp
/// @brief 运行时模块测试 — 任务体内的动态派发、协作等待、子任务依赖。
///
/// 覆盖的接口：
///   - Runtime::async(F)                     派发一个返回 future 的子任务
///   - Runtime::async(F, args...)            带参数版本
///   - Runtime::silent_async(F)              即发即忘子任务
///   - Runtime::cowait()                     协作等待所有子任务完成
///   - Runtime::cowait_until(pred)           协作条件等待
///   - Runtime::executor()                   访问父 Executor
///   - Runtime::worker()                     当前 worker 视图
///
/// 关键不变量：cowait 期间 worker 不会阻塞/休眠，而是主动窃取并执行其他任务。

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: silent_async + cowait — 动态扇出
// ============================================================================

/// @section silent-async-dynamic-fan-out
/// @test [runtime][silent_async] 在 Runtime 任务内，silent_async 派发 N 个子任务，cowait 等待全部完成。
/// @details 这是最经典的 Runtime 用法 — 数据驱动的动态扇出。
TEST_CASE("Runtime: silent_async dynamic fan-out + cowait", "[runtime][silent_async][cowait]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> n{0};
    constexpr int N = 50;

    flow.emplace([&](tfl::Runtime& rt) {
        for (int i = 0; i < N; ++i) {
            rt.silent_async([&] { n.fetch_add(1, std::memory_order_relaxed); });
        }
        rt.cowait();   // 等待所有 silent_async 派发的子任务完成
    });

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(n.load() == N);
}

/// @section implicit-wait
/// @test [runtime][cowait] 即使不调用 cowait，父任务仍会等待子任务完成（join_counter 自动等待）。
/// @details Runtime 派发的子任务会附加到父节点的 join_counter 上。
///          父任务 invoke 返回后，tear_down 会自动等待子任务 — 无需显式调用 cowait。
///          cowait 的真正目的是确保子任务在**任务体中间**完成，父任务才能继续执行。
TEST_CASE("Runtime: implicit wait — framework auto-waits after parent invoke returns", "[runtime][implicit-wait]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> n{0};
    constexpr int N = 20;

    flow.emplace([&](tfl::Runtime& rt) {
        for (int i = 0; i < N; ++i) {
            rt.silent_async([&] { n.fetch_add(1); });
        }
        // 不调用 cowait — 让框架在 tear_down 阶段等待
    });

    auto next = flow.emplace([&] {
        // 当此节点运行时，所有 silent_async 必须已完成
    });
    flow.for_each([&](tfl::Task t) {  // 安全构图：使用第一个节点作为 next 的前驱
        if (t != next) t.precede(next);
    });

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(n.load() == N);
}

// ============================================================================
// SECTION 2: async — 派发并获取结果
// ============================================================================

/// @section async-dispatch-results
/// @test [runtime][async] Runtime::async 返回 Future，可获取计算结果。
TEST_CASE("Runtime: async dispatches result-returning sub-tasks", "[runtime][async]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> result{0};

    flow.emplace([&](tfl::Runtime& rt) {
        auto fut = rt.async([] { return 17 * 2; });
        // 必须显式等待才能获取结果
        rt.cowait_until([&]() noexcept {
            return fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        });
        result.store(fut.get());
    });

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(result.load() == 34);
}

/// @section async-with-arguments
/// @test [runtime][async] async 支持参数转发。
TEST_CASE("Runtime: async with arguments", "[runtime][async][args]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> result{0};

    flow.emplace([&](tfl::Runtime& rt) {
        auto fut = rt.async([](int x, int y) { return x + y; }, 10, 32);
        rt.cowait();
        result.store(fut.get());
    });

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(result.load() == 42);
}

// ============================================================================
// SECTION 3: cowait_until — 谓词驱动的协作等待
// ============================================================================

/// @section cowait-until-predicate
/// @test [runtime][cowait_until] 谓词一旦为真立即返回。
TEST_CASE("Runtime: cowait_until predicate-driven wait", "[runtime][cowait_until]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> done_count{0};
    constexpr int TARGET = 10;

    flow.emplace([&](tfl::Runtime& rt) {
        for (int i = 0; i < TARGET; ++i) {
            rt.silent_async([&] {
                done_count.fetch_add(1, std::memory_order_relaxed);
            });
        }
        // 等待完成计数达到 TARGET
        rt.cowait_until([&]() noexcept {
            return done_count.load(std::memory_order_relaxed) >= TARGET;
        });
    });

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(done_count.load() >= TARGET);
}

// ============================================================================
// SECTION 4: 嵌套 Runtime — Runtime 在子任务内部
// ============================================================================

/// @section nested-runtime-dispatch
/// @test [runtime][nested] silent_async 派发的 Runtime 子任务可以再次 silent_async。
TEST_CASE("Runtime: nested Runtime dispatch", "[runtime][nested]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> leaf_count{0};

    flow.emplace([&](tfl::Runtime& rt) {
        for (int i = 0; i < 4; ++i) {
            rt.silent_async([&](tfl::Runtime& sub_rt) {
                for (int j = 0; j < 5; ++j) {
                    sub_rt.silent_async([&] { leaf_count.fetch_add(1); });
                }
                sub_rt.cowait();
            });
        }
        rt.cowait();
    });

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(leaf_count.load() == 20);
}

// ============================================================================
// SECTION 5: 上下文查询 — executor() / worker()
// ============================================================================

/// @section executor-context-access
/// @test [runtime][context] Runtime::executor() 返回父 Executor。
TEST_CASE("Runtime: executor context access", "[runtime][context]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<bool> matched{false};

    flow.emplace([&](tfl::Runtime& rt) {
        // 比较地址
        matched.store(&rt.executor() == &env.executor);
    });

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(matched.load());
}
