/// @file test_edge_cases.cpp
/// @brief 边界条件测试 — 空图 / 单节点 / 复用 / 不连通图 / 最小线程池。
///
/// 这些测试容易被忽略，但往往是真实 bug 所在之处。
/// 参考来源：GoogleTest 风格的"零、一、多"覆盖原则。

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: 空 Flow
// ============================================================================

/// @test [edge][empty] 提交空 Flow 不会出错。
/// @details 用户可能从零次迭代的循环中构造空 Flow；框架不应崩溃。
TEST_CASE("Edge: Empty Flow submission without error", "[edge][empty]") {
    TestEnv env;
    tfl::Flow flow;
    REQUIRE(flow.empty());
    REQUIRE_NOTHROW(env.executor.deferred_async(flow).start().wait());
}

/// @test [edge][empty][callback] 空 Flow 带回调 — 回调仍然会被调用。
/// @details 即使没有任务运行，完成回调的语义也应当被保留。
TEST_CASE("Edge: Empty Flow with callback", "[edge][empty][callback]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> cb{0};

    env.executor.deferred_async(flow, [&]() noexcept { cb.store(1); }).start().wait();
    REQUIRE(cb.load() == 1);
}

/// @test [edge][empty][repeat] 空 Flow 重复执行 5 次。
TEST_CASE("Edge: Empty Flow repeated execution", "[edge][empty][repeat]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> cb{0};

    env.executor.deferred_async(flow, 5ULL, [&]() noexcept { cb.fetch_add(1); })
                .start().wait();
    REQUIRE(cb.load() == 1);  // 回调仅执行一次
}

// ============================================================================
// SECTION 2: 单节点
// ============================================================================

/// @test [edge][single] 单节点 Flow。
TEST_CASE("Edge: Single-node Flow", "[edge][single]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> n{0};
    flow.emplace([&] { n.store(42); });

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(n.load() == 42);
}

/// @test [edge][single][repeat] 单节点重复执行 N 次。
TEST_CASE("Edge: Single node repeated N times", "[edge][single][repeat]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> n{0};
    flow.emplace([&] { n.fetch_add(1); });

    env.executor.deferred_async(flow, 50ULL).start().wait();
    REQUIRE(n.load() == 50);
}

// ============================================================================
// SECTION 3: Flow 复用
// ============================================================================

/// @test [edge][reuse] 同一个 Flow 提交多次 — 每次运行相互独立。
/// @details 验证在 deferred_async 调用之间，Flow 的拓扑/状态能正确重置。
TEST_CASE("Edge: Same Flow submitted multiple times", "[edge][reuse]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> n{0};

    auto a = flow.emplace([&] { n.fetch_add(1); });
    auto b = flow.emplace([&] { n.fetch_add(10); });
    a.precede(b);

    constexpr int RUNS = 100;
    for (int i = 0; i < RUNS; ++i) {
        env.executor.deferred_async(flow).start().wait();
    }

    // 每次运行增加 11
    REQUIRE(n.load() == 11 * RUNS);
}

// ============================================================================
// SECTION 4: 多个不连通子图
// ============================================================================

/// @test [edge][disconnected] 同一 Flow 中包含多个不连通子图。
/// @details 这本质上是合法的 DAG；验证调度器能正确启动每一个连通分量。
TEST_CASE("Edge: Disconnected subgraphs", "[edge][disconnected]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> a_hits{0}, b_hits{0}, c_hits{0};

    // 子图 A: 链式
    auto a1 = flow.emplace([&] { a_hits.fetch_add(1); });
    auto a2 = flow.emplace([&] { a_hits.fetch_add(1); });
    a1.precede(a2);

    // 子图 B: 菱形
    auto b1 = flow.emplace([&] { b_hits.fetch_add(1); });
    auto b2 = flow.emplace([&] { b_hits.fetch_add(1); });
    auto b3 = flow.emplace([&] { b_hits.fetch_add(1); });
    auto b4 = flow.emplace([&] { b_hits.fetch_add(1); });
    b1.precede(b2, b3);
    b4.succeed(b2, b3);

    // 子图 C: 孤立单节点
    flow.emplace([&] { c_hits.fetch_add(1); });

    env.executor.deferred_async(flow).start().wait();

    REQUIRE(a_hits.load() == 2);
    REQUIRE(b_hits.load() == 4);
    REQUIRE(c_hits.load() == 1);
}

// ============================================================================
// SECTION 5: 大量小 Flow 的重复构造
// ============================================================================

/// @test [edge][lifecycle] 反复构造和销毁 Flow。
/// @details 模拟"每个请求一个 Flow"的服务器场景。
TEST_CASE("Edge: Repeated construct/destroy Flow", "[edge][lifecycle]") {
    TestEnv env;
    constexpr int ITERS = 200;
    std::atomic<int> hits{0};

    for (int i = 0; i < ITERS; ++i) {
        tfl::Flow flow;
        flow.emplace([&] { hits.fetch_add(1); });
        flow.emplace([&] { hits.fetch_add(1); });
        env.executor.deferred_async(flow).start().wait();
    }

    REQUIRE(hits.load() == ITERS * 2);
}

// ============================================================================
// SECTION 6: clear 后复用
// ============================================================================

/// @test [edge][clear] Flow::clear 后继续 emplace 仍正常工作。
TEST_CASE("Edge: Reuse after clear", "[edge][clear]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> n{0};

    flow.emplace([&] { n.store(1); });
    env.executor.deferred_async(flow).start().wait();
    REQUIRE(n.load() == 1);

    flow.clear();
    REQUIRE(flow.empty());

    flow.emplace([&] { n.store(99); });
    env.executor.deferred_async(flow).start().wait();
    REQUIRE(n.load() == 99);
}

// ============================================================================
// SECTION 7: 两次提交之间编辑图
// ============================================================================

/// @test [edge][edit] 两次提交之间添加/删除边。
/// @details 验证图编辑阶段（用户层）与执行阶段（框架层）之间的状态隔离。
TEST_CASE("Edge: Edit graph between submissions", "[edge][edit]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> hits{0};

    auto a = flow.emplace([&] { hits.fetch_add(1); });
    auto b = flow.emplace([&] { hits.fetch_add(1); });
    a.precede(b);

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(hits.load() == 2);

    // 添加新节点 c，链接在 b 之后
    auto c = flow.emplace([&] { hits.fetch_add(1); });
    b.precede(c);

    env.executor.deferred_async(flow).start().wait();
    // a/b/c 各运行一次 = +3 = 5
    REQUIRE(hits.load() == 5);
}

// ============================================================================
// SECTION 8: 超大节点数（仅构图）
// ============================================================================

/// @test [edge][large-graph] 5 万节点图的构造（不执行）。
/// @details 检查 Flow 能否容纳大规模图；emplace + precede 接口在大型图上不应退化。
TEST_CASE("Edge: 50K-node graph construction", "[edge][large-graph]") {
    constexpr int N = 50'000;

    tfl::Flow flow;
    std::vector<tfl::Task> tasks;
    tasks.reserve(N);
    for (int i = 0; i < N; ++i) {
        tasks.push_back(flow.emplace([] {}));
    }
    REQUIRE(flow.size() == N);

    // 每个节点连接到其后 5 个（如果存在）
    for (int i = 0; i < N; ++i) {
        for (int k = 1; k <= 5 && i + k < N; ++k) {
            tasks[i].precede(tasks[i + k]);
        }
    }

    // 第一个节点入度应为 0，最后一个节点出度应为 0
    REQUIRE(tasks[0].num_predecessors() == 0);
    REQUIRE(tasks[N - 1].num_successors() == 0);
}
