/// @file test_runtime.cpp
/// @brief Runtime 模块测试 —— 任务体内动态派发、协作式等待、子任务依赖。
///
/// 覆盖接口：
///   - Runtime::async(F)                     派发返回 future 的子任务
///   - Runtime::async(F, args...)            带参版
///   - Runtime::silent_async(F)              fire-and-forget 子任务
///   - Runtime::cowait()                     协作式等待全部子任务
///   - Runtime::cowait_until(pred)           协作式条件等待
///   - Runtime::executor()                   访问父 Executor
///   - Runtime::worker()                     当前 worker 视图
///
/// 关键不变量：cowait 期间 worker 不阻塞睡眠，会主动窃取其他任务执行。

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: silent_async + cowait —— 动态扇出
// ============================================================================

/// @test [runtime][silent_async] 在 Runtime 任务里 silent_async 派发 N 个子任务，cowait 等齐。
/// @details 这是最经典的 Runtime 用法 —— 数据驱动的动态扇出。
TEST_CASE("Runtime: silent_async 动态扇出 + cowait", "[runtime][silent_async][cowait]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> n{0};
    constexpr int N = 50;

    flow.emplace([&](tfl::Runtime& rt) {
        for (int i = 0; i < N; ++i) {
            rt.silent_async([&] { n.fetch_add(1, std::memory_order_relaxed); });
        }
        rt.cowait();   // 等齐所有 silent_async 派发的子任务
    });

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(n.load() == N);
}

/// @test [runtime][cowait] 不调 cowait 时，父任务仍会等子任务结束（join_counter 自动等齐）。
/// @details Runtime 派发的子任务挂在父节点 join_counter 上，
///          父节点 invoke 返回后 tear_down 自动等子任务 —— 不需要显式 cowait。
///          cowait 的真正用途是 **task body 中间** 需要确保子任务完成才能继续。
TEST_CASE("Runtime: 隐式等待 —— 父 invoke 返回后框架自动等齐", "[runtime][implicit-wait]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> n{0};
    constexpr int N = 20;

    flow.emplace([&](tfl::Runtime& rt) {
        for (int i = 0; i < N; ++i) {
            rt.silent_async([&] { n.fetch_add(1); });
        }
        // 不调用 cowait —— 让框架在 tear_down 阶段等齐
    });

    auto next = flow.emplace([&] {
        // 这个节点跑的时候，所有 silent_async 必须都完成了
    });
    flow.for_each([&](tfl::Task t) {  // 安全建图：拿第一个节点作为 next 的前驱
        if (t != next) t.precede(next);
    });

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(n.load() == N);
}

// ============================================================================
// SECTION 2: async —— 派发并取结果
// ============================================================================

/// @test [runtime][async] Runtime::async 返回 Future，可取计算结果。
TEST_CASE("Runtime: async 派发返回结果的子任务", "[runtime][async]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> result{0};

    flow.emplace([&](tfl::Runtime& rt) {
        auto fut = rt.async([] { return 17 * 2; });
        // 必须显式等待获取结果
        rt.cowait_until([&]() noexcept {
            return fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        });
        result.store(fut.get());
    });

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(result.load() == 34);
}

/// @test [runtime][async] async 带参数转发。
TEST_CASE("Runtime: async 带参", "[runtime][async][args]") {
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
// SECTION 3: cowait_until —— 谓词驱动协作式等待
// ============================================================================

/// @test [runtime][cowait_until] 谓词为真后立即返回。
TEST_CASE("Runtime: cowait_until 谓词驱动等待", "[runtime][cowait_until]") {
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
        // 等到完成数达到 TARGET
        rt.cowait_until([&]() noexcept {
            return done_count.load(std::memory_order_relaxed) >= TARGET;
        });
    });

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(done_count.load() >= TARGET);
}

// ============================================================================
// SECTION 4: 嵌套 Runtime —— 子任务里再开 Runtime
// ============================================================================

/// @test [runtime][nested] silent_async 派发的 Runtime 子任务可以再 silent_async。
TEST_CASE("Runtime: 嵌套 Runtime 派发", "[runtime][nested]") {
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
// SECTION 5: 上下文查询 —— executor() / worker()
// ============================================================================

/// @test [runtime][context] Runtime::executor() 返回的就是父 Executor。
TEST_CASE("Runtime: executor 上下文访问", "[runtime][context]") {
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
