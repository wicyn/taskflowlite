
#include "taskflow/taskflow.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

// ── 全局计数器 ────────────────────────────────────────
static std::atomic<int> g_counter{0};

static void add_one() {
    g_counter.fetch_add(1, std::memory_order_relaxed);
}

// ── 计时 RAII ─────────────────────────────────────────
class Timer {
public:
    explicit Timer(std::string name)
        : m_name(std::move(name))
        , m_start(std::chrono::steady_clock::now()) {}

    ~Timer() {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - m_start)
                      .count();
        std::cout << "[" << m_name << "] elapsed: " << ms << " ms\n";
    }

private:
    std::string m_name;
    std::chrono::steady_clock::time_point m_start;
};

static void verify(int expected) {
    int actual = g_counter.load();
    std::cout << "  verify: expected=" << expected << ", actual=" << actual
              << (expected == actual ? "  [PASS]" : "  [FAIL]") << "\n\n";
}

// ── 01: 纯并行 ───────────────────────────────────────
//
// 拓扑：32 个独立节点，彼此之间无任何依赖边，可全部同时调度。
//
// 线程数：8    任务数/次：32    重复次数：500,000
//
static void test_01() {
    constexpr int kRuns = 500'000;
    constexpr int kSize = 32;
    g_counter.store(0);

    tf::Executor executor(8);
    tf::Taskflow flow;

    for (int i = 0; i < kSize; ++i)
        flow.emplace([] { add_one(); });

    {
        Timer t("test_01 [32 parallel | 8 threads | 500k runs]");
        executor.run_n(flow, kRuns).wait();
    }
    verify(kRuns * kSize);
}

// ── 02: 纯串行链 ─────────────────────────────────────
//
// 拓扑：32 个节点首尾相连，形成单条依赖链。
//
// 线程数：1    任务数/次：32    重复次数：1,000,000
//
static void test_02() {
    constexpr int kRuns = 1'000'000;
    constexpr int kSize = 32;
    g_counter.store(0);

    tf::Executor executor(1);
    tf::Taskflow flow;

    tf::Task prev;
    for (int i = 0; i < kSize; ++i) {
        tf::Task cur = flow.emplace([] { add_one(); });
        if (i > 0) prev.precede(cur);
        prev = cur;
    }

    {
        Timer t("test_02 [32 serial  | 1 thread  | 1M runs  ]");
        executor.run_n(flow, kRuns).wait();
    }
    verify(kRuns * kSize);
}

// ── 03: 菱形 DAG ─────────────────────────────────────
//
// 拓扑：经典菱形，共 6 个节点：
//
//         a
//        / \
//       b1  c1
//       |   |
//       b2  c2
//        \ /
//         d
//
// 线程数：2    任务数/次：6    重复次数：1,000,000
//
static void test_03() {
    constexpr int kRuns  = 1'000'000;
    constexpr int kNodes = 6;
    g_counter.store(0);

    tf::Executor executor(2);
    tf::Taskflow flow;

    auto make = [&] { return flow.emplace([] { add_one(); }); };

    auto a  = make();
    auto b1 = make(); auto b2 = make();
    auto c1 = make(); auto c2 = make();
    auto d  = make();

    a.precede(b1, c1);
    b1.precede(b2); c1.precede(c2);
    b2.precede(d);  c2.precede(d);

    {
        Timer t("test_03 [diamond DAG | 2 threads | 1M runs  ]");
        executor.run_n(flow, kRuns).wait();
    }
    verify(kRuns * kNodes);
}

// ── 04: 多层全连接（参数化模板）────────────────────────
//
// 拓扑：kLayers 层 × kPerLayer 节点，层间全连接。
//
template<int kLayers, int kPerLayer, int kRuns, int kThreads>
static void run_full_connected(const char* tag) {
    constexpr int kNodes = kLayers * kPerLayer;
    g_counter.store(0);

    tf::Executor executor(kThreads);
    tf::Taskflow flow;

    std::vector<tf::Task> prev, cur;
    for (int layer = 0; layer < kLayers; ++layer) {
        cur.clear();
        for (int j = 0; j < kPerLayer; ++j) {
            auto t = flow.emplace([] { add_one(); });
            for (auto& p : prev) p.precede(t);
            cur.push_back(t);
        }
        prev = cur;
    }

    {
        Timer tm(tag);
        executor.run_n(flow, kRuns).wait();
    }
    verify(kNodes * kRuns);
}

