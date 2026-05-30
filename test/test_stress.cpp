/// @file test_stress.cpp
/// @brief 压力与属性测试 —— 大规模任务、深链、宽扇出、随机 DAG 拓扑序验证。
///
/// 这里混合了两种测试类型：
///
///   1. **压力测试**：大规模，验证调度器在大量任务下正确工作；
///   2. **属性测试**：使用随机生成的输入验证不变式（拓扑序、计数等）。
///
/// 随机测试使用 std::mt19937 并设置固定种子，确保失败可复现。

#include "test_common.hpp"

#include <random>
#include <set>
#include <numeric>

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: 大规模即发即忘
// ============================================================================

/// @test [stress][async] 100K detach 任务全部完成且结果正确
TEST_CASE("Stress: 100K detach", "[stress][async]") {
    TestEnv env(8);
    constexpr int N = 100'000;
    std::atomic<long> sum{0};

    for (int i = 0; i < N; ++i) {
        env.executor.detach([&, i] {
            sum.fetch_add(i, std::memory_order_relaxed);
        });
    }
    env.executor.wait_for_all();

    // 求和 0..N-1 = N*(N-1)/2
    REQUIRE(sum.load() == static_cast<long>(N) * (N - 1) / 2);
}

// ============================================================================
// SECTION 2: 深链
// ============================================================================

/// @test [stress][chain] 10K 深度串行链，每个节点必须看到前驱已完成
/// @details 使用 step.exchange(i+1) == i 验证严格顺序；违规计数必须为 0。
TEST_CASE("Stress: 10K deep chain ordering guarantee", "[stress][chain][deep]") {
    TestEnv env(4);
    tfl::Flow flow;
    constexpr int N = 10'000;
    std::atomic<int> step{0};
    std::atomic<int> violations{0};

    std::vector<tfl::Task> tasks;
    tasks.reserve(N);
    for (int i = 0; i < N; ++i) {
        auto t = flow.emplace([&, i] {
            int prev = step.exchange(i + 1);
            if (prev != i) violations.fetch_add(1);
        });
        tasks.push_back(t);
    }
    for (int i = 1; i < N; ++i) tasks[i - 1].precede(tasks[i]);

    env.executor.async(flow).wait();
    REQUIRE(step.load() == N);
    REQUIRE(violations.load() == 0);
}

// ============================================================================
// SECTION 3: 宽扇出
// ============================================================================

/// @test [stress][wide] root → 10K 并行节点 → sink
TEST_CASE("Stress: 10K wide fanout + converge", "[stress][wide]") {
    TestEnv env(8);
    tfl::Flow flow;
    constexpr int N = 10'000;
    std::atomic<int> hits{0};
    std::atomic<bool> sink_saw_all{false};

    auto root = flow.emplace([] {});
    auto sink = flow.emplace([&] {
        sink_saw_all.store(hits.load() == N);
    });
    for (int i = 0; i < N; ++i) {
        auto t = flow.emplace([&] {
            hits.fetch_add(1, std::memory_order_relaxed);
        });
        root.precede(t);
        t.precede(sink);
    }

    env.executor.async(flow).wait();
    REQUIRE(hits.load() == N);
    REQUIRE(sink_saw_all.load());
}

// ============================================================================
// SECTION 4: 突发提交
// ============================================================================

/// @test [stress][burst] 依次提交 1000 个独立 Flow
/// @details 验证 Executor 在连续提交大量短作业时不会泄漏或丢失任务。
TEST_CASE("Stress: 1000 independent Flows submitted sequentially", "[stress][burst]") {
    TestEnv env;
    constexpr int FLOWS = 1'000;
    constexpr int PER_FLOW = 4;
    std::atomic<int> total{0};

    for (int f = 0; f < FLOWS; ++f) {
        tfl::Flow flow;
        for (int i = 0; i < PER_FLOW; ++i) {
            flow.emplace([&] { total.fetch_add(1); });
        }
        env.executor.async(flow).wait();
    }

    REQUIRE(total.load() == FLOWS * PER_FLOW);
}

// ============================================================================
// SECTION 5: 随机 DAG + 拓扑序属性验证
// ============================================================================

