
#include "../taskflowlite/taskflowlite.hpp"
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
// 目的：测量调度器在完全并行负载下的吞吐量上限，
//       以及 SBO 小对象捕获（无堆分配）的基础开销。
//
// 线程数：8    任务数/次：32    重复次数：500,000
//
static void test_01() {
    constexpr int kRuns = 500'000;
    constexpr int kSize = 32;
    g_counter.store(0);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 8);
    tfl::Flow flow;

    for (int i = 0; i < kSize; ++i)
        flow.emplace([] { add_one(); });

    auto task = executor.submit(flow, kRuns);
    {
        Timer t("test_01 [32 parallel | 8 threads | 500k runs]");
        task.start().wait();
    }
    verify(kRuns * kSize);
}

// ── 02: 纯串行链 ─────────────────────────────────────
//
// 拓扑：32 个节点首尾相连，形成单条依赖链，每步必须等待上一步完成。
// 目的：测量调度器在零并发（单线程、完全串行）场景下
//       的逐节点调度开销与依赖传递延迟。
//
// 线程数：1    任务数/次：32    重复次数：1,000,000
//
static void test_02() {
    constexpr int kRuns = 1'000'000;
    constexpr int kSize = 32;
    g_counter.store(0);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 1);
    tfl::Flow flow;

    tfl::Task prev;
    for (int i = 0; i < kSize; ++i) {
        tfl::Task cur = flow.emplace([] { add_one(); });
        if (i > 0) prev.precede(cur);
        prev = cur;
    }

    auto task = executor.submit(flow, kRuns);
    {
        Timer t("test_02 [32 serial  | 1 thread  | 1M runs  ]");
        task.start().wait();
    }
    verify(kRuns * kSize);
}

// ── 03: 菱形 DAG ─────────────────────────────────────
//
// 拓扑：经典菱形，共 6 个节点：
//
//         a          ← 根，无依赖
//        / \
//       b1  c1       ← 第一分叉（可并行）
//       |   |
//       b2  c2       ← 第二层（各自依赖上一层）
//        \ /
//         d          ← 汇聚节点，等待 b2 与 c2 双双完成
//
// 目的：测量 fork/join 模式下的汇聚同步开销，
//       以及调度器在适度并行 + 依赖等待混合场景下的表现。
//
// 线程数：2    任务数/次：6    重复次数：1,000,000
//
static void test_03() {
    constexpr int kRuns  = 1'000'000;
    constexpr int kNodes = 6;
    g_counter.store(0);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 2);
    tfl::Flow flow;

    auto make = [&] { return flow.emplace([] { add_one(); }); };

    auto a  = make();
    auto b1 = make(); auto b2 = make();
    auto c1 = make(); auto c2 = make();
    auto d  = make();

    a.precede(b1, c1);
    b1.precede(b2);  c1.precede(c2);
    b2.precede(d);   c2.precede(d);

    auto task = executor.submit(flow, kRuns);
    {
        Timer t("test_03 [diamond DAG | 2 threads | 1M runs  ]");
        task.start().wait();
    }
    verify(kRuns * kNodes);
}

// ── 04: 多层全连接（参数化模板）────────────────────────
//
// 拓扑：kLayers 层 × kPerLayer 节点，层间全连接。
//       节点总数 = kLayers × kPerLayer
//       依赖边数 = (kLayers-1) × kPerLayer²
//
// 目的：通过改变层宽，观察 join-counter 原子操作、
//       work-stealing 调度以及边遍历开销随规模的增长曲线。
//
template<int kLayers, int kPerLayer, int kRuns, int kThreads>
static void run_full_connected(const char* tag) {
    constexpr int kNodes = kLayers * kPerLayer;
    g_counter.store(0);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, kThreads);
    tfl::Flow flow;

    std::vector<tfl::Task> prev, cur;
    for (int layer = 0; layer < kLayers; ++layer) {
        cur.clear();
        for (int j = 0; j < kPerLayer; ++j) {
            auto t = flow.emplace([] { add_one(); });
            for (auto& p : prev) p.precede(t);
            cur.push_back(t);
        }
        prev = cur;
    }

    auto task = executor.submit(flow, kRuns);
    { Timer tm(tag); task.start().wait(); }
    verify(kNodes * kRuns);
}

