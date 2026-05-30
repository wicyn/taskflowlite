/// @file test_patterns.cpp
/// @brief 经典并行模式测试 — 流水线 / Map-Reduce / 树形归约 / 波前 / 模板计算。
///
/// 参考来源：
///   - Taskflow 自带的示例（matrix_multiply / wavefront / parallel_reduce）
///   - Intel TBB 教程的应用模式
///   - Cilk++ 教材中的标准基准测试
///
/// 这些模式在工业代码中极为常见；作为集成测试，它们同时
/// 锻炼了 DAG 构造、调度、数据依赖、聚合等子系统。
///
/// 结果使用闭式公式验证，避免与实现形成循环引用。

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: 三阶段流水线 — 生产者 → 转换器 → 消费者
// ============================================================================

/// @test [pattern][pipeline] 三阶段流水线
/// @details ETL 风格：生成数据 → 转换 → 聚合。验证公式：0+2+4+...+198 = 9900。
TEST_CASE("Pattern: Three-stage Pipeline", "[pattern][pipeline]") {
    TestEnv env;
    tfl::Flow flow;

    constexpr int N = 100;
    std::vector<int> raw(N), processed(N);
    std::atomic<long> total{0};

    auto producer = flow.emplace([&] {
        for (int i = 0; i < N; ++i) raw[i] = i;
    });
    auto transformer = flow.emplace([&] {
        for (int i = 0; i < N; ++i) processed[i] = raw[i] * 2;
    });
    auto consumer = flow.emplace([&] {
        long s = 0;
        for (int v : processed) s += v;
        total.store(s);
    });

    producer.precede(transformer);
    transformer.precede(consumer);

    env.executor.async(flow).wait();
    REQUIRE(total.load() == 9900);  // 0+2+4+...+198
}

// ============================================================================
// SECTION 2: Map-Reduce — 并行 Map + 单点归约
// ============================================================================

/// @test [pattern][map-reduce] 平方和：sum_{k=1..N} k^2
/// @details N 路并行 map（独立无依赖），最终单点归约汇总。
///          闭式验证：N(N+1)(2N+1)/6 = 89440 (N=64)。
TEST_CASE("Pattern: Map-Reduce sum of squares", "[pattern][map-reduce]") {
    TestEnv env;
    tfl::Flow flow;

    constexpr int N = 64;
    std::vector<int> input(N), mapped(N);
    for (int i = 0; i < N; ++i) input[i] = i + 1;

    auto root = flow.emplace([] {});
    std::vector<tfl::Task> mappers;
    mappers.reserve(N);
    for (int i = 0; i < N; ++i) {
        auto m = flow.emplace([i, &input, &mapped] {
            mapped[i] = input[i] * input[i];
        });
        root.precede(m);
        mappers.push_back(m);
    }

    std::atomic<long> sum{0};
    auto reducer = flow.emplace([&] {
        long s = 0;
        for (int v : mapped) s += v;
        sum.store(s);
    });
    for (auto& m : mappers) m.precede(reducer);

    env.executor.async(flow).wait();
    REQUIRE(sum.load() == 64L * 65 * 129 / 6);  // = 89440
}

// ============================================================================
// SECTION 3: 二叉树归约 — 对数深度求和
// ============================================================================

