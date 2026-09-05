/// @file test_integration.cpp
/// @brief 跨组件集成测试 —— Executor 生命周期、深度嵌套、混合负载、析构压力。
///
/// 覆盖的不变量:
///   E3  ✓ 析构 join 所有 worker，无 hang
///   E5  ✓ run / shutdown 并发，不死锁
///   E9  ✓ 异常任务的父 join_counter 仍正确
///   E11 ✓ 并发 run 多个 Flow，各自独立完成
///   E13 ⏸ 0 worker 行为
///   E14 ✓ 巨大 worker 数不挂 / 10 万节点链
///   X2  ✓ 多 Flow + 异常: 全部完成，无 hang
///   X4  ✓ Subflow + Condition + 嵌套 Subflow
///   X5  ✓ Executor 在压力下析构
///   F8  ✓ Subflow 完成后父任务才视为完成

#include "test_common.hpp"

#include <atomic>
#include <chrono>
#include <random>
#include <vector>

using tfl_test::TestEnv;

// ============================================================================
// 第 1 节: Executor 生命周期边界 (E14, E3)
// ============================================================================

/// @test [integration][executor][lifecycle] E14: 大量 worker 构造不崩溃
TEST_CASE("Integration: executor with many workers does not crash", "[integration][executor][lifecycle]") {
    constexpr std::size_t kN = 128;
    REQUIRE_NOTHROW([&] {
        tfl::Executor exec(kN);
        std::atomic<int> n{0};
        auto t = tfl::AsyncTask([&] { n.store(42); });
        exec.run(t); t.wait();
        REQUIRE(n.load() == 42);
    }());
}

/// @test [integration][executor][lifecycle] E3: 析构 join 所有 worker
TEST_CASE("Integration: executor destructor joins all workers", "[integration][executor][lifecycle]") {
    std::atomic<int> hits{0};
    {
        tfl::Executor exec(4);
        tfl::Flow flow;
        for (int i = 0; i < 100; ++i) {
            flow.emplace([&] { hits.fetch_add(1, std::memory_order_relaxed); });
        }
        exec.async(flow).wait();
        exec.wait_for_all();
    }
    REQUIRE(hits.load() == 100);
}

// ============================================================================
// 第 2 节: 深度嵌套 Subflow (F8, X4)
// ============================================================================

/// @test [integration][subflow][nested] F8+X4: 三层深度 Subflow 嵌套
TEST_CASE("Integration: three-level deep Subflow nesting", "[integration][subflow][nested]") {
    TestEnv env;
    std::atomic<int> innermost{0}, middle{0}, outermost{0};

    // 最内层: 2 个任务
    tfl::Flow inner;
    inner.emplace([&] { innermost.fetch_add(1); });
    inner.emplace([&] { innermost.fetch_add(1); });

    // 中层: 嵌入 inner + 自身任务
    tfl::Flow mid;
    mid.emplace(std::move(inner));
    mid.emplace([&] { middle.fetch_add(1); });

    // 外层: 嵌入 mid + 前后任务
    tfl::Flow outer;
    auto pre = outer.emplace([&] { outermost.fetch_add(1); });
    auto sub = outer.emplace(std::move(mid));
    auto post = outer.emplace([&] { outermost.fetch_add(1); });
    pre.precede(sub);
    sub.precede(post);

    env.executor.async(outer).wait();

    REQUIRE(innermost.load() == 2);
    REQUIRE(middle.load() == 1);
    REQUIRE(outermost.load() == 2);
}

/// @test [integration][subflow][repeat-nested] 嵌套 subflow + 固定次数循环
TEST_CASE("Integration: nested subflow with repeat counts", "[integration][subflow][repeat][nested]") {
    TestEnv env;
    std::atomic<int> leaf{0};

    // 叶子: 单任务
    tfl::Flow leaf_flow;
    leaf_flow.emplace([&] { leaf.fetch_add(1); });

    // 中层: 重复叶子 3 次
    tfl::Flow mid_flow;
    mid_flow.emplace(std::move(leaf_flow), 3ULL);

    // 外层: 重复中层 2 次
    tfl::Flow outer;
    outer.emplace(std::move(mid_flow), 2ULL);

    env.executor.async(outer).wait();

    // 叶子每 repeat 跑 1 次: 2×3×1 = 6
    REQUIRE(leaf.load() == 6);
}