// ── 04a: 超小规模 — 4 层 × 2 节点 ───────────────────
//
// 节点：8    边：3×4=12    线程：2
// 目的：基线参照——近乎串行、几乎无竞争，
//       衡量框架在零压力下的最低调度开销。
//
static void test_04a() {
    run_full_connected<4, 2, 1'000'000, 2>(
        "test_04a [4x2  full   | 2 threads |  1M runs ]");
}

// ── 04b: 小规模 — 6 层 × 4 节点 ─────────────────────
//
// 节点：24    边：5×16=80    线程：4
// 目的：中等低密度，首次出现可观的并行竞争，
//       观察 join-counter 在少量并发下的基础开销。
//
static void test_04b() {
    run_full_connected<6, 4, 500'000, 4>(
        "test_04b [6x4  full   | 4 threads | 500k runs]");
}

// ── 04c: 中等规模 — 8 层 × 8 节点 ───────────────────
//
// 节点：64    边：7×64=448    线程：8
// 目的：标准参照点（原 test_04），平衡并行度与同步开销。
//
static void test_04c() {
    run_full_connected<8, 8, 100'000, 8>(
        "test_04c [8x8  full   | 8 threads | 100k runs]");
}

// ── 04d: 中大规模 — 8 层 × 16 节点 ──────────────────
//
// 节点：128    边：7×256=1792    线程：8
// 目的：层宽翻倍后，每个节点的扇入从 8 升至 16，
//       join-counter 竞争与边遍历开销双双加剧。
//
static void test_04d() {
    run_full_connected<8, 16, 50'000, 8>(
        "test_04d [8x16 full   | 8 threads |  50k runs]");
}

// ── 04e: 大规模 — 8 层 × 32 节点 ────────────────────
//
// 节点：256    边：7×1024=7168    线程：8
// 目的：扇入达到 32，每层节点在就绪后将向 32 个后继
//       各自发起 precede 通知，测量宽图的通知风暴。
//
static void test_04e() {
    run_full_connected<8, 32, 20'000, 8>(
        "test_04e [8x32 full   | 8 threads |  20k runs]");
}

// ── 04f: 超大规模 — 6 层 × 100 节点 ─────────────────
//
// 节点：600    边：5×10000=50000    线程：8
// 目的：层宽突破 100，单次运行产生 5 万条依赖；
//       极端化测试边存储遍历、原子计数器在海量竞争下的饱和行为，
//       以及 work-stealing 在任务暴发后的队列溢出与窃取频率。
//
static void test_04f() {
    run_full_connected<6, 100, 2'000, 8>(
        "test_04f [6x100 full  | 8 threads |   2k runs]");
}

// ── 05: 二叉归约树 ───────────────────────────────────
//
// 拓扑：深度为 5 的满二叉树，共 63 个节点。
//       32 个叶节点无前驱，可全部并行启动；
//       每对兄弟完成后其父节点方可执行，逐层向上归约至根。
//
//   叶层(32) → 第4层(16) → 第3层(8) → 第2层(4) → 第1层(2) → 根(1)
//
// 目的：测量递归 fan-in（多级汇聚）模式下
//       join-counter 的逐层原子递减开销，
//       以及并行度从 32 逐步收敛到 1 的调度行为。
//
// 线程数：8    任务数/次：63    重复次数：500,000
//
static void test_05() {
    constexpr int kDepth = 5;                      // 叶节点数 = 2^kDepth = 32
    constexpr int kNodes = (1 << (kDepth + 1)) - 1; // 63
    constexpr int kRuns  = 500'000;
    g_counter.store(0);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 8);
    tfl::Flow flow;

    // 从叶向根逐层构建：level[d] 存放第 d 层（d=0 为叶层）节点
    std::vector<tfl::Task> level;
    level.reserve(1 << kDepth);
    for (int i = 0; i < (1 << kDepth); ++i)
        level.push_back(flow.emplace([] { add_one(); }));

    for (int d = kDepth - 1; d >= 0; --d) {
        std::vector<tfl::Task> next;
        next.reserve(1 << d);
        for (int i = 0; i < (1 << d); ++i) {
            auto parent = flow.emplace([] { add_one(); });
            level[2 * i    ].precede(parent);
            level[2 * i + 1].precede(parent);
            next.push_back(parent);
        }
        level = std::move(next);
    }
    // level[0] 即根节点

    auto task = executor.submit(flow, kRuns);
    {
        Timer t("test_05 [binary tree | 8 threads | 500k runs]");
        task.start().wait();
    }
    verify(kNodes * kRuns);
}