// ── 04a: 超小规模 — 4 层 × 2 节点 ───────────────────
static void test_04a() {
    run_full_connected<4, 2, 1'000'000, 2>(
        "test_04a [4x2  full   | 2 threads |  1M runs ]");
}

// ── 04b: 小规模 — 6 层 × 4 节点 ─────────────────────
static void test_04b() {
    run_full_connected<6, 4, 500'000, 4>(
        "test_04b [6x4  full   | 4 threads | 500k runs]");
}

// ── 04c: 中等规模 — 8 层 × 8 节点 ───────────────────
static void test_04c() {
    run_full_connected<8, 8, 100'000, 8>(
        "test_04c [8x8  full   | 8 threads | 100k runs]");
}

// ── 04d: 中大规模 — 8 层 × 16 节点 ──────────────────
static void test_04d() {
    run_full_connected<8, 16, 50'000, 8>(
        "test_04d [8x16 full   | 8 threads |  50k runs]");
}

// ── 04e: 大规模 — 8 层 × 32 节点 ────────────────────
static void test_04e() {
    run_full_connected<8, 32, 20'000, 8>(
        "test_04e [8x32 full   | 8 threads |  20k runs]");
}

// ── 04f: 超大规模 — 6 层 × 100 节点 ─────────────────
static void test_04f() {
    run_full_connected<6, 100, 2'000, 8>(
        "test_04f [6x100 full  | 8 threads |   2k runs]");
}

// ── 05: 二叉归约树 ───────────────────────────────────
//
// 拓扑：深度为 5 的满二叉树，共 63 个节点。
//
// 线程数：8    任务数/次：63    重复次数：500,000
//
static void test_05() {
    constexpr int kDepth = 5;
    constexpr int kNodes = (1 << (kDepth + 1)) - 1; // 63
    constexpr int kRuns  = 500'000;
    g_counter.store(0);

    tf::Executor executor(8);
    tf::Taskflow flow;

    std::vector<tf::Task> level;
    level.reserve(1 << kDepth);
    for (int i = 0; i < (1 << kDepth); ++i)
        level.push_back(flow.emplace([] { add_one(); }));

    for (int d = kDepth - 1; d >= 0; --d) {
        std::vector<tf::Task> next;
        next.reserve(1 << d);
        for (int i = 0; i < (1 << d); ++i) {
            auto parent = flow.emplace([] { add_one(); });
            level[2 * i    ].precede(parent);
            level[2 * i + 1].precede(parent);
            next.push_back(parent);
        }
        level = std::move(next);
    }

    {
        Timer t("test_05 [binary tree | 8 threads | 500k runs]");
        executor.run_n(flow, kRuns).wait();
    }
    verify(kNodes * kRuns);
}

// ── 06: 宽扇出 / 扇入 ────────────────────────────────
//
// 拓扑：source → 256 个并行节点 → sink，共 258 个节点。
//
// 线程数：8    任务数/次：258    重复次数：100,000
//
static void test_06() {
    constexpr int kWidth = 256;
    constexpr int kNodes = kWidth + 2;
    constexpr int kRuns  = 100'000;
    g_counter.store(0);

    tf::Executor executor(8);
    tf::Taskflow flow;

    auto source = flow.emplace([] { add_one(); });
    auto sink   = flow.emplace([] { add_one(); });

    for (int i = 0; i < kWidth; ++i) {
        auto w = flow.emplace([] { add_one(); });
        source.precede(w);
        w.precede(sink);
    }

    {
        Timer t("test_06 [1→256→1   | 8 threads | 100k runs]");
        executor.run_n(flow, kRuns).wait();
    }
    verify(kNodes * kRuns);
}