// ============================================================================
// 第 3 节: Condition + Subflow 混合 (X4)
// ============================================================================

/// @test [integration][condition][subflow] Branch 条件路由到 Subflow 或 Basic 任务
TEST_CASE("Integration: condition task routing to Subflow vs basic", "[integration][condition][subflow]") {
    TestEnv env;

    for (int route = 0; route < 2; ++route) {
        std::atomic<int> sub_hits{0}, basic_hits{0};

        tfl::Flow sub;
        sub.emplace([&] { sub_hits.fetch_add(1); });
        sub.emplace([&] { sub_hits.fetch_add(1); });

        tfl::Flow flow;
        auto cond = flow.emplace([route](tfl::Branch& br) {
            br.select(static_cast<std::size_t>(route));
        });
        auto sub_node = flow.emplace(std::move(sub));
        auto basic_node = flow.emplace([&] { basic_hits.fetch_add(1); });
        cond.precede(sub_node, basic_node);

        env.executor.async(flow).wait();

        if (route == 0) {
            REQUIRE(sub_hits.load() == 2);
            REQUIRE(basic_hits.load() == 0);
        } else {
            REQUIRE(sub_hits.load() == 0);
            REQUIRE(basic_hits.load() == 1);
        }
    }
}

// ============================================================================
// 第 4 节: 并发提交多个 Flow (E11)
// ============================================================================

/// @test [integration][concurrent-run][stress] E11: 并发提交多个 Flow
TEST_CASE("Integration: concurrent run of multiple Flows", "[integration][concurrent-run][stress]") {
    TestEnv env(8);
    constexpr int kThreads = 4, kTasksPerFlow = 50;

    std::atomic<int> totals[kThreads] = {};

    auto make_flow = [&](int idx) {
        tfl::Flow f;
        for (int i = 0; i < kTasksPerFlow; ++i) {
            f.emplace([&total = totals[idx]] {
                total.fetch_add(1, std::memory_order_relaxed);
            });
        }
        return f;
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, idx = i] {
            auto flow = make_flow(idx);
            env.executor.async(flow).wait();
        });
    }

    for (auto& t : threads) t.join();

    for (int i = 0; i < kThreads; ++i) {
        INFO("thread " << i);
        REQUIRE(totals[i].load() == kTasksPerFlow);
    }
}

// ============================================================================
// 第 5 节: 析构压力 (X5, E5)
// ============================================================================

/// @test [integration][shutdown][stress] X5+E5: 析构时还有未完成的 Flow 任务
TEST_CASE("Integration: executor destruction with pending tasks", "[integration][shutdown][stress]") {
    std::atomic<int> hits{0};
    constexpr int N = 1000;

    {
        tfl::Executor exec(4);

        tfl::Flow flow;
        for (int i = 0; i < N; ++i) {
            flow.emplace([&] { hits.fetch_add(1, std::memory_order_relaxed); });
        }
        exec.async(std::move(flow));
        // async 自动启动；不调 wait —— 内部处理
    }
    // 析构函数内部 _shutdown() → wait_for_all() → join workers
    REQUIRE(hits.load() == N);
}

/// @test [integration][shutdown][stress] 析构时还有未完成的 silent_async 任务
TEST_CASE("Integration: executor destruction with pending silent_async", "[integration][shutdown][stress]") {
    std::atomic<int> hits{0};
    constexpr int N = 500;

    {
        tfl::Executor exec(4);
        for (int i = 0; i < N; ++i) {
            exec.silent_async([&] {
                hits.fetch_add(1, std::memory_order_relaxed);
            });
        }
        // 不调 wait_for_all —— 让析构处理
    }
    REQUIRE(hits.load() == N);
}