// ── 06: 宽扇出 / 扇入 ────────────────────────────────
//
// 拓扑：单根 → 256 个完全并行的中间节点 → 单汇聚节点，共 258 个节点。
//
//   source ──→ [w0, w1, ..., w255] ──→ sink
//
// 目的：极端化地压测 join-counter：sink 节点的计数器需被
//       256 个并发线程各自减一，触发 256 次 fetch_sub CAS 竞争；
//       同时检验 work-stealing 在大宽度分发下的任务均匀性。
//
// 线程数：8    任务数/次：258    重复次数：100,000
//
static void test_06() {
    constexpr int kWidth = 256;
    constexpr int kNodes = kWidth + 2; // source + kWidth workers + sink
    constexpr int kRuns  = 100'000;
    g_counter.store(0);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 8);
    tfl::Flow flow;

    auto source = flow.emplace([] { add_one(); });
    auto sink   = flow.emplace([] { add_one(); });

    for (int i = 0; i < kWidth; ++i) {
        auto w = flow.emplace([] { add_one(); });
        source.precede(w);
        w.precede(sink);
    }

    auto task = executor.submit(flow, kRuns);
    {
        Timer t("test_06 [1->256->1   | 8 threads | 100k runs]");
        task.start().wait();
    }
    verify(kNodes * kRuns);
}

// ── 07: 独立流水线 ────────────────────────────────────
//
// 拓扑：16 条完全独立的流水线，每条 8 个阶段首尾串联，
//       共 128 个节点、112 条边。各链互不依赖，可全并行驱动。
//
//   pipe[0]:  s0 → s1 → ... → s7
//   pipe[1]:  s0 → s1 → ... → s7
//   ...
//   pipe[15]: s0 → s1 → ... → s7
//
// 目的：以完全独立的多链场景测量 work-stealing 跨链调度效率，
//       观察线程在多个独立就绪队列之间的负载均衡质量，
//       区别于单链串行（test_02）和层间全连接（test_04）。
//
// 线程数：8    任务数/次：128    重复次数：200,000
//
static void test_07() {
    constexpr int kPipelines = 16;
    constexpr int kStages    = 8;
    constexpr int kNodes     = kPipelines * kStages; // 128
    constexpr int kRuns      = 200'000;
    g_counter.store(0);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 8);
    tfl::Flow flow;

    for (int p = 0; p < kPipelines; ++p) {
        tfl::Task prev;
        for (int s = 0; s < kStages; ++s) {
            tfl::Task cur = flow.emplace([] { add_one(); });
            if (s > 0) prev.precede(cur);
            prev = cur;
        }
    }

    auto task = executor.submit(flow, kRuns);
    {
        Timer t("test_07 [16 pipes   | 8 threads | 200k runs]");
        task.start().wait();
    }
    verify(kNodes * kRuns);
}

// ── 08: 网格 DAG ─────────────────────────────────────
//
// 拓扑：N×N 二维网格，node[i][j] 同时依赖上方 node[i-1][j]
//       与左方 node[i][j-1]，从左上角向右下角传播。
//       共 256 个节点、480 条依赖边。
//
//   (0,0) → (0,1) → ... → (0,15)
//     ↓        ↓               ↓
//   (1,0) → (1,1) → ... → (1,15)
//     ...
//   (15,0)→ (15,1)→ ... → (15,15)
//
// 目的：测量"对角波前"并行模式：每条对角线上的节点
//       可以同时就绪，并行度从 1 增长到 N 再收缩回 1，
//       兼有双重 fan-in（上 + 左）的 join-counter 压力。
//
// 线程数：8    任务数/次：256    重复次数：100,000
//
static void test_08() {
    constexpr int kN    = 16;
    constexpr int kNodes = kN * kN; // 256
    constexpr int kRuns  = 100'000;
    g_counter.store(0);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 8);
    tfl::Flow flow;

    // 行优先构建；依赖建立在同一次 emplace 之后
    std::vector<std::vector<tfl::Task>> grid(kN, std::vector<tfl::Task>(kN));
    for (int i = 0; i < kN; ++i) {
        for (int j = 0; j < kN; ++j) {
            grid[i][j] = flow.emplace([] { add_one(); });
            if (i > 0) grid[i - 1][j].precede(grid[i][j]);
            if (j > 0) grid[i][j - 1].precede(grid[i][j]);
        }
    }

    auto task = executor.submit(flow, kRuns);
    {
        Timer t("test_08 [16x16 grid | 8 threads | 100k runs]");
        task.start().wait();
    }
    verify(kNodes * kRuns);
}

