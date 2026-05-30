/// @file 18_pipeline_producer_consumer.cpp
/// @brief 演示多阶段流水线：生产→处理→消费，各阶段间通过 DAG 依赖 + Semaphore 限流。
///
/// 构建: g++ -std=c++23 -O2 -pthread 18_pipeline_producer_consumer.cpp -o 18_pipeline_producer_consumer

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <string>
#include <chrono>

/// @brief 线程安全的有界队列 — 模拟流水线阶段间的缓冲区。
template <typename T>
class BoundedBuffer {
public:
    explicit BoundedBuffer(std::size_t capacity) : m_capacity(capacity) {}

    void push(T item) {
        std::unique_lock lk(m_mtx);
        m_not_full.wait(lk, [this] { return m_queue.size() < m_capacity; });
        m_queue.push(std::move(item));
        m_not_empty.notify_one();
    }

    T pop() {
        std::unique_lock lk(m_mtx);
        m_not_empty.wait(lk, [this] { return !m_queue.empty(); });
        T item = std::move(m_queue.front());
        m_queue.pop();
        m_not_full.notify_one();
        return item;
    }

private:
    std::queue<T> m_queue;
    std::size_t m_capacity;
    std::mutex m_mtx;
    std::condition_variable m_not_empty;
    std::condition_variable m_not_full;
};

int main() {
    std::cout << "=== Example 18: Pipeline (Producer-Consumer) ===\n\n";

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);

    // ================================================================
    //  三阶段流水线
    //  Stage 1 (Generate)  →  Stage 2 (Process)  →  Stage 3 (Consume)
    //
    //  每个 Stage 内部使用 DAG 并行，Stage 之间通过 Semaphore 控制并发
    // ================================================================

    constexpr int NUM_BATCHES = 6;
    constexpr int ITEMS_PER_BATCH = 100;

    // Stage 间缓冲区
    BoundedBuffer<int> buf_s1_s2(200);  // Stage1 → Stage2
    BoundedBuffer<int> buf_s2_s3(200);  // Stage2 → Stage3

    std::atomic<std::int64_t> total_produced{0};
    std::atomic<std::int64_t> total_processed{0};
    std::atomic<std::int64_t> total_consumed{0};

    // 限流信号量：每个阶段最多 2 个并发批次
    tfl::Semaphore s1_slots(2, "s1_slots");
    tfl::Semaphore s2_slots(2, "s2_slots");
    tfl::Semaphore s3_slots(2, "s3_slots");

    tfl::Flow pipeline;
    pipeline.name("Three_Stage_Pipeline");

    // ---- 为每批数据创建一组 (Stage1 → Stage2 → Stage3) 任务 ----
    for (int batch = 0; batch < NUM_BATCHES; ++batch) {
        // Stage 1: 生产数据
        auto s1 = pipeline.emplace([&total_produced, batch] {
            std::int64_t local = 0;
            for (int i = 0; i < ITEMS_PER_BATCH; ++i) {
                local += (batch * 1000 + i) % 97;  // 伪计算
            }
            total_produced.fetch_add(local, std::memory_order_relaxed);
            std::cout << "  [S1] Batch " << batch << " produced, subtotal="
                      << local << "\n";
        }).name("Gen_Batch_" + std::to_string(batch));
        s1.acquire(s1_slots).release(s1_slots);

        // Stage 2: 处理数据
        auto s2 = pipeline.emplace([&total_processed, batch] {
            std::int64_t local = 0;
            for (int i = 0; i < ITEMS_PER_BATCH; ++i) {
                local += (batch * 1000 + i) * 2 % 101;  // 伪处理
            }
            total_processed.fetch_add(local, std::memory_order_relaxed);
            std::cout << "  [S2] Batch " << batch << " processed, subtotal="
                      << local << "\n";
        }).name("Proc_Batch_" + std::to_string(batch));
        s2.acquire(s2_slots).release(s2_slots);

        // Stage 3: 消费/汇总
        auto s3 = pipeline.emplace([&total_consumed, batch] {
            std::int64_t local = 0;
            for (int i = 0; i < ITEMS_PER_BATCH; ++i) {
                local += (batch * 1000 + i) * 3 % 103;  // 伪消费
            }
            total_consumed.fetch_add(local, std::memory_order_relaxed);
            std::cout << "  [S3] Batch " << batch << " consumed, subtotal="
                      << local << "\n";
        }).name("Cons_Batch_" + std::to_string(batch));
        s3.acquire(s3_slots).release(s3_slots);

        // 同一批次内的依赖链：S1 → S2 → S3
        s1.precede(s2);
        s2.precede(s3);

        // Why: 不同批次之间没有显式依赖，由 Semaphore 控制并发数。
        //      这样不同批次的 Stage1/Stage2/Stage3 可以流水线式重叠执行：
        //      Batch0/S3 可以和 Batch1/S2、Batch2/S1 同时跑。
    }

    executor.async(pipeline).wait();

    std::cout << "\n--- Pipeline Results ---\n";
    std::cout << "Total produced:  " << total_produced.load() << "\n";
    std::cout << "Total processed: " << total_processed.load() << "\n";
    std::cout << "Total consumed:  " << total_consumed.load() << "\n";

    return 0;
}
