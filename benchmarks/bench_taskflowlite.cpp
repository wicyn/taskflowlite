/// @file bench_taskflowlite.cpp
/// @brief TaskflowLite 基准测试 —— 使用 NonrepeatAsyncTask + submit 模式

#include "../taskflowlite/taskflowlite.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

static std::atomic<int> g_counter{0};
static void add_one() { /*g_counter.fetch_add(1, std::memory_order_relaxed);*/ }

class Timer {
public:
    explicit Timer(std::string name): m_name(std::move(name)), m_start(std::chrono::steady_clock::now()) {}
    ~Timer() {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_start).count();
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

// ============================================================================
// 01: 纯并行
// ============================================================================
static void test_01() {
    constexpr int kRuns = 500'000, kSize = 32;
    g_counter.store(0);
    tfl::Executor executor(8);
    tfl::Flow flow;
    for (int i = 0; i < kSize; ++i) flow.emplace([] { add_one(); });
    auto task = tfl::NonrepeatAsyncTask(flow, kRuns);
    { Timer t("test_01 [32 parallel | 8 threads | 500k runs]"); executor.submit(task).wait(); }
    verify(kRuns * kSize);
}

// ============================================================================
// 02: 纯串行链
// ============================================================================
static void test_02() {
    constexpr int kRuns = 1'000'000, kSize = 32;
    g_counter.store(0);
    tfl::Executor executor(1);
    tfl::Flow flow;
    tfl::Task prev;
    for (int i = 0; i < kSize; ++i) {
        tfl::Task cur = flow.emplace([] { add_one(); });
        if (i > 0) prev.precede(cur);
        prev = cur;
    }
    auto task = tfl::NonrepeatAsyncTask(flow, kRuns);
    { Timer t("test_02 [32 serial  | 1 thread  | 1M runs  ]"); executor.submit(task).wait(); }
    verify(kRuns * kSize);
}

// ============================================================================
// 03: 菱形 DAG
// ============================================================================
static void test_03() {
    constexpr int kRuns = 1'000'000, kNodes = 6;
    g_counter.store(0);
    tfl::Executor executor(2);
    tfl::Flow flow;
    auto mk = [&] { return flow.emplace([] { add_one(); }); };
    auto a = mk(), b1 = mk(), c1 = mk(), b2 = mk(), c2 = mk(), d = mk();
    a.precede(b1, c1); b1.precede(b2); c1.precede(c2); b2.precede(d); c2.precede(d);
    auto task = tfl::NonrepeatAsyncTask(flow, kRuns);
    { Timer t("test_03 [diamond DAG | 2 threads | 1M runs  ]"); executor.submit(task).wait(); }
    verify(kRuns * kNodes);
}

// ============================================================================
// 04: 多层全连接
// ============================================================================
template<int kLayers, int kPerLayer, int kRuns, int kThreads>
static void run_full_connected(const char* tag) {
    constexpr int kNodes = kLayers * kPerLayer;
    g_counter.store(0);
    tfl::Executor executor(kThreads);
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
    auto task = tfl::NonrepeatAsyncTask(flow, kRuns);
    { Timer tm(tag); executor.submit(task).wait(); }
    verify(kNodes * kRuns);
}
static void test_04a() { run_full_connected<4, 2, 1'000'000, 2>("test_04a [4x2  full   | 2 threads |  1M runs ]"); }
static void test_04b() { run_full_connected<6, 4, 500'000, 4>("test_04b [6x4  full   | 4 threads | 500k runs]"); }
static void test_04c() { run_full_connected<8, 8, 100'000, 8>("test_04c [8x8  full   | 8 threads | 100k runs]"); }
static void test_04d() { run_full_connected<8, 16, 50'000, 8>("test_04d [8x16 full   | 8 threads |  50k runs]"); }
static void test_04e() { run_full_connected<8, 32, 20'000, 8>("test_04e [8x32 full   | 8 threads |  20k runs]"); }
static void test_04f() { run_full_connected<6, 100, 2'000, 8>("test_04f [6x100 full  | 8 threads |   2k runs]"); }

// ============================================================================
// 05: 二叉归约树
// ============================================================================
static void test_05() {
    constexpr int kDepth = 5, kNodes = (1 << (kDepth + 1)) - 1, kRuns = 500'000;
    g_counter.store(0);
    tfl::Executor executor(8);
    tfl::Flow flow;
    std::vector<tfl::Task> level; level.reserve(1 << kDepth);
    for (int i = 0; i < (1 << kDepth); ++i) level.push_back(flow.emplace([] { add_one(); }));
    for (int d = kDepth - 1; d >= 0; --d) {
        std::vector<tfl::Task> next; next.reserve(1 << d);
        for (int i = 0; i < (1 << d); ++i) {
            auto parent = flow.emplace([] { add_one(); });
            level[2*i].precede(parent); level[2*i+1].precede(parent);
            next.push_back(parent);
        }
        level = std::move(next);
    }
    auto task = tfl::NonrepeatAsyncTask(flow, kRuns);
    { Timer t("test_05 [binary tree | 8 threads | 500k runs]"); executor.submit(task).wait(); }
    verify(kNodes * kRuns);
}

// ============================================================================
// 06: 宽扇出 / 扇入
// ============================================================================
static void test_06() {
    constexpr int kWidth = 256, kNodes = kWidth + 2, kRuns = 100'000;
    g_counter.store(0);
    tfl::Executor executor(8);
    tfl::Flow flow;
    auto source = flow.emplace([] { add_one(); });
    auto sink   = flow.emplace([] { add_one(); });
    for (int i = 0; i < kWidth; ++i) { auto w = flow.emplace([] { add_one(); }); source.precede(w); w.precede(sink); }
    auto task = tfl::NonrepeatAsyncTask(flow, kRuns);
    { Timer t("test_06 [1->256->1   | 8 threads | 100k runs]"); executor.submit(task).wait();}
    verify(kNodes * kRuns);
}

// ============================================================================
// 07: 独立流水线
// ============================================================================
static void test_07() {
    constexpr int kPipes = 16, kStages = 8, kNodes = kPipes * kStages, kRuns = 200'000;
    g_counter.store(0);
    tfl::Executor executor(8);
    tfl::Flow flow;
    for (int p = 0; p < kPipes; ++p) {
        tfl::Task prev;
        for (int s = 0; s < kStages; ++s) {
            tfl::Task cur = flow.emplace([] { add_one(); });
            if (s > 0) prev.precede(cur);
            prev = cur;
        }
    }
    auto task = tfl::NonrepeatAsyncTask(flow, kRuns);
    { Timer t("test_07 [16 pipes   | 8 threads | 200k runs]"); executor.submit(task).wait(); }
    verify(kNodes * kRuns);
}

// ============================================================================
// 08: 网格 DAG
// ============================================================================
static void test_08() {
    constexpr int kN = 16, kNodes = kN * kN, kRuns = 100'000;
    g_counter.store(0);
    tfl::Executor executor(8);
    tfl::Flow flow;
    std::vector<std::vector<tfl::Task>> grid(kN, std::vector<tfl::Task>(kN));
    for (int i = 0; i < kN; ++i) for (int j = 0; j < kN; ++j) {
        grid[i][j] = flow.emplace([] { add_one(); });
        if (i > 0) grid[i-1][j].precede(grid[i][j]);
        if (j > 0) grid[i][j-1].precede(grid[i][j]);
    }
    auto task = tfl::NonrepeatAsyncTask(flow, kRuns);
    { Timer t("test_08 [16x16 grid | 8 threads | 100k runs]"); executor.submit(task).wait(); }
    verify(kNodes * kRuns);
}

// ============================================================================
// 09: 稀疏随机 DAG
// ============================================================================
static void test_09() {
    constexpr int kN = 64, kRuns = 500'000;
    g_counter.store(0);
    tfl::Executor executor(8);
    tfl::Flow flow;
    std::vector<tfl::Task> nodes(kN);
    for (int i = 0; i < kN; ++i) nodes[i] = flow.emplace([] { add_one(); });
    uint32_t rng = 0xDEAD'BEEFu;
    for (int i = 0; i < kN; ++i) for (int j = i + 1; j < kN; ++j) {
        rng = rng * 1'664'525u + 1'013'904'223u;
        if ((rng >> 24) < 20u) nodes[i].precede(nodes[j]);
    }
    auto task = tfl::NonrepeatAsyncTask(flow, kRuns);
    { Timer t("test_09 [sparse DAG | 8 threads | 500k runs]"); executor.submit(task).wait(); }
    verify(kN * kRuns);
}

// ============================================================================
// 10: Jump — 重试循环
// ============================================================================
static void test_10() {
    constexpr int kLoopIter = 1'000'000;
    g_counter.store(0);
    tfl::Executor executor(1);
    tfl::Flow flow;
    auto init    = flow.emplace([] { });
    auto process = flow.emplace([] { add_one(); });
    int count = 0;
    auto retry = flow.emplace([&count](tfl::Jump& jmp) {
        if (count < kLoopIter - 1) { ++count; jmp.select(0); }
    });
    init.precede(process); process.precede(retry); retry.precede(process);
    auto task = tfl::NonrepeatAsyncTask(flow, 1ULL);
    { Timer t("test_10 [jump retry  | 1 thread  |  1M iter ]"); executor.submit(task).wait(); }
    verify(kLoopIter);
}

// ============================================================================
// 11: MultiJump — 并行扇出循环
// ============================================================================
static void test_11() {
    constexpr int kIter = 200'000;
    g_counter.store(0);
    tfl::Executor executor(4);
    tfl::Flow flow;
    auto init     = flow.emplace([] {});
    auto branch_a = flow.emplace([] { add_one(); });
    auto branch_b = flow.emplace([] { add_one(); });
    auto branch_c = flow.emplace([] { add_one(); });
    int count = 0;
    auto mj = flow.emplace([&count](tfl::MultiJump& jmp) {
        if (count < kIter - 1) { ++count; jmp.select(0, 1, 2); }
    });
    init.precede(branch_a, branch_b, branch_c);
    branch_a.precede(mj); branch_b.precede(mj); branch_c.precede(mj);
    mj.precede(branch_a, branch_b, branch_c);
    auto task = tfl::NonrepeatAsyncTask(flow, 1ULL);
    { Timer t("test_11 [multi-jump  | 4 threads | 200k iter]"); executor.submit(task).wait(); }
    verify(kIter * 3);
}

// ============================================================================
// 12: Subflow 单次
// ============================================================================
static void test_12() {
    constexpr int kInner = 8, kRuns = 200'000;
    g_counter.store(0);
    tfl::Executor executor(4);
    tfl::Flow flow; tfl::Flow inner;
    for (int i = 0; i < kInner; ++i) inner.emplace([] { add_one(); });
    auto pre = flow.emplace([] { add_one(); });
    bool once = false;
    auto sf = flow.emplace(std::move(inner), [&once]() mutable noexcept { return std::exchange(once, !once); });
    auto post = flow.emplace([] { add_one(); });
    pre.precede(sf); sf.precede(post);
    auto task = tfl::NonrepeatAsyncTask(flow, kRuns);
    { Timer t("test_12 [subflow x1  | 4 threads | 200k runs]"); executor.submit(task).wait(); }
    verify((kInner + 2) * kRuns);
}

// ============================================================================
// 13: Subflow 循环
// ============================================================================
static void test_13() {
    constexpr int kInnerRuns = 500'000;
    g_counter.store(0);
    tfl::Executor executor(2);
    tfl::Flow flow; tfl::Flow inner;
    auto sf_a = inner.emplace([] { add_one(); }); auto sf_b = inner.emplace([] { add_one(); });
    auto sf_c = inner.emplace([] { add_one(); }); auto sf_d = inner.emplace([] { add_one(); });
    sf_a.precede(sf_b, sf_c); sf_b.precede(sf_d); sf_c.precede(sf_d);
    int loops = 0;
    flow.emplace(std::move(inner), [&loops]() mutable noexcept { return ++loops > kInnerRuns; });
    auto task = tfl::NonrepeatAsyncTask(flow, 1ULL);
    { Timer t("test_13 [subflow loop| 2 threads | 500k iter]"); executor.submit(task).wait(); }
    verify(4 * kInnerRuns);
}

// ============================================================================
// 14: 空任务延迟
// ============================================================================
static void test_14() {
    constexpr int kRuns = 10'000'000;
    g_counter.store(0);
    tfl::Executor executor(1);
    tfl::Flow flow;
    flow.emplace([] { add_one(); });
    auto task = tfl::NonrepeatAsyncTask(flow, kRuns);
    { Timer t("test_14 [empty task  | 1 thread  | 10M runs ]"); executor.submit(task).wait(); }
    verify(kRuns);
}

// ============================================================================
// 15: 并行 for
// ============================================================================
static void test_15() {
    constexpr int kRuns = 10'000; int kSize = 1024;
    g_counter.store(0);
    tfl::Executor executor(8);
    tfl::Flow flow;
    for (int i = 0; i < kSize; ++i) flow.emplace([] { add_one(); });
    auto task = tfl::NonrepeatAsyncTask(flow, kRuns);
    { Timer t("test_15 [parallel for| 8 threads | 1024 tasks x 10k runs]"); executor.submit(task).wait(); }
    verify(kRuns * kSize);
}

// ============================================================================
// 16: 归约树（带计算）
// ============================================================================
static void test_16() {
    constexpr int kDepth = 6, kNodes = (1 << (kDepth + 1)) - 1, kRuns = 50'000;
    g_counter.store(0);
    tfl::Executor executor(8);
    tfl::Flow flow;
    auto work = [] { std::atomic<int> x{0}; for (int i = 0; i < 10; ++i) x.fetch_add(i, std::memory_order_relaxed); add_one(); };
    int leaves = 1 << kDepth;
    std::vector<tfl::Task> level; level.reserve(leaves);
    for (int i = 0; i < leaves; ++i) level.push_back(flow.emplace(work));
    for (int d = kDepth - 1; d >= 0; --d) {
        std::vector<tfl::Task> next; next.reserve(1 << d);
        for (int i = 0; i < (1 << d); ++i) {
            auto p = flow.emplace(work); level[2*i].precede(p); level[2*i+1].precede(p); next.push_back(p);
        }
        level = std::move(next);
    }
    auto task = tfl::NonrepeatAsyncTask(flow, kRuns);
    { Timer t("test_16 [reduce tree | 8 threads | 127 nodes x 50k runs]"); executor.submit(task).wait(); }
    verify(kNodes * kRuns);
}

// ============================================================================
// 17: 前缀和扫描链
// ============================================================================
static void test_17() {
    constexpr int kRuns = 100'000; int kSize = 128;
    g_counter.store(0);
    tfl::Executor executor(1);
    tfl::Flow flow;
    tfl::Task prev;
    for (int i = 0; i < kSize; ++i) {
        tfl::Task cur = flow.emplace([] { add_one(); });
        if (i > 0) prev.precede(cur);
        prev = cur;
    }
    auto task = tfl::NonrepeatAsyncTask(flow, kRuns);
    { Timer t("test_17 [scan chain  | 1 thread  | 128 x 100k runs]"); executor.submit(task).wait(); }
    verify(kRuns * kSize);
}

// ============================================================================
// 18: 三角波前
// ============================================================================
static void test_18() {
    constexpr int kN = 20, kRuns = 10'000;
    int kNodes = kN * (kN + 1) / 2;
    g_counter.store(0);
    tfl::Executor executor(8);
    tfl::Flow flow;
    std::vector<std::vector<tfl::Task>> grid(kN);
    for (int i = 0; i < kN; ++i) {
        grid[i].reserve(i + 1);
        for (int j = 0; j <= i; ++j) {
            grid[i].push_back(flow.emplace([] { add_one(); }));
            if (i > 0 && j <= i - 1) grid[i-1][j].precede(grid[i][j]);
            if (j > 0) grid[i][j-1].precede(grid[i][j]);
        }
    }
    auto task = tfl::NonrepeatAsyncTask(flow, kRuns);
    { Timer t("test_18 [wavefront   | 8 threads | 210 nodes x 10k runs]"); executor.submit(task).wait(); }
    verify(kNodes * kRuns);
}

// ============================================================================
// 19: 异构工作负载
// ============================================================================
static void test_19() {
    constexpr int kRuns = 100'000, kLight = 8, kHeavy = 8;
    int kNodes = kLight + kHeavy + 2;
    g_counter.store(0);
    tfl::Executor executor(8);
    tfl::Flow flow;
    auto light = [] { add_one(); };
    auto heavy = [] { std::atomic<int> x{0}; for (int i = 0; i < 500; ++i) x.fetch_add(i, std::memory_order_relaxed); add_one(); };
    auto src = flow.emplace(light); auto sink = flow.emplace(light);
    for (int i = 0; i < kLight; ++i) { auto t = flow.emplace(light); src.precede(t); t.precede(sink); }
    for (int i = 0; i < kHeavy; ++i) { auto t = flow.emplace(heavy); src.precede(t); t.precede(sink); }
    auto task = tfl::NonrepeatAsyncTask(flow, kRuns);
    { Timer t("test_19 [hetero      | 8 threads | 18 nodes x 100k runs]"); executor.submit(task).wait(); }
    verify(kNodes * kRuns);
}

// ============================================================================
// 20: 内存压力
// ============================================================================
static void test_20() {
    constexpr int kLayers = 10, kPerLayer = 200, kRuns = 500;
    int kNodes = kLayers * kPerLayer;
    g_counter.store(0);
    tfl::Executor executor(8);
    tfl::Flow flow;
    std::vector<tfl::Task> prev, cur;
    for (int l = 0; l < kLayers; ++l) {
        cur.clear();
        for (int j = 0; j < kPerLayer; ++j) {
            auto t = flow.emplace([] { add_one(); });
            for (auto& p : prev) p.precede(t);
            cur.push_back(t);
        }
        prev = cur;
    }
    auto task = tfl::NonrepeatAsyncTask(flow, kRuns);
    { Timer t("test_20 [mem stress  | 8 threads | 2000 nodes x 500 runs]"); executor.submit(task).wait(); }
    verify(kNodes * kRuns);
}

// ============================================================================
// main
// ============================================================================
int main() {
    test_01(); test_02(); test_03();
    test_04a(); test_04b(); test_04c(); test_04d(); test_04e(); test_04f();
    test_05(); test_06(); test_07(); test_08(); test_09();
    test_10(); test_11(); test_12(); test_13();
    test_14(); test_15(); test_16(); test_17(); test_18(); test_19(); test_20();
    return 0;
}
