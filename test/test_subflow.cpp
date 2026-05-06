/// @file test_subflow.cpp
/// @brief Subflow 测试 —— 子图嵌套、固定次数循环、谓词驱动循环。
///
/// 覆盖接口：
///   - Flow::emplace(Gh)                       挂载子图（默认 1 次）
///   - Flow::emplace(Gh, num)                  固定循环 num 次
///   - Flow::emplace(Gh, predicate)            谓词驱动循环
///   - 子图右值（move）/ 子图左值（reference_wrapper）
///
/// 关键不变量：
///   1. 子图作为单个节点参与父图依赖；
///   2. 固定循环：子图执行 num 次后才驱动后继；
///   3. 谓词循环：predicate 返回 true 表示停止循环；
///   4. 父图共享 lambda 捕获的状态可与子图共享。

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: 基本嵌套 —— 子图执行 1 次
// ============================================================================

/// @test [subflow][basic] 子图作为单个节点嵌入父图，执行 1 次。
TEST_CASE("Subflow: 基本嵌套执行", "[subflow][basic]") {
    TestEnv env;
    std::atomic<int> outer_count{0};
    std::atomic<int> inner_count{0};

    tfl::Flow inner;
    inner.emplace([&] { inner_count.fetch_add(1); });
    inner.emplace([&] { inner_count.fetch_add(1); });

    tfl::Flow main_flow;
    auto pre = main_flow.emplace([&] { outer_count.fetch_add(1); });
    auto sub = main_flow.emplace(std::move(inner));
    auto post = main_flow.emplace([&] { outer_count.fetch_add(1); });

    pre.precede(sub);
    sub.precede(post);

    env.executor.deferred_async(main_flow).start().wait();
    REQUIRE(outer_count.load() == 2);
    REQUIRE(inner_count.load() == 2);
}

// ============================================================================
// SECTION 2: 固定次数循环
// ============================================================================

/// @test [subflow][repeat] emplace(sub, N) 让子图执行 N 次。
TEST_CASE("Subflow: 固定次数循环", "[subflow][repeat]") {
    TestEnv env;
    std::atomic<int> hits{0};
    constexpr int LOOPS = 4;

    tfl::Flow inner;
    inner.emplace([&] { hits.fetch_add(1); });

    tfl::Flow main_flow;
    main_flow.emplace(std::move(inner), static_cast<std::uint64_t>(LOOPS));

    env.executor.deferred_async(main_flow).start().wait();
    REQUIRE(hits.load() == LOOPS);
}

// ============================================================================
// SECTION 3: 谓词驱动循环
// ============================================================================

/// @test [subflow][predicate] 谓词返回 true 时停止循环。
/// @details 谓词在每次子图完成后调用一次，返回 true 表示"足够了"。
TEST_CASE("Subflow: 谓词驱动循环", "[subflow][predicate]") {
    TestEnv env;
    std::atomic<int> hits{0};
    constexpr int TARGET = 6;

    tfl::Flow inner;
    inner.emplace([&] { hits.fetch_add(1); });

    tfl::Flow main_flow;
    int loops = 0;
    main_flow.emplace(std::move(inner), [&loops]() mutable noexcept {
        return loops++ >= TARGET;  // 跑够 TARGET 次后返回 true 停止
    });

    env.executor.deferred_async(main_flow).start().wait();
    REQUIRE(hits.load() == TARGET);
}

// ============================================================================
// SECTION 4: 子图共享外部状态
// ============================================================================

/// @test [subflow][capture] 主图与子图通过 lambda 捕获共享 atomic 状态。
TEST_CASE("Subflow: 主子图共享外部 atomic", "[subflow][capture]") {
    TestEnv env;
    std::atomic<int> shared{0};

    tfl::Flow inner;
    inner.emplace([&] { shared.fetch_add(10); });
    inner.emplace([&] { shared.fetch_add(100); });

    tfl::Flow main_flow;
    main_flow.emplace([&] { shared.fetch_add(1); });
    auto sub = main_flow.emplace(std::move(inner));
    main_flow.emplace([&] { shared.fetch_add(1000); }).succeed(sub);

    // pre 节点和 sub 节点都没显式 precede —— 先把 pre 串前面更稳妥
    // 这里只验证 shared 累加结果即可
    env.executor.deferred_async(main_flow).start().wait();
    REQUIRE(shared.load() == 1 + 10 + 100 + 1000);
}

// ============================================================================
// SECTION 5: 子图左值（reference_wrapper）
// ============================================================================

/// @test [subflow][lvalue] 子图传左值 → reference_wrapper，子图生命周期由外部管理。
TEST_CASE("Subflow: 左值挂载", "[subflow][lvalue]") {
    TestEnv env;
    std::atomic<int> hits{0};

    tfl::Flow persistent_sub;        // 必须存活到 deferred_async 之后
    persistent_sub.emplace([&] { hits.fetch_add(1); });

    tfl::Flow main_flow;
    main_flow.emplace(persistent_sub);  // 左值传入

    env.executor.deferred_async(main_flow).start().wait();
    REQUIRE(hits.load() == 1);
}