// ── 09: 稀疏随机 DAG ──────────────────────────────────
//
// 拓扑：64 个节点按拓扑序编号，节点 i → 节点 j（i < j）以约 8% 的
//       概率随机添加依赖边（固定种子保证可复现），约产生 160 条边。
//       整体呈不规则 DAG，无明显层次结构。
//
// 目的：模拟真实应用中依赖图不规整的情形，
//       测量调度器在不可预测并行度、非均匀 fan-in/fan-out 下的鲁棒性，
//       以及 work-stealing 对随机任务图的自适应负载均衡能力。
//
// 线程数：8    任务数/次：~64    重复次数：500,000
//
static void test_09() {
    constexpr int kN    = 64;
    constexpr int kRuns = 500'000;
    g_counter.store(0);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 8);
    tfl::Flow flow;

    std::vector<tfl::Task> nodes(kN);
    for (int i = 0; i < kN; ++i)
        nodes[i] = flow.emplace([] { add_one(); });

    // 固定种子 LCG，~8% 边密度
    uint32_t rng = 0xDEAD'BEEFu;
    int edge_count = 0;
    for (int i = 0; i < kN; ++i) {
        for (int j = i + 1; j < kN; ++j) {
            rng = rng * 1'664'525u + 1'013'904'223u;
            if ((rng >> 24) < 20u) { // 20/256 ≈ 7.8%
                nodes[i].precede(nodes[j]);
                ++edge_count;
            }
        }
    }

    auto task = executor.submit(flow, kRuns);
    {
        Timer t("test_09 [sparse DAG | 8 threads | 500k runs]");
        task.start().wait();
    }
    verify(kN * kRuns);
}

// ── 10: Jump — 重试循环（单跳回环）──────────────────
//
// 拓扑：3 个节点构成最小有向环，用 Jump 实现 while 语义：
//
//   init ──→ process ──→ retry
//                ↑           │ count < kLoopIter : jmp.select(0) → 重置并重新入队 process
//                └───────────┘ count == kLoopIter: 不调用 jmp.select()  → 拓扑自然结束
//
// Jump 的 callable 签名：[](tfl::Jump& jmp)
//   - 调用 jmp.select(N) → 将第 N 个 precede 目标（jump 边权重为 0）
//     的 join_counter 重置并重新入调度队列，形成有向环
//   - 不调用 jmp.select() → 走普通后继（若无则拓扑结束）
//
// 注：retry.precede(process) 创建 weight-0 的跳转边，
//     不会计入 process 的静态 join_counter，不引发死锁。
//
// 目的：测量循环调度的热路径极限吞吐量——
//       单次"调度 + 执行 + 跳转决策"周期的最低开销。
//
// 线程数：1    循环次数：1,000,000    外层提交：1 次
//
static void test_10() {
    constexpr int kLoopIter = 1'000'000;
    g_counter.store(0);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 1);
    tfl::Flow flow;

    // init：明确的无依赖起点，确保 process 有唯一入口
    auto init    = flow.emplace([] { /* 入口初始化，不计数 */ });
    auto process = flow.emplace([] { add_one(); });

    int count = 0;
    // Jump：通过 jmp.select(0) 将 process（target[0]）重新激活
    auto retry = flow.emplace([&count](tfl::Jump& jmp) {
        if (count < kLoopIter - 1) { ++count; jmp.select(0); }
        // 最后一次不跳转，拓扑自然结束
    });

    init.precede(process);   // 静态边：init → process（weight-1）
    process.precede(retry);  // 静态边：process → retry（weight-1）
    retry.precede(process);  // 跳转边：retry → process（weight-0，target[0]）

    auto task = executor.submit(flow, 1); // 只提交一次，循环由 jump 内部驱动
    {
        Timer t("test_10 [jump retry  | 1 thread  |  1M iter ]");
        task.start().wait();
    }
    verify(kLoopIter); // process 被执行 kLoopIter 次
}