// ── 07: 独立流水线 ────────────────────────────────────
//
// 拓扑：16 条完全独立的流水线，每条 8 个阶段。
//
// 线程数：8    任务数/次：128    重复次数：200,000
//
static void test_07() {
    constexpr int kPipelines = 16;
    constexpr int kStages    = 8;
    constexpr int kNodes     = kPipelines * kStages;
    constexpr int kRuns      = 200'000;
    g_counter.store(0);

    tf::Executor executor(8);
    tf::Taskflow flow;

    for (int p = 0; p < kPipelines; ++p) {
        tf::Task prev;
        for (int s = 0; s < kStages; ++s) {
            tf::Task cur = flow.emplace([] { add_one(); });
            if (s > 0) prev.precede(cur);
            prev = cur;
        }
    }

    {
        Timer t("test_07 [16 pipes   | 8 threads | 200k runs]");
        executor.run_n(flow, kRuns).wait();
    }
    verify(kNodes * kRuns);
}

// ── 08: 网格 DAG ─────────────────────────────────────
//
// 拓扑：16×16 二维网格，对角波前传播，共 256 个节点。
//
// 线程数：8    任务数/次：256    重复次数：100,000
//
static void test_08() {
    constexpr int kN     = 16;
    constexpr int kNodes = kN * kN;
    constexpr int kRuns  = 100'000;
    g_counter.store(0);

    tf::Executor executor(8);
    tf::Taskflow flow;

    std::vector<std::vector<tf::Task>> grid(kN, std::vector<tf::Task>(kN));
    for (int i = 0; i < kN; ++i) {
        for (int j = 0; j < kN; ++j) {
            grid[i][j] = flow.emplace([] { add_one(); });
            if (i > 0) grid[i - 1][j].precede(grid[i][j]);
            if (j > 0) grid[i][j - 1].precede(grid[i][j]);
        }
    }

    {
        Timer t("test_08 [16x16 grid | 8 threads | 100k runs]");
        executor.run_n(flow, kRuns).wait();
    }
    verify(kNodes * kRuns);
}

// ── 09: 稀疏随机 DAG ──────────────────────────────────
//
// 拓扑：64 个节点，固定种子 ~8% 边密度随机 DAG。
//
// 线程数：8    任务数/次：64    重复次数：500,000
//
static void test_09() {
    constexpr int kN    = 64;
    constexpr int kRuns = 500'000;
    g_counter.store(0);

    tf::Executor executor(8);
    tf::Taskflow flow;

    std::vector<tf::Task> nodes(kN);
    for (int i = 0; i < kN; ++i)
        nodes[i] = flow.emplace([] { add_one(); });

    uint32_t rng = 0xDEAD'BEEFu;
    for (int i = 0; i < kN; ++i) {
        for (int j = i + 1; j < kN; ++j) {
            rng = rng * 1'664'525u + 1'013'904'223u;
            if ((rng >> 24) < 20u)
                nodes[i].precede(nodes[j]);
        }
    }

    {
        Timer t("test_09 [sparse DAG | 8 threads | 500k runs]");
        executor.run_n(flow, kRuns).wait();
    }
    verify(kN * kRuns);
}

// ── 10: 条件任务循环（对应 tfl::Jump）─────────────────
//
// Taskflow 用 tf::condition_task 实现等价的 while 循环：
//
//   init → process → retry(condition)
//                         │ count < kLoopIter : 返回 0 → 跳回 process
//                         └ count == kLoopIter: 返回 1 → 拓扑结束
//
// condition_task 返回整数，对应 precede 的第 N 个后继（0-based）。
// 注：Taskflow 条件任务会绕过后继的 join_counter 直接调度，
//     与 tfl::Jump 的 jmp.select(N) 语义完全对应。
//
// 线程数：1    循环次数：1,000,000    外层提交：1 次
//
static void test_10() {
    constexpr int kLoopIter = 1'000'000;
    g_counter.store(0);

    tf::Executor executor(1);
    tf::Taskflow flow;

    auto init    = flow.emplace([] {});
    auto process = flow.emplace([] { add_one(); });

    int count = 0;
    // condition_task：返回 0 → 跳回 process（target[0]）
    //                 返回 1 → 走 end（target[1]，拓扑结束）
    auto retry = flow.emplace([&count]() -> int {
        if (count < kLoopIter - 1) { ++count; return 0; }
        return 1;
    });

    // 无后继的 end 占位节点，用于接收 retry 返回 1 时的"退出"分支
    auto end = flow.emplace([] {});

    init.precede(process);
    process.precede(retry);
    retry.precede(process, end); // 0 → process, 1 → end

    {
        Timer t("test_10 [cond loop  | 1 thread  |  1M iter ]");
        executor.run(flow).wait();
    }
    verify(kLoopIter);
}

