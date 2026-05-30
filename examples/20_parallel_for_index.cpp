/// @file 14_parallel_for_index.cpp
/// @brief 并行 for 循环：将线性索引范围分片，批量创建任务并行执行。
///
/// 构建: g++ -std=c++23 -O2 -pthread 14_parallel_for_index.cpp -o 14_parallel_for_index

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <vector>
#include <atomic>
#include <cmath>
#include <cstdint>

/// @brief 手动并行 for 循环 — 将 [0, N) 拆成 num_chunks 块，每块一个任务。
/// 这不是框架内置函数，而是用 Flow + emplace 搭出来的模式。
template <typename F>
void parallel_for(tfl::Flow& flow, std::size_t N, std::size_t num_chunks, F&& func) {
    const std::size_t chunk_size = (N + num_chunks - 1) / num_chunks;
    for (std::size_t c = 0; c < num_chunks; ++c) {
        const std::size_t start = c * chunk_size;
        const std::size_t end   = std::min(start + chunk_size, N);
        if (start >= N) break;

        // Why: func 通过 std::ref 传递引用，避免拷贝闭包（func 可能很大）
        flow.emplace([start, end, &func = func] {
            for (std::size_t i = start; i < end; ++i) {
                func(i);
            }
        }).name("Chunk_" + std::to_string(c));
    }
}

int main() {
    std::cout << "=== Example 14: Parallel For-Index ===\n\n";

    constexpr std::size_t N = 10'000'000;

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, std::thread::hardware_concurrency());

    // ================================================================
    //  场景 1：计算每个元素的平方，验证正确性
    // ================================================================
    {
        tfl::Flow flow;
        flow.name("Compute_Squares");

        std::vector<std::int64_t> results(N, 0);
        std::atomic<std::size_t> verified{0};

        const std::size_t num_chunks = std::thread::hardware_concurrency() * 4;
        // 每个 chunk 的 lambda 在整个 chunk 内迭代，减少了任务提交开销
        for (std::size_t c = 0; c < num_chunks; ++c) {
            const std::size_t start = c * (N / num_chunks);
            const std::size_t end   = (c == num_chunks - 1) ? N : start + N / num_chunks;

            flow.emplace([&results, &verified, start, end] {
                for (std::size_t i = start; i < end; ++i) {
                    results[i] = static_cast<std::int64_t>(i) * i;
                }
                verified.fetch_add(end - start, std::memory_order_relaxed);
            }).name("Chunk_" + std::to_string(c));
        }

        executor.async(flow).wait();

        // 快速抽查验证
        bool ok = (results[0] == 0) && (results[1] == 1) &&
                  (results[100] == 10000LL) &&
                  (results[N-1] == static_cast<std::int64_t>(N-1) * static_cast<std::int64_t>(N-1));
        std::cout << "Square computation: " << (ok ? "✓ Correct" : "✗ ERROR")
                  << " (verified " << verified.load() << " elements)\n";
    }

    // ================================================================
    //  场景 2：并行筛法标记素数（演示共享数组写入无竞争）
    // ================================================================
    {
        tfl::Flow flow;
        flow.name("Prime_Sieve");

        std::vector<std::atomic<bool>> is_prime(N);
        // 初始化：全部标记为 true（单线程足够快）
        for (auto& flag : is_prime) flag.store(true, std::memory_order_relaxed);
        is_prime[0].store(false, std::memory_order_relaxed);
        is_prime[1].store(false, std::memory_order_relaxed);

        const std::size_t limit = static_cast<std::size_t>(std::sqrt(N)) + 1;
        const std::size_t num_chunks = std::thread::hardware_concurrency() * 2;

        // 外层串行遍历小素数，内层并行标记倍数
        for (std::size_t p = 2; p < limit; ++p) {
            if (!is_prime[p].load(std::memory_order_relaxed)) continue;

            tfl::Flow inner_flow;
            inner_flow.name("Mark_Multiples_of_" + std::to_string(p));

            const std::size_t batch = std::max<std::size_t>((N - p * p) / num_chunks, 1000);
            for (std::size_t start = p * p; start < N; ) {
                const std::size_t end = std::min(start + batch, N);
                inner_flow.emplace([&is_prime, p, start, end] {
                    for (std::size_t k = start; k < end; k += p) {
                        is_prime[k].store(false, std::memory_order_relaxed);
                    }
                });
                start = end;
            }

            executor.async(std::move(inner_flow)).wait();
        }

        // 统计素数个数
        std::atomic<std::size_t> prime_count{0};
        tfl::Flow count_flow;
        const std::size_t count_chunks = num_chunks * 2;
        for (std::size_t c = 0; c < count_chunks; ++c) {
            const std::size_t start = c * (N / count_chunks);
            const std::size_t end   = (c == count_chunks - 1) ? N : start + N / count_chunks;

            count_flow.emplace([&is_prime, &prime_count, start, end] {
                std::size_t local = 0;
                for (std::size_t i = start; i < end; ++i) {
                    if (is_prime[i].load(std::memory_order_relaxed)) ++local;
                }
                prime_count.fetch_add(local, std::memory_order_relaxed);
            });
        }
        executor.async(std::move(count_flow)).wait();

        std::cout << "Primes under " << N << ": " << prime_count.load() << "\n";
    }

    executor.wait_for_all();
    std::cout << "Done.\n";
    return 0;
}
