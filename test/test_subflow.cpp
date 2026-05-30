/// @file test_subflow.cpp
/// @brief Subflow 测试 —— 子图嵌套、固定次数循环、谓词驱动循环。
///
/// 覆盖的接口：
///   - Flow::emplace(Gh)                       挂载子图（默认 1 次）
///   - Flow::emplace(Gh, num)                  固定次数循环 num 次
///   - Flow::emplace(Gh, predicate)            谓词驱动循环
///   - Subgraph 右值（移动）/ 子图左值（reference_wrapper）
///
/// 关键不变式：
///   1. 子图作为单个节点参与父图依赖；
///   2. 固定次数循环：子图执行 num 次后再驱动后继节点；
///   3. 谓词循环：谓词返回 true 表示停止循环；
///   4. 通过 lambda 捕获的父图状态可与子图共享。

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: 基本嵌套 —— 子图执行一次
// ============================================================================

/// @test [subflow][basic] 子图作为单个节点嵌入父图，执行一次。
TEST_CASE("Subflow: basic nesting execution", "[subflow][basic]") {
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

    env.executor.async(main_flow).wait();
    REQUIRE(outer_count.load() == 2);
    REQUIRE(inner_count.load() == 2);
}

// ============================================================================
// SECTION 2: 固定次数循环
// ============================================================================

/// @test [subflow][repeat] emplace(sub, N) 使子图执行 N 次。
TEST_CASE("Subflow: fixed-count loop", "[subflow][repeat]") {
    TestEnv env;
    std::atomic<int> hits{0};
    constexpr int LOOPS = 4;

    tfl::Flow inner;
    inner.emplace([&] { hits.fetch_add(1); });

    tfl::Flow main_flow;
    main_flow.emplace(std::move(inner), static_cast<std::uint64_t>(LOOPS));

    env.executor.async(main_flow).wait();
    REQUIRE(hits.load() == LOOPS);
}

// ============================================================================
// SECTION 3: 谓词驱动循环
// ============================================================================

/// @test [subflow][predicate] 谓词返回 true 时循环停止。
/// @details 谓词在每次子图完成后调用一次；返回 true 表示"够了"。
TEST_CASE("Subflow: predicate-driven loop", "[subflow][predicate]") {
    TestEnv env;
    std::atomic<int> hits{0};
    constexpr int TARGET = 6;

    tfl::Flow inner;
    inner.emplace([&] { hits.fetch_add(1); });

    tfl::Flow main_flow;
    int loops = 0;
    main_flow.emplace(std::move(inner), [&loops]() mutable noexcept {
        return loops++ >= TARGET;  // 返回 true 表示 TARGET 次迭代后停止
    });

    env.executor.async(main_flow).wait();
    REQUIRE(hits.load() == TARGET);
}

// ============================================================================
// SECTION 4: 子图共享外部状态
// ============================================================================

/// @test [subflow][capture] 父图和子图通过 lambda 捕获共享外部原子状态。
TEST_CASE("Subflow: parent and subgraph share external atomic", "[subflow][capture]") {
    TestEnv env;
    std::atomic<int> shared{0};

    tfl::Flow inner;
    inner.emplace([&] { shared.fetch_add(10); });
    inner.emplace([&] { shared.fetch_add(100); });

    tfl::Flow main_flow;
    main_flow.emplace([&] { shared.fetch_add(1); });
    auto sub = main_flow.emplace(std::move(inner));
    main_flow.emplace([&] { shared.fetch_add(1000); }).succeed(sub);

    // pre 节点与 sub 节点没有显式 precede —— 更安全的做法是将 pre 前置串联
    // 这里仅验证累积的共享结果
    env.executor.async(main_flow).wait();
    REQUIRE(shared.load() == 1 + 10 + 100 + 1000);
}

// ============================================================================
// SECTION 5: 子图左值（reference_wrapper）
// ============================================================================

/// @test [subflow][lvalue] 以左值传入子图 → reference_wrapper，子图生命周期由外部管理。
TEST_CASE("Subflow: lvalue mounting", "[subflow][lvalue]") {
    TestEnv env;
    std::atomic<int> hits{0};

    tfl::Flow persistent_sub;        // 必须在 async 调用后仍然存活
    persistent_sub.emplace([&] { hits.fetch_add(1); });

    tfl::Flow main_flow;
    main_flow.emplace(persistent_sub);  // 左值传入

    env.executor.async(main_flow).wait();
    REQUIRE(hits.load() == 1);
}