// ── 11: multi_condition_task — 并行扇出循环（对应 tfl::MultiJump）──
//
// Taskflow 的 multi_condition_task 返回 tf::SmallVector<int>，
// 可同时激活多个条件后继，与 tfl::MultiJump 语义完全等价。
//
// 拓扑：
//   init ──→ branch_a ──┐
//        ──→ branch_b ──┼──→ mj (multi_condition_task)
//        ──→ branch_c ──┘        ├── {0,1,2}: 同时激活 branch_a/b/c（循环）
//                                └── {3}    : 激活 end（终止）
//
// 为什么需要 init 节点：
//   `mj.precede(branch_a, branch_b, branch_c, end)` 会在 branch_a/b/c 的
//   前驱列表中注册 mj（条件边），导致 Taskflow 初始化拓扑时
//   `num_dependents() > 0`，三条分支不会进入初始就绪队列。
//   init 通过普通 weight-1 边为 branch_a/b/c 各贡献 1 个强依赖：
//     - 图启动时 init（num_dependents=0）立即调度
//     - init 完成后 branch_a/b/c strong_dependents 1→0 → 全部并行触发
//   第 2+ 轮：mj 条件边直接入队 branch_a/b/c（不经过 join_counter），
//            init 已执行完，不再参与后续循环。
//
//   mj 完成后：
//     1. mj 的 join_counter 恢复为 3（Taskflow 循环图自动重置）
//     2. 条件边直接将 branch_a/b/c 压入就绪队列（不修改其 join_counter）
//     3. 三条分支完成后再次 decrement mj：3→2→1→0 → mj 再次运行
//
// 线程数：4    每轮工作节点：3    总迭代：200,000 轮
//
static void test_11() {
    constexpr int kIter = 200'000;
    g_counter.store(0);

    tf::Executor executor(4);
    tf::Taskflow flow;

    // init：无前驱，图启动时唯一立即就绪节点
    auto init     = flow.emplace([] {});
    auto branch_a = flow.emplace([] { add_one(); });
    auto branch_b = flow.emplace([] { add_one(); });
    auto branch_c = flow.emplace([] { add_one(); });

    int count = 0;
    // multi_condition_task：返回 SmallVector<int> 同时激活多个条件后继
    //   返回 {0,1,2} → 同时重激活 branch_a/b/c（并行扇出）
    //   返回 {3}     → 激活 end，拓扑结束
    auto mj  = flow.emplace([&count]() -> tf::SmallVector<int> {
        if (count++ < kIter - 1) return {0, 1, 2};
        return {3};
    });
    auto end = flow.emplace([] {});

    // 普通边：init 第一轮触发三条分支（weight-1，计入 strong_dependents）
    init.precede(branch_a, branch_b, branch_c);

    // 普通汇聚边（weight-1）：三条分支完成后驱动 mj
    branch_a.precede(mj);
    branch_b.precede(mj);
    branch_c.precede(mj);

    // 条件边（不计入 strong_dependents，不经过 join_counter）：
    //   0 → branch_a, 1 → branch_b, 2 → branch_c, 3 → end
    mj.precede(branch_a, branch_b, branch_c, end);

    {
        Timer t("test_11 [multi-cond  | 4 threads | 200k iter]");
        executor.run(flow).wait();
    }
    verify(kIter * 3);
}