// ── 11: MultiJump — 并行扇出循环（多路跳转）─────────
//
// 拓扑：3 条并行工作分支 + 1 个 MultiJump 汇聚控制节点，
//       MultiJump 每次同时重激活所有 3 条分支，形成并行环路：
//
//   ┌──→ branch_a ──┐
//   │               ▼
//   ├──→ branch_b ──→ mj ──→ jmp.select(0,1,2) 同时重置并入队三条分支
//   │               ▲       count >= kIter 时不跳转，拓扑结束
//   └──→ branch_c ──┘
//
// MultiJump callable 签名：[](tfl::MultiJump& jmp)
//   - 可多次调用 jmp.select(N)，每次独立重置对应 target 的 join_counter
//   - 相当于在同一控制节点内并行发出多个 Jump 信号
//
// 目的：测量多路并行跳转的扇出激活开销，
//       与 Jump（单路）对比多 target 重置与入队的额外代价；
//       同时测量 work-stealing 在并行分支周期性重启下的调度稳定性。
//
// 线程数：4    每轮工作节点：3    总迭代：200,000 轮
//
static void test_11() {
    constexpr int kIter = 200'000;
    g_counter.store(0);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    tfl::Flow flow;

    auto branch_a = flow.emplace([] { add_one(); });
    auto branch_b = flow.emplace([] { add_one(); });
    auto branch_c = flow.emplace([] { add_one(); });

    int count = 0;
    auto mj = flow.emplace([&count](tfl::MultiJump& jmp) {
        if (count < kIter - 1) {
            ++count;
            jmp.select(0); // → branch_a
            jmp.select(1); // → branch_b
            jmp.select(2); // → branch_c
        }
    });

    // 普通汇聚边（weight-1）：mj 等待三条分支全部完成
    branch_a.precede(mj);
    branch_b.precede(mj);
    branch_c.precede(mj);

    // 跳转边（weight-0）：mj → branch_X target[0,1,2]
    mj.precede(branch_a, branch_b, branch_c);

    // ── 修复：加 init 节点 ────────────────────────────────────────────────
    // 问题根因：branch_a/b/c 的 predecessor 列表中有 mj（jump 边 weight=0
    //           但物理存在），导致 _set_up_graph 中 _num_predecessors()≠0，
    //           三个分支全部被排除在初始调度之外，图启动后 n=0 无任务运行。
    //
    // 解法：引入 init 作为唯一真正无前驱的节点（_num_predecessors()==0），
    //       通过普通 weight-1 边为 branch_a/b/c 各贡献 join_count=1：
    //
    //   _set_up_graph 时：
    //     init : num_predecessors=0 → 进入初始调度集合
    //     branch_X: join_counter=1（来自 init），等待 init 完成后触发
    //
    //   第 1 轮：init 执行 → 三条分支 join_counter 1→0 → 全部就绪
    //   第 2+ 轮：mj.jmp.select(N) 直接 store(0) 重置计数器 → 分支重新就绪
    //             init 不再参与（已执行完，join_counter 恢复为 0 但无人触发）
    auto init = flow.emplace([] {});          // 无前驱，唯一合法起点
    init.precede(branch_a, branch_b, branch_c); // weight-1，为三条分支各贡献 join_count=1

    auto task = executor.submit(flow, 1);
    {
        Timer t("test_11 [multi-jump  | 4 threads | 200k iter]");
        task.start().wait();
    }
    verify(kIter * 3);
}
// ── 12: Subflow — 静态子图单次嵌入 ───────────────────
//
// 拓扑：主图 pre → subflow_node → post，共 3 个主图节点；
//       subflow_node 内嵌一张 8 节点并行子图（独立 DAG）；
//       谓词 `return true` 表示执行一次后立即停止（不循环）。
//
//   pre ──→ [sf: t0 t1 t2 t3 t4 t5 t6 t7] ──→ post
//              （8 节点无边，全部并行）
//
// Subflow API：flow.emplace(std::move(inner_flow), predicate)
//   - inner_flow 整张图作为一个调度单元嵌入主图
//   - predicate 在每次子图完成后调用：
//       返回 true  → 子图执行结束，驱动后继节点
//       返回 false → 子图再执行一轮（循环）
//
// 目的：测量静态子图的提交、执行与生命周期销毁开销；
//       外层 submit(flow, kRuns) 重复整张主图，
//       便于与等价的非嵌套拓扑（test_01）对比额外开销。
//
// 线程数：4    子图节点数：8    主图节点数：10    重复次数：200,000
//