/// @test [pattern][tree-reduce] 16 路输入，二叉树归约
/// @details 树形归约具有对数深度 = log2(N)，理论上比线性归约更快。
///          partial[1] 为根节点，存储 sum(1..16) = 136。
TEST_CASE("Pattern: Binary tree reduce", "[pattern][tree-reduce]") {
    TestEnv env;
    tfl::Flow flow;

    constexpr int N = 16;  // 必须为 2 的幂
    std::array<std::atomic<long>, 2 * N> partial;
    for (auto& p : partial) p.store(0);

    // 叶子节点：写入 partial[N..2N-1]
    std::vector<tfl::Task> level;
    level.reserve(N);
    for (int i = 0; i < N; ++i) {
        auto t = flow.emplace([i, &partial] {
            partial[N + i].store(i + 1);
        });
        level.push_back(t);
    }

    // 内部节点：partial[k] = partial[2k] + partial[2k+1]
    int idx_base = N / 2;
    while (level.size() > 1) {
        std::vector<tfl::Task> next_level;
        next_level.reserve(level.size() / 2);
        for (size_t i = 0; i < level.size(); i += 2) {
            int my_idx = idx_base + static_cast<int>(i / 2);
            auto t = flow.emplace([my_idx, &partial] {
                partial[my_idx].store(
                    partial[2 * my_idx].load() +
                    partial[2 * my_idx + 1].load());
            });
            level[i].precede(t);
            level[i + 1].precede(t);
            next_level.push_back(t);
        }
        level = std::move(next_level);
        idx_base /= 2;
    }

    env.executor.async(flow).wait();
    REQUIRE(partial[1].load() == N * (N + 1) / 2);  // = 136
}

// ============================================================================
// SECTION 4: 多层菱形 — 嵌套扇出/扇入
// ============================================================================

/// @test [pattern][diamond] 嵌套菱形拓扑
/// @details 4 层 x (4 路展开 + 1 次收缩)，验证调度器在多层菱形展开下的正确性。
TEST_CASE("Pattern: Multi-level diamond expand/contract", "[pattern][diamond]") {
    TestEnv env;
    tfl::Flow flow;

    constexpr int LEVELS = 4;
    constexpr int WIDTH = 4;
    std::atomic<int> total_runs{0};

    tfl::Task prev = flow.emplace([&] { total_runs.fetch_add(1); });
    for (int level = 0; level < LEVELS; ++level) {
        std::vector<tfl::Task> mid;
        mid.reserve(WIDTH);
        for (int i = 0; i < WIDTH; ++i) {
            auto t = flow.emplace([&] { total_runs.fetch_add(1); });
            prev.precede(t);
            mid.push_back(t);
        }
        auto next = flow.emplace([&] { total_runs.fetch_add(1); });
        for (auto& m : mid) m.precede(next);
        prev = next;
    }

    env.executor.async(flow).wait();
    // 1 个起始 + 4 层 x (4 个中间 + 1 个收缩) = 21
    REQUIRE(total_runs.load() == 1 + LEVELS * (WIDTH + 1));
}

// ============================================================================
// SECTION 5: 一维波前 — 链式波前
// ============================================================================

/// @test [pattern][wavefront-1d] 节点 i 依赖于 i-1
/// @details 类似于动态规划中的链式状态转移。
///          形式上虽是串行链，但要求 wave[i] = wave[i-1] + 1 严格成立。
TEST_CASE("Pattern: 1D Wavefront", "[pattern][wavefront-1d]") {
    TestEnv env;
    tfl::Flow flow;

    constexpr int N = 32;
    std::array<std::atomic<int>, N> wave;
    for (auto& w : wave) w.store(-1);

    std::vector<tfl::Task> tasks;
    tasks.reserve(N);
    for (int i = 0; i < N; ++i) {
        auto t = flow.emplace([i, &wave] {
            int prev = (i == 0) ? 0 : wave[i - 1].load();
            wave[i].store(prev + 1);
        });
        tasks.push_back(t);
    }
    for (int i = 1; i < N; ++i) tasks[i - 1].precede(tasks[i]);

    env.executor.async(flow).wait();
    for (int i = 0; i < N; ++i) {
        REQUIRE(wave[i].load() == i + 1);
    }
}

// ============================================================================
// SECTION 6: 二维波前 — 模板计算
// ============================================================================