// ── 12: Subflow — 静态子图单次嵌入 ───────────────────
//
// Taskflow 的 subflow 节点通过 (tf::Subflow& sf) 签名在运行时动态构建子图。
// 与 tfl 的 flow.emplace(std::move(inner), pred) 语义等价：
// 每次外层 run 时子图重新执行一次。
//
// 线程数：4    子图节点数：8    主图节点数：10    重复次数：200,000
//
static void test_12() {
    constexpr int kInner = 8;
    constexpr int kRuns  = 200'000;
    g_counter.store(0);

    tf::Executor executor(4);

    // 预构建内嵌子图：提取到外部，避免每次 subflow 调度时重复 emplace
    // 与 tfl flow.emplace(std::move(inner), pred) 的静态嵌入语义一致
    tf::Taskflow inner;
    for (int i = 0; i < kInner; ++i)
        inner.emplace([] { add_one(); });

    tf::Taskflow flow;
    auto pre     = flow.emplace([] { add_one(); });
    auto sf_node = flow.emplace([&inner](tf::Subflow& sf) {
        sf.executor().corun(inner); // 协作式执行预构建子图，Worker 不挂起
    });
    auto post    = flow.emplace([] { add_one(); });

    pre.precede(sf_node);
    sf_node.precede(post);

    {
        Timer t("test_12 [subflow x1  | 4 threads | 200k runs]");
        executor.run_n(flow, kRuns).wait();
    }
    verify((kInner + 2) * kRuns);
}

// ── 13: Subflow — 内部循环子图 ───────────────────────
//
// Taskflow 的 subflow 在运行时构建，用内部循环实现 tfl 谓词驱动的等价效果：
// subflow 节点每次被调用时，通过 corun 反复执行菱形子图 kInnerRuns 次。
//
// 注：tfl 版本依赖调度器外部谓词循环；Taskflow 版本在 subflow 内部
//     用 corun 实现等价的协作式循环，保持 Worker 线程不阻塞。
//
// 线程数：2    子图节点数/轮：4    循环次数：500,000    外层提交：1 次
//
static void test_13() {
    constexpr int kInnerRuns = 500'000;
    g_counter.store(0);

    tf::Executor executor(2);

    // 预构建菱形子图：提取到外部，每轮 corun 复用同一张图，避免反复构建开销
    tf::Taskflow diamond;
    auto sf_a = diamond.emplace([] { add_one(); });
    auto sf_b = diamond.emplace([] { add_one(); });
    auto sf_c = diamond.emplace([] { add_one(); });
    auto sf_d = diamond.emplace([] { add_one(); });
    sf_a.precede(sf_b, sf_c);
    sf_b.precede(sf_d);
    sf_c.precede(sf_d);

    tf::Taskflow flow;
    flow.emplace([&diamond](tf::Subflow& sf) {
        for (int i = 0; i < kInnerRuns; ++i)
            sf.executor().corun(diamond); // 协作式执行，Worker 线程不挂起
    });

    {
        Timer t("test_13 [subflow loop| 2 threads | 500k iter]");
        executor.run(flow).wait();
    }
    verify(4 * kInnerRuns);
}

// ── main ──────────────────────────────────────────────
int main() {
    // 01 纯并行        — 32 独立节点，测吞吐量上限
    test_01();

    // 02 纯串行链      — 32 节点单链，测逐节点调度延迟
    test_02();

    // 03 菱形 DAG      — fork/join，测汇聚同步开销
    test_03();

    // 04 多层全连接系列 — 从超小到超大，观察规模增长曲线
    test_04a(); // 4×2    节点:  8   边:   12   基线/零压力
    test_04b(); // 6×4    节点: 24   边:   80   低竞争
    test_04c(); // 8×8    节点: 64   边:  448   标准参照
    test_04d(); // 8×16   节点:128   边: 1792   中高密度
    test_04e(); // 8×32   节点:256   边: 7168   高密度通知风暴
    test_04f(); // 6×100  节点:600   边:50000   超大饱和压力

    // 05 二叉归约树    — 32 叶→根，测多级递归 join-counter
    test_05();

    // 06 宽扇出/扇入   — 1→256→1，测极端 join-counter 竞争
    test_06();

    // 07 独立流水线    — 16 链×8 阶段，测跨链 work-stealing 均衡
    test_07();

    // 08 网格 DAG      — 16×16，测对角波前 + 双重 fan-in
    test_08();

    // 09 稀疏随机 DAG  — 不规则依赖，测调度鲁棒性
    test_09();

    // 10 条件循环      — condition_task 实现 Jump 等价，测热路径调度吞吐
    test_10();

    // 11 条件并行循环  — condition + 普通并行边模拟 MultiJump
    test_11();

    // 12 Subflow 单次  — 动态子图嵌入，测运行时构建 + 销毁开销
    test_12();

    // 13 Subflow 内部循环 — corun 驱动菱形子图 500k 次
    test_13();
}