/// @test [stress][random][property] 200 节点随机 DAG，验证执行顺序符合拓扑序
/// @details 节点 i 只能依赖于 [0, i)，保证无环。
///          全局原子计数器 global_order 为每个节点分配执行序号。
///          属性：对于每条边 u→v，exec_order(u) < exec_order(v)。
///
///          固定种子 42 → 失败可复现。
TEST_CASE("Stress: random DAG topological order correctness", "[stress][random][property]") {
    TestEnv env(8);

    constexpr int N = 200;
    constexpr int MAX_DEPS_PER_NODE = 5;
    std::mt19937 rng(42);

    struct NodeInfo {
        std::vector<int> deps;
        std::atomic<int> exec_order{-1};
    };
    std::vector<NodeInfo> info(N);

    // 随机依赖：节点 i 从 [0, i) 中选择 0..min(MAX_DEPS, i) 个前驱
    for (int i = 0; i < N; ++i) {
        if (i == 0) continue;
        int max_deps = std::min(MAX_DEPS_PER_NODE, i);
        int num_deps = std::uniform_int_distribution<>(0, max_deps)(rng);
        std::set<int> dep_set;
        while (static_cast<int>(dep_set.size()) < num_deps) {
            dep_set.insert(std::uniform_int_distribution<>(0, i - 1)(rng));
        }
        info[i].deps.assign(dep_set.begin(), dep_set.end());
    }

    tfl::Flow flow;
    std::atomic<int> global_order{0};
    std::vector<tfl::Task> tasks;
    tasks.reserve(N);
    for (int i = 0; i < N; ++i) {
        auto t = flow.emplace([&, i] {
            info[i].exec_order.store(global_order.fetch_add(1));
        });
        tasks.push_back(t);
    }
    for (int i = 0; i < N; ++i) {
        for (int dep : info[i].deps) {
            tasks[dep].precede(tasks[i]);
        }
    }

    env.executor.async(flow).wait();

    // 属性 1：所有节点均已执行
    for (int i = 0; i < N; ++i) {
        REQUIRE(info[i].exec_order.load() >= 0);
    }
    // 属性 2：边 u→v 满足拓扑序
    for (int i = 0; i < N; ++i) {
        for (int dep : info[i].deps) {
            REQUIRE(info[dep].exec_order.load() < info[i].exec_order.load());
        }
    }
}

// ============================================================================
// SECTION 6: 单工作线程最小线程池
// ============================================================================

/// @test [stress][single-worker] 1 个工作线程调度 100 个并行节点
/// @details 验证调度器能在最小并发资源下正确串行化执行。
TEST_CASE("Stress: single worker scheduling", "[stress][single-worker]") {
    TestEnv env(1);
    tfl::Flow flow;
    constexpr int N = 100;
    std::atomic<int> hits{0};

    for (int i = 0; i < N; ++i) {
        flow.emplace([&] { hits.fetch_add(1); });
    }

    env.executor.async(flow).wait();
    REQUIRE(hits.load() == N);
}

// ============================================================================
// SECTION 7: 工作窃取压力 —— 多 Runtime 嵌套派发
// ============================================================================

/// @test [stress][work-stealing] 16 个 Runtime，每个派发 100 个子任务
/// @details 模拟工作窃取场景：多个工作线程同时遇到 detach 派发，
///          应均衡负载，无死锁，无丢失任务。
TEST_CASE("Stress: work stealing with multiple Runtimes", "[stress][work-stealing]") {
    TestEnv env(8);
    tfl::Flow flow;
    constexpr int OUTER = 16, INNER = 100;
    std::atomic<int> hits{0};

    for (int o = 0; o < OUTER; ++o) {
        flow.emplace([&](tfl::Runtime& rt) {
            for (int i = 0; i < INNER; ++i) {
                rt.detach([&] {
                    hits.fetch_add(1, std::memory_order_relaxed);
                });
            }
            rt.cowait();
        });
    }

    env.executor.async(flow).wait();
    REQUIRE(hits.load() == OUTER * INNER);
}

// ============================================================================
// SECTION 8: 信号量限流压力
// ============================================================================

/// @test [stress][semaphore] 100 个任务竞争 4 个配额的信号量
/// @details 大量任务竞争少量配额，验证：
///          - 所有任务最终完成
///          - 历史峰值并发不超过配额
///          - 无死锁
TEST_CASE("Stress: semaphore rate-limiting heavy contention", "[stress][semaphore]") {
    TestEnv env(8);
    tfl::Semaphore sem{4};

    constexpr int N = 100;
    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    std::atomic<int> done{0};

    tfl::Flow flow;
    for (int i = 0; i < N; ++i) {
        auto t = flow.emplace([&] {
            int now = active.fetch_add(1) + 1;
            int prev = peak.load();
            while (now > prev && !peak.compare_exchange_weak(prev, now)) {}
            // 非常短的"工作"
            for (volatile int k = 0; k < 100; ++k) {}
            active.fetch_sub(1);
            done.fetch_add(1);
        });
        t.acquire(sem).release(sem);
    }

    env.executor.async(flow).wait();

    REQUIRE(done.load() == N);
    REQUIRE(peak.load() <= 4);
    REQUIRE(peak.load() > 0);
    REQUIRE(sem.value() == 4);  // 所有配额已归还
}

// ============================================================================
// SECTION 9: 混合工作负载
// ============================================================================

/// @test [stress][mixed] 在同一 Executor 上同时运行 Flow + detach
/// @details 验证 Executor 支持不同类型的工作单元并发执行，
///          wait_for_all 正确等待全部完成。
TEST_CASE("Stress: mixed Flow + detach", "[stress][mixed]") {
    TestEnv env(4);
    std::atomic<int> flow_hits{0};
    std::atomic<int> async_hits{0};

    // 提交 Flow（异步启动）
    tfl::Flow flow;
    for (int i = 0; i < 50; ++i) {
        flow.emplace([&] { flow_hits.fetch_add(1); });
    }
    auto flow_task = tfl::NonrepeatAsyncTask(flow);

    // 同时大量 detach
    for (int i = 0; i < 500; ++i) {
        env.executor.detach([&] { async_hits.fetch_add(1); });
    }

    env.executor.submit(flow_task);
    flow_task.wait();
    env.executor.wait_for_all();

    REQUIRE(flow_hits.load() == 50);
    REQUIRE(async_hits.load() == 500);
}
