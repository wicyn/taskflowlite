/// @file 07_semaphore.cpp
/// @brief 演示 Semaphore 任务级并发控制——限流、延迟激活与容量重置。
///   覆盖 acquire/release 绑定、初始容量 0 的事件通知、安全 reset 等场景。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <string>
#include <mutex>
#include <chrono>
#include <thread>
#include <vector>

using namespace tfl;

// --- 线程安全日志 ---
std::mutex g_mtx;
#define LOG(msg) do { \
    std::lock_guard<std::mutex> lck(g_mtx); \
    std::cout << "[Semaphore-Demo] " << msg << std::endl; \
} while(0)


int main() {
    LOG("=== Semaphore Task-Level Concurrency Test Started ===");
    ResumeAlways handler;
    // 开启 4 个 Worker 线程，观察限流压制效果
    Executor executor(handler, 4);

    // ========================================================================
    // 场景 1：基础限流（保护受限 I/O 资源）
    // ========================================================================
    // 定义一个最大容量为 2 的信号量。
    // 即使有 4 个 Worker 线程，绑定到此信号量的任务最多也只能 2 个同时运行。
    Semaphore db_pool_sem(2);

    {
        LOG("\n--- [Test 1] DB Connection Pool Rate Limiting (Max: 2) ---");
        Flow flow_io;
        flow_io.name("IO_Stress_Test");

        // 一次性提交 5 个并发数据库查询任务
        for (int i = 1; i <= 5; ++i) {
            auto db_task = flow_io.emplace([i] {
                LOG("  -> Task " << i << " acquired DB connection, starting time-consuming query...");
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                LOG("  <- Task " << i << " query finished, releasing connection.");
                }).name("DB_Query_" + std::to_string(i));

            // 核心用法：给任务同时绑定 acquire 和 release
            db_task.acquire(db_pool_sem).release(db_pool_sem);
        }

        // 提交并阻塞等待完成。你将看到任务严格按 2 个一组输出。
        executor.async(std::move(flow_io)).wait();
        LOG("Test 1 finished. Current db_pool_sem available: " << db_pool_sem.value()
            << " / " << db_pool_sem.max_value());
    }

    // ========================================================================
    // 场景 2：非对称同步与延迟激活（初始值 0）
    // ========================================================================
    // 定义一个容量为 3 但初始可用值为 0 的信号量。
    // 这意味着任何尝试 acquire 的任务一开始都会挂起，直到其他任务 release。
    Semaphore init_event_sem(3, 0);

    {
        LOG("\n--- [Test 2] Delayed Activation & Event Notification (Init: 0, Max: 3) ---");
        Flow flow_event;
        flow_event.name("Event_Driven_Test");

        // 创建 3 个饥饿的消费者任务。启动时会尝试 acquire，失败后挂起。
        for (int i = 1; i <= 3; ++i) {
            flow_event.emplace([i] {
                LOG("    [Consumer " << i << "] finally got authorization, starting subsequent logic!");
                }).name("Consumer_" + std::to_string(i))
                    .acquire(init_event_sem); // 注意：只 acquire，不 release
        }

        // 创建生产者任务，负责初始化资源后一次性释放 3 个配额。
        auto producer = flow_event.emplace([] {
            LOG("  [Producer] Performing long global initialization...");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            LOG("  [Producer] Initialization complete! Releasing 3 semaphore quotas to wake up consumers.");
            }).name("Producer");

        // 核心用法：给任务绑定多次 release，或分别 release。
        // 这里模拟生产者完成后释放 3 个配额。
        producer.release( init_event_sem, 3 );

        // 注意：这里没有设置 'precede' 依赖边！
        // 消费者和生产者在图层面完全并行；同步完全通过信号量机制完成。
        executor.async(std::move(flow_event)).wait();

        LOG("Test 2 finished. Current init_event_sem available: " << init_event_sem.value());
    }

    // ========================================================================
    // 场景 3：状态重置与异常防护
    // ========================================================================
    {
        LOG("\n--- [Test 3] Semaphore Safe Reset ---");

        LOG("db_pool_sem max capacity before reset: " << db_pool_sem.max_value());

        // 动态扩容：假设业务需要在运行时把 DB 连接池升级到 10
        db_pool_sem.reset(10);
        LOG("db_pool_sem max capacity after reset: " << db_pool_sem.max_value()
            << " (Available: " << db_pool_sem.value() << ")");

        // 同时显式指定当前可用值
        db_pool_sem.reset(10, 5);
        LOG("db_pool_sem max capacity after reset with current value: " << db_pool_sem.max_value()
            << " (Available: " << db_pool_sem.value() << ")");

        // 异常测试防护：如果有任务当前正挂起等待，严禁 reset！
        // 因为 reset 操作可能导致 m_waiters 节点与容量之间发生逻辑撕裂。
        // 源码中抛出的异常完美防止了这一点。
    }

    executor.wait_for_all();
    LOG("\n=== Semaphore Test Completed Successfully ===");
    return 0;
}
