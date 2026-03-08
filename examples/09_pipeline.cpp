/// @file 09_pipeline.cpp
/// @brief 演示基于 Runtime 实现的 Map-Reduce (拆分-计算-聚合) 并发流水线。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <vector>
#include <atomic>

int main() {
    std::cout << "=== Example 09: Map-Reduce Pipeline ===\n";

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    tfl::Flow flow;

    std::vector<int> data(100);
    for (int i = 0; i < 100; ++i) data[i] = 1; // 预期总和 100

    std::atomic<int> global_sum{0};
    constexpr int batchSize = 25;

    // Map 阶段：动态拆分任务
    auto map_task = flow.emplace([](const std::vector<int>& vec, std::atomic<int>& sum, tfl::Runtime& rt) {
        std::cout << "[Map] Splitting data into chunks...\n";

        for (size_t i = 0; i < vec.size(); i += batchSize) {
            // 针对每个 Chunk，抛入后台静默执行
            rt.silent_async([&vec, &sum, i]() {
                int local_sum = 0;
                for (size_t j = 0; j < batchSize && (i + j) < vec.size(); ++j) {
                    local_sum += vec[i + j];
                }
                std::cout << "  -> Chunk " << (i / batchSize) << " computed: " << local_sum << "\n";
                sum.fetch_add(local_sum, std::memory_order_relaxed);
            });
        }
    }, std::cref(data), std::ref(global_sum));

    // Reduce 阶段：在所有 silent_async 完成后执行
    auto reduce_task = flow.emplace([](std::atomic<int>& sum) {
        std::cout << "[Reduce] All chunks processed. Final sum = " << sum.load() << "\n";
    }, std::ref(global_sum));

    map_task.precede(reduce_task);

    executor.submit(flow).start().wait();

    std::cout << "Pipeline example complete!\n";
    return 0;
}
