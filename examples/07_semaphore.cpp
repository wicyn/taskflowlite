/// @file 07_semaphore.cpp
/// @brief 演示 Semaphore 限制任务的最大并发度，展示值捕获与引用捕获的经典混用。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>

int main245254() {
    std::cout << "=== Example 07: Semaphore (Concurrency Limit) ===\n";

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4); // 4 个物理线程

    tfl::Semaphore dbLimit(2); // 限制只能 2 个任务同时访问
    std::atomic<int> active{ 0 };
    std::mutex coutMutex;

    tfl::Flow flow;

    for (int i = 0; i < 5; ++i) {
        // 核心：引用捕获共享状态 [&active, &coutMutex]，值捕获循环变量 [i]
        auto task = flow.emplace([&active, &coutMutex, i] {
            int current = active.fetch_add(1) + 1;
            {
                std::lock_guard lock(coutMutex);
                std::cout << "[Worker] Task " << i << " executing. Active DB connections: " << current << "\n";
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            active.fetch_sub(1);
            });

        // 声明该任务需要消耗 dbLimit 配额
        task.acquire(dbLimit).release(dbLimit);
    }

    std::cout << "Submitting 5 tasks to 4 threads, but Semaphore limit is 2...\n";
    executor.submit(flow).start().wait();
    return 0;
}


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
    // Start 4 Worker threads to observe the concurrency suppression effect
    Executor executor(handler, 4);

    // ========================================================================
    // Scenario 1: Basic Rate Limiting (Protecting restricted I/O resources)
    // ========================================================================
    // Define a semaphore with a maximum capacity of 2.
    // Even with 4 Worker threads, a maximum of 2 tasks bound to this semaphore 
    // can run simultaneously.
    Semaphore db_pool_sem(2);

    {
        LOG("\n--- [Test 1] DB Connection Pool Rate Limiting (Max: 2) ---");
        Flow flow_io;
        flow_io.name("IO_Stress_Test");

        // Instantly submit 5 concurrent database query tasks
        for (int i = 1; i <= 5; ++i) {
            auto db_task = flow_io.emplace([i] {
                LOG("  -> Task " << i << " acquired DB connection, starting time-consuming query...");
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                LOG("  <- Task " << i << " query finished, releasing connection.");
                }).name("DB_Query_" + std::to_string(i));

            // Core usage: Bind both acquire and release to the task
            db_task.acquire(db_pool_sem).release(db_pool_sem);
        }

        // Submit and block until finished. You will see tasks strictly outputting in pairs of 2.
        executor.submit(std::move(flow_io)).start().wait();
        LOG("Test 1 finished. Current db_pool_sem available: " << db_pool_sem.value()
            << " / " << db_pool_sem.max_value());
    }

    // ========================================================================
    // Scenario 2: Asymmetric Sync & Delayed Activation (Initial value 0)
    // ========================================================================
    // Define a semaphore with a capacity of 3, but an initial available value of 0.
    // This means any task attempting to acquire it initially will be suspended 
    // until another task releases it.
    Semaphore init_event_sem(3, 0);

    {
        LOG("\n--- [Test 2] Delayed Activation & Event Notification (Init: 0, Max: 3) ---");
        Flow flow_event;
        flow_event.name("Event_Driven_Test");

        // Create 3 hungry consumer tasks. They will try to acquire upon startup, 
        // fail, and get suspended.
        for (int i = 1; i <= 3; ++i) {
            flow_event.emplace([i] {
                LOG("    [Consumer " << i << "] finally got authorization, starting subsequent logic!");
                }).name("Consumer_" + std::to_string(i))
                    .acquire(init_event_sem); // Note: Only acquire, no release here
        }

        // Create a producer task responsible for initializing resources, 
        // then releasing 3 quotas at once.
        auto producer = flow_event.emplace([] {
            LOG("  [Producer] Performing long global initialization...");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            LOG("  [Producer] Initialization complete! Releasing 3 semaphore quotas to wake up consumers.");
            }).name("Producer");

        // Core usage: Bind multiple identical releases to a task, or release separately.
        // Here we simulate the producer releasing 3 quotas after finishing.
        producer.release( init_event_sem, 3 );

        // Note: No 'precede' dependency edges are set here!
        // Consumers and producers are completely parallel at the DAG level; 
        // synchronization is achieved purely through the semaphore mechanism.
        executor.submit(std::move(flow_event)).start().wait();

        LOG("Test 2 finished. Current init_event_sem available: " << init_event_sem.value());
    }

    // ========================================================================
    // Scenario 3: State Reset & Exception Prevention
    // ========================================================================
    {
        LOG("\n--- [Test 3] Semaphore Safe Reset ---");

        LOG("db_pool_sem max capacity before reset: " << db_pool_sem.max_value());

        // Dynamic expansion: Suppose business needs require upgrading the DB pool to 10 at runtime
        db_pool_sem.reset(10);
        LOG("db_pool_sem max capacity after reset: " << db_pool_sem.max_value()
            << " (Available: " << db_pool_sem.value() << ")");

        // Reset with an explicitly specified current value
        db_pool_sem.reset(10, 5);
        LOG("db_pool_sem max capacity after reset with current value: " << db_pool_sem.max_value()
            << " (Available: " << db_pool_sem.value() << ")");

        // Exception test prevention: If tasks are currently suspended and waiting, resetting is strictly prohibited!
        // Because a reset operation might cause logical tearing between the m_waiters nodes and the capacity.
        // The Exception thrown in your source code perfectly prevents this.
    }

    executor.wait_for_all();
    LOG("\n=== Semaphore Test Completed Successfully ===");
    return 0;
}