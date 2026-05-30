/// @file 13_parallel_reduce.cpp
/// @brief 并行 Map-Reduce：将大数据集分片并行处理，结果汇总到 atomic 累加器。
///
/// 构建: g++ -std=c++23 -O2 -pthread 13_parallel_reduce.cpp -o 13_parallel_reduce

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <vector>
#include <atomic>
#include <random>
#include <numeric>
#include <cstdint>

int main() {
    std::cout << "=== Example 13: Parallel Map-Reduce ===\n\n";

    // 生成测试数据
    constexpr std::size_t N = 10'000'000;
    std::vector<std::int64_t> data(N);
    {
        std::mt19937_64 rng{42};
        std::uniform_int_distribution<std::int64_t> dist(1, 100);
        for (auto& v : data) v = dist(rng);
    }

    // 串行求和（用于验证正确性）
    const std::int64_t expected = std::accumulate(data.begin(), data.end(), 0LL);
    std::cout << "Data size: " << N << ", expected sum: " << expected << "\n";

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, std::thread::hardware_concurrency());
    tfl::Flow flow;
    flow.name("Parallel_Reduce");

    std::atomic<std::int64_t> total_sum{0};

    // ---- Map 阶段：将数据拆分为多个 chunk，并行处理 ----
    constexpr std::size_t NUM_CHUNKS = 8;
    const std::size_t chunk_size = N / NUM_CHUNKS;
    std::vector<tfl::Task> chunks;
    chunks.reserve(NUM_CHUNKS);

    for (std::size_t c = 0; c < NUM_CHUNKS; ++c) {
        const std::size_t start = c * chunk_size;
        const std::size_t end   = (c == NUM_CHUNKS - 1) ? N : start + chunk_size;

        auto task = flow.emplace(
            [&data, &total_sum, start, end] {
                std::int64_t local = 0;
                for (std::size_t i = start; i < end; ++i) {
                    local += data[i];
                }
                // Why: fetch_add relaxed — 只关心最终总和，不关心各 chunk 的顺序
                total_sum.fetch_add(local, std::memory_order_relaxed);
            }
        ).name("Chunk_" + std::to_string(c));
        chunks.push_back(task);
    }

    // ---- Reduce 阶段：等待所有 chunk 完成后汇总 ----
    auto reduce = flow.emplace([&total_sum, expected] {
        const std::int64_t result = total_sum.load(std::memory_order_relaxed);
        std::cout << "Parallel sum: " << result << "\n";
        std::cout << (result == expected ? "✓ Correct!" : "✗ ERROR: mismatch!") << "\n";
    }).name("Reduce");

    // 所有 chunk → reduce（fan-in：reduce 等待全部 chunk 完成）
    for (auto& chunk : chunks) chunk.precede(reduce);

    executor.async(flow).wait();

    return 0;
}