/// @test [pattern][wavefront-2d] 二维模板：每个单元格依赖于上方和左侧
/// @details 经典并行模式。grid[i][j] = grid[i-1][j] + grid[i][j-1] + 1。
///          边界条件：grid[0][j] = j+1, grid[i][0] = i+1。
TEST_CASE("Pattern: 2D Stencil Wavefront", "[pattern][wavefront-2d]") {
    TestEnv env;
    tfl::Flow flow;

    constexpr int H = 8, W = 8;
    std::array<std::atomic<int>, H * W> grid;
    for (auto& c : grid) c.store(0);

    auto idx = [W](int i, int j) { return i * W + j; };

    std::vector<std::vector<tfl::Task>> tasks(H, std::vector<tfl::Task>(W));
    for (int i = 0; i < H; ++i) {
        for (int j = 0; j < W; ++j) {
            tasks[i][j] = flow.emplace([&, i, j] {
                int top = (i > 0) ? grid[idx(i - 1, j)].load() : 0;
                int left = (j > 0) ? grid[idx(i, j - 1)].load() : 0;
                grid[idx(i, j)].store(top + left + 1);
            });
        }
    }
    for (int i = 0; i < H; ++i) {
        for (int j = 0; j < W; ++j) {
            if (i > 0) tasks[i - 1][j].precede(tasks[i][j]);
            if (j > 0) tasks[i][j - 1].precede(tasks[i][j]);
        }
    }

    env.executor.async(flow).wait();

    REQUIRE(grid[idx(0, 0)].load() == 1);
    REQUIRE(grid[idx(0, W - 1)].load() == W);
    REQUIRE(grid[idx(H - 1, 0)].load() == H);
    REQUIRE(grid[idx(H - 1, W - 1)].load() > 0);
}

// ============================================================================
// SECTION 7: 分治 — 递归子图
// ============================================================================

/// @test [pattern][divide-conquer] 使用 Subflow 表达分治
/// @details 经典 fork-join："分裂 → 处理（并行 N 路）→ 合并"。
TEST_CASE("Pattern: Divide-and-conquer fork-join", "[pattern][divide-conquer]") {
    TestEnv env;
    constexpr int N = 8;
    std::vector<int> data(N);
    for (int i = 0; i < N; ++i) data[i] = i + 1;
    std::atomic<long> total{0};

    tfl::Flow inner;
    for (int i = 0; i < N; ++i) {
        inner.emplace([i, &data, &total] {
            total.fetch_add(data[i] * data[i]);
        });
    }

    tfl::Flow outer;
    auto split = outer.emplace([] {});
    auto process = outer.emplace(std::move(inner));
    auto merge = outer.emplace([] {});

    split.precede(process);
    process.precede(merge);

    env.executor.async(outer).wait();
    // 求和 1^2..8^2 = 204
    REQUIRE(total.load() == 1 + 4 + 9 + 16 + 25 + 36 + 49 + 64);
}

// ============================================================================
// SECTION 8: 异步任务依赖图
// ============================================================================

/// @test [pattern][async-dag] 使用 AsyncTask<> 构建动态依赖图
/// @details 非典型 DAG：使用 AsyncTask 而非 Flow 来构造复杂依赖关系。
TEST_CASE("Pattern: AsyncTask dependency graph", "[pattern][async-dag]") {
    TestEnv env;
    std::atomic<int> step{0};
    std::atomic<bool> ok_b{false}, ok_c{false}, ok_d{false};

    // A → {B, C} → D
    auto a = tfl::NonrepeatAsyncTask([&] { step.fetch_add(1); });
    auto b = tfl::NonrepeatAsyncTask([&] {
        ok_b.store(step.load() >= 1);
        step.fetch_add(1);
    });
    auto c = tfl::NonrepeatAsyncTask([&] {
        ok_c.store(step.load() >= 1);
        step.fetch_add(1);
    });
    auto d = tfl::NonrepeatAsyncTask([&] {
        ok_d.store(step.load() >= 3);  // a + b + c 均已运行
        step.fetch_add(1);
    });

    env.executor.submit(d, b, c);
    env.executor.submit(b, a);
    env.executor.submit(c, a);
    env.executor.submit(a);

    d.wait();
    env.executor.wait_for_all();

    REQUIRE(ok_b.load());
    REQUIRE(ok_c.load());
    REQUIRE(ok_d.load());
    REQUIRE(step.load() == 4);
}