static void test_12() {
    constexpr int kInner = 8;
    constexpr int kRuns  = 200'000;
    g_counter.store(0);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    tfl::Flow flow;

    tfl::Flow inner;
    for (int i = 0; i < kInner; ++i)
        inner.emplace([] { add_one(); });

    auto pre  = flow.emplace([] { add_one(); });

    // toggle：第1次调用返回 false（运行子图），第2次返回 true（停止）
    // 外层每轮重置：kRuns 次迭代均正确执行一次子图
    bool once = false;
    auto sf_node = flow.emplace(std::move(inner),
                                [&once]() mutable noexcept {
                                    return std::exchange(once, !once);
                                });

    auto post = flow.emplace([] { add_one(); });

    pre.precede(sf_node);
    sf_node.precede(post);

    auto task = executor.submit(flow, kRuns);
    {
        Timer t("test_12 [subflow x1  | 4 threads | 200k runs]");
        task.start().wait();
    }
    verify((kInner + 2) * kRuns);
}

// ── 13: Subflow — 谓词驱动循环子图 ───────────────────
//
// 拓扑：单个 subflow_node，内嵌一张 4 节点菱形子图，
//       通过谓词驱动循环执行 kInnerRuns 次：
//
//   subflow_node（kInnerRuns 轮循环）
//       每轮展开：sf_a → [sf_b, sf_c] → sf_d
//
// 执行流程：
//   run_inner() → call predicate()
//     predicate 返回 false → 再次 run_inner() → ... → predicate 返回 true → 结束
//
// 目的：测量谓词驱动的子图重复执行开销——
//       每轮包括子图 Topology 重置、节点 join_counter 恢复
//       以及谓词调用的控制流代价；
//       与外层 submit 重复（test_12）对比，区分内外层重复机制的性能差异。
//
// 线程数：2    子图节点数/轮：4    循环次数：500,000    外层提交：1 次
//
static void test_13() {
    constexpr int kInnerRuns = 500'000;
    g_counter.store(0);

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 2);
    tfl::Flow flow;

    // 内嵌子图：4 节点菱形
    tfl::Flow inner;
    auto sf_a = inner.emplace([] { add_one(); });
    auto sf_b = inner.emplace([] { add_one(); });
    auto sf_c = inner.emplace([] { add_one(); });
    auto sf_d = inner.emplace([] { add_one(); });
    sf_a.precede(sf_b, sf_c);
    sf_b.precede(sf_d);
    sf_c.precede(sf_d);

    // 谓词：每次子图完成后调用；返回 true 则停止循环
    int loops = 0;
    flow.emplace(std::move(inner), [&loops]() mutable noexcept {
        // loops=500000 时返回 true（停止），共运行 500000 次
        return ++loops > kInnerRuns;
    });

    auto task = executor.submit(flow, 1); // 只提交一次，循环由谓词驱动
    {
        Timer t("test_13 [subflow loop| 2 threads | 500k iter]");
        task.start().wait();
    }
    verify(4 * kInnerRuns); // 每轮 4 个节点
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

    // 10 Jump 重试循环   — init→process→retry 环，测最简热路径调度吞吐
    test_10();

    // 11 MultiJump 并行扇出 — 3 分支并行循环，测多 target 同时重激活
    test_11();

    // // 12 Subflow 单次    — pre→sf_node→post，静态子图嵌入，测提交/销毁开销
    test_12();

    // // 13 Subflow 谓词循环 — 菱形子图循环 500k 次，测谓词驱动 vs 外层 submit 差异
    test_13();
}