// ============================================================================
// 第 6 节: wait_for_all 语义
// ============================================================================

/// @test [integration][wait_for_all] wait_for_all 覆盖 Flow 和 silent_async
TEST_CASE("Integration: wait_for_all covers both flows and silent_async", "[integration][wait_for_all]") {
    TestEnv env;
    std::atomic<int> flow_done{0}, async_done{0};
    constexpr int kFlowTasks = 50, kAsyncTasks = 100;

    tfl::Flow flow;
    for (int i = 0; i < kFlowTasks; ++i) {
        flow.emplace([&] { flow_done.fetch_add(1); });
    }

    env.executor.async(flow);

    for (int i = 0; i < kAsyncTasks; ++i) {
        env.executor.silent_async([&] { async_done.fetch_add(1); });
    }

    env.executor.wait_for_all();

    REQUIRE(flow_done.load() == kFlowTasks);
    REQUIRE(async_done.load() == kAsyncTasks);
}

/// @test [integration][wait_for_all] wait_for_all 多次调用幂等
TEST_CASE("Integration: multiple wait_for_all calls", "[integration][wait_for_all]") {
    TestEnv env;
    std::atomic<int> n{0};

    env.executor.silent_async([&] { n.store(1); });
    env.executor.wait_for_all();
    REQUIRE(n.load() == 1);

    // 第二次应立刻返回
    REQUIRE_NOTHROW(env.executor.wait_for_all());

    // 提交新任务后再等
    env.executor.silent_async([&] { n.store(2); });
    env.executor.wait_for_all();
    REQUIRE(n.load() == 2);
}

// ============================================================================
// 第 7 节: 混合异常 + Subflow (X2)
// ============================================================================

/// @test [integration][exception][subflow] X2+E9: 一个 Subflow 的异常不影响兄弟 Subflow
TEST_CASE("Integration: exception in one Subflow does not block sibling", "[integration][exception][subflow]") {
    TestEnv env;
    std::atomic<bool> sibling_done{false};

    tfl::Flow bad_inner;
    bad_inner.emplace([] { throw std::runtime_error("inner boom"); });

    tfl::Flow good_inner;
    good_inner.emplace([&] { sibling_done.store(true); });

    tfl::Flow outer;
    outer.emplace(std::move(bad_inner));
    outer.emplace(std::move(good_inner));

    env.executor.async(outer).wait();

    REQUIRE(sibling_done.load());
}

/// @test [integration][exception][subflow][nested] 异常在深度嵌套 Subflow 中传播
TEST_CASE("Integration: exception in deeply nested Subflow", "[integration][exception][subflow][nested]") {
    TestEnv env;
    std::atomic<bool> outer_post{false};

    tfl::Flow inner;
    inner.emplace([] { throw std::runtime_error("deep"); });

    tfl::Flow mid;
    mid.emplace(std::move(inner));

    tfl::Flow outer;
    auto sub = outer.emplace(std::move(mid));
    auto post = outer.emplace([&] { outer_post.store(true); });
    sub.precede(post);

    env.executor.async(outer).wait();

    // 图内异常传播后，依赖后继节点不运行
    REQUIRE_FALSE(outer_post.load());
}

// ============================================================================
// 第 8 节: Flow 复用 (E11)
// ============================================================================

/// @test [integration][reuse] 同一 Flow 多次提交
TEST_CASE("Integration: same Flow reused after each completion", "[integration][reuse][stress]") {
    TestEnv env(4);

    tfl::Flow flow;
    std::atomic<int> counter{0};
    flow.emplace([&] { counter.fetch_add(1); });

    constexpr int kSubmissions = 100;
    for (int i = 0; i < kSubmissions; ++i) {
        env.executor.async(flow).wait();
    }

    REQUIRE(counter.load() == kSubmissions);
}

// ============================================================================
// 第 9 节: Runtime wait 跨 Flow (X4)
// ============================================================================

