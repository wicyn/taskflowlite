/// @file 17_parallel_reduce.cpp
/// @brief 演示静态分区的并行归约（与例 09 动态扇出形成对比）。
///
/// 设计对比：
///   - 09_pipeline.cpp: 用 Runtime + silent_async 在运行期动态扇出 chunk；
///   - 本例:           在 Flow 构建期就把数据切分成 N 个并行分区，每个分区
///                     是一个静态节点，最后由 reducer 节点汇聚。
///
/// 静态分区的优势：
///   1. 拓扑可视化（D2 dump）一目了然，便于调试；
///   2. 无运行期任务派发开销，N 固定时性能更优；
///   3. 每个分区可独立挂 observer / semaphore 做精细控制。
///
/// 静态分区的劣势：
///   - 数据规模或 chunk 数运行期才确定时不适用 → 那种场景请用 09 的 Runtime 模式。

#include "../taskflowlite/taskflowlite.hpp"
#include <functional>
#include <iostream>
#include <vector>
#include <atomic>
#include <numeric>
#include <random>
#include <thread>
#include <chrono>
#include <syncstream>

int main() {
    std::osyncstream(std::cout) << "=== Example 17: Static-Partition Parallel Reduce ===\n\n";

    tfl::Executor executor(4);

    // ─────────────────────────────────────────────────────────
    // 1. 准备大数组（1M 个 int）
    // ─────────────────────────────────────────────────────────
    constexpr std::size_t N = 1'000'000;
    constexpr std::size_t PARTITIONS = 8;   // 静态分区数（编译期常量）

    std::vector<int> data(N);
    {
        std::mt19937 gen(42);
        std::uniform_int_distribution<int> dist(1, 10);
        for (auto& v : data) v = dist(gen);
    }

    // 每个分区独立的局部累加器（避免 false sharing：用 alignas）
    struct alignas(64) PartialSum {
        std::atomic<std::int64_t> value{0};
    };
    std::vector<PartialSum> partials(PARTITIONS);
    std::atomic<std::int64_t> final_sum{0};

    // ─────────────────────────────────────────────────────────
    // 2. 构建 fork-join 拓扑
    //
    //          init                           ── (1 节点)
    //          ├── worker_0
    //          ├── worker_1
    //          ├── ...                        ── (8 个并行 worker)
    //          └── worker_7
    //                  │
    //              reducer                    ── (1 汇聚节点)
    // ─────────────────────────────────────────────────────────
    tfl::Flow flow;
    flow.name("Parallel_Reduce");

    auto init = flow.emplace([PARTITIONS] {
        std::osyncstream(std::cout) << "[Init] Launching " << PARTITIONS << " parallel reduce workers\n";
    }).name("init");

    auto reducer = flow.emplace([&] {
        std::int64_t total = 0;
        for (auto& p : partials) total += p.value.load();
        final_sum.store(total);
        std::osyncstream(std::cout) << "[Reduce] Aggregating " << PARTITIONS
                  << " partition results = " << total << "\n";
    }).name("reducer");

    // 关键：循环建分区任务，每个任务捕获自己的 chunk 范围
    const std::size_t chunk = N / PARTITIONS;
    for (std::size_t p = 0; p < PARTITIONS; ++p) {
        const std::size_t lo = p * chunk;
        const std::size_t hi = (p == PARTITIONS - 1) ? N : (p + 1) * chunk;

        // 使用 emplace 参数转发：i = p, range = [lo, hi)
        // 这样捕获是值传递，不需要 std::ref，闭包更简洁
        auto worker = flow.emplace(std::bind_front(
            [&data, &partials](std::size_t idx, std::size_t a, std::size_t b) {
                std::int64_t local = 0;
                for (std::size_t i = a; i < b; ++i) local += data[i];
                partials[idx].value.store(local);
                std::osyncstream(std::cout) << "  [Worker " << idx << "] [" << a << ", " << b
                          << ") → " << local << "\n";
            },
            p, lo, hi
        )).name("worker_" + std::to_string(p));

        init.precede(worker);
        worker.precede(reducer);
    }

    // ─────────────────────────────────────────────────────────
    // 3. 计时执行
    // ─────────────────────────────────────────────────────────
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    executor.async(flow).wait();
    const auto t1 = Clock::now();

    const auto par_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            t1 - t0).count();

    // 串行 baseline
    const auto t2 = Clock::now();
    const std::int64_t serial = std::accumulate(
        data.begin(), data.end(), std::int64_t{0});
    const auto t3 = Clock::now();
    const auto seq_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            t3 - t2).count();

    std::osyncstream(std::cout) << "\n[Verify] parallel = " << final_sum.load()
              << ", serial = " << serial
              << " (" << (final_sum.load() == serial ? "OK" : "MISMATCH") << ")\n";
    std::osyncstream(std::cout) << "[Time]   parallel = " << par_us << " us, "
              << "serial = " << seq_us << " us, "
              << "speedup = " << (static_cast<double>(seq_us) / par_us) << "x\n";

    return 0;
}