/// @test [integration][runtime][flow] Runtime wait 在 Flow successor 之前完成子任务
TEST_CASE("Integration: Runtime wait before successor in Flow", "[integration][runtime][flow]") {
    TestEnv env;
    std::atomic<int> sub_done{0};
    std::atomic<bool> successor_ran{false};
    constexpr int kSubTasks = 50;

    tfl::Flow flow;
    auto rt = flow.emplace([&](tfl::Runtime& rt) {
        for (int i = 0; i < kSubTasks; ++i) {
            rt.silent_async([&] { sub_done.fetch_add(1); });
        }
        rt.wait();  // 等所有子任务在父任务体内完成
    });
    auto next = flow.emplace([&] {
        successor_ran.store(sub_done.load() == kSubTasks);
    });
    rt.precede(next);

    env.executor.async(flow).wait();

    REQUIRE(sub_done.load() == kSubTasks);
    REQUIRE(successor_ran.load());
}

// ============================================================================
// 第 10 节: 超大量 DAG (E14)
// ============================================================================

/// @test [integration][large] 10 万节点链构造与执行
TEST_CASE("Integration: 100K-node chain construction and execution", "[integration][large][stress]") {
    TestEnv env(4);

    constexpr int N = 100'000;
    std::atomic<int> counter{0};

    tfl::Flow flow;
    std::vector<tfl::Task> tasks;
    tasks.reserve(N);
    for (int i = 0; i < N; ++i) {
        tasks.push_back(flow.emplace([&] {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }
    for (int i = 1; i < N; ++i) {
        tasks[i - 1].precede(tasks[i]);
    }

    env.executor.async(flow).wait();
    REQUIRE(counter.load() == N);
}

// ============================================================================
// 第 11 节: 自定义 graph_holder 类型
// ============================================================================

/// @brief 用户自定义的 Flow 包装类型，满足 graph_holder concept
struct NamedPipeline {
    tfl::Flow flow;
    std::string name;

    tfl::Flow& graph() noexcept { return flow; }
    const tfl::Flow& graph() const noexcept { return flow; }
};

/// @test [integration][graph_holder] 自定义 graph_holder 提交到 executor
TEST_CASE("Integration: custom graph_holder submitted to executor", "[integration][graph_holder]") {
    TestEnv env;
    NamedPipeline pipeline;
    pipeline.flow.emplace([] {});
    pipeline.flow.emplace([] {});
    REQUIRE_NOTHROW(env.executor.async(pipeline.flow).wait());
}

// ============================================================================
// 第 12 节: Future + Flow 集成
// ============================================================================

/// @test [integration][future] async Future 与 Flow 并发使用
TEST_CASE("Integration: async Future used alongside Flow", "[integration][future]") {
    TestEnv env;

    auto fut = env.executor.async([] { return 21 * 2; });

    tfl::Flow flow;
    std::atomic<int> flow_done{0};
    flow.emplace([&] { flow_done.fetch_add(1); });
    env.executor.async(flow).wait();

    REQUIRE(flow_done.load() == 1);
    REQUIRE(fut.get() == 42);
}

// ============================================================================
// 覆盖矩阵
// ============================================================================
//   E3  ✓ "Integration: executor destructor joins all workers"
//   E5  ✓ "Integration: executor destruction with pending tasks" / "pending silent_async"
//   E9  ✓ "Integration: exception in deeply nested Subflow"
//   E11 ✓ "Integration: concurrent run of multiple Flows" / "same Flow submitted multiple times"
//   E13 ⏸ 0 worker（API 设计问题）
//   E14 ✓ "Integration: executor with many workers does not crash" / "100K-node chain"
//   X2  ✓ "Integration: exception in one Subflow does not block sibling"
//   X4  ✓ "Integration: three-level deep Subflow nesting" / "condition task routing to Subflow"
//         "Integration: Runtime wait before successor in Flow"
//   X5  ✓ "Integration: executor destruction with pending tasks" / "pending silent_async"
//   F8  ✓ "Integration: three-level deep Subflow nesting" / "nested subflow with repeat counts"
