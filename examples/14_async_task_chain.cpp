/// @file 14_async_task_chain.cpp
/// @brief 演示 AsyncTask 依赖链 —— 运行期动态拼接任务依赖关系。
///
/// 与 Flow 静态图的对比：
///   - Flow:      预先 emplace 所有节点、precede 连边、async(flow).wait() 提交；
///   - AsyncTask: 每个任务是独立的引用计数句柄，可在运行期通过 run(task, deps...)
///                动态拼接依赖，适合"依赖关系运行期才确定"的 ETL / pipeline 场景。
///
/// 关键 API：
///   - AsyncTask(callable)     创建 Idle 态任务句柄
///   - executor.run(task)           提交任务（无依赖直接启动）
///   - executor.run(task, deps...)  提交任务并声明依赖
///   - task.wait() / task.get()        阻塞等待 / 等待 + 取异常

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <atomic>
#include <string>
#include <thread>
#include <chrono>
#include <syncstream>

int main() {
    std::osyncstream(std::cout) << "=== Example 14: AsyncTask Explicit Dependency Chain ===\n\n";

    tfl::Executor executor(4);

    // ======================================================================
    // 场景：经典 ETL（Extract -> Transform -> Load）三段式数据管道
    //
    //          extract                       (no deps)
    //             +-- transform_a            (deps: extract)
    //             +-- transform_b            (deps: extract)
    //                       |
    //                       +---- load        (deps: transform_a, transform_b)
    //                       +---- audit       (deps: transform_a, transform_b)
    //
    // 5 个独立任务，依赖关系通过 run(task, deps...) 运行期建好。
    // ======================================================================

    std::atomic<int> rows_extracted{0};
    std::atomic<int> rows_transformed{0};

    // Step 1: Create all tasks (Idle state, not yet submitted)
    auto extract = tfl::AsyncTask([&] {
        std::osyncstream(std::cout) << "[Extract] Reading records from source database...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        rows_extracted.store(1000);
    });
    extract.name("extract");

    auto transform_a = tfl::AsyncTask([&] {
        std::osyncstream(std::cout) << "[Transform-A] Data cleaning (NULL fill, type conversion)...\n";
        rows_transformed.fetch_add(rows_extracted.load() / 2);
    });
    transform_a.name("transform_a");

    auto transform_b = tfl::AsyncTask([&] {
        std::osyncstream(std::cout) << "[Transform-B] Field aggregation (group-by, sum)...\n";
        rows_transformed.fetch_add(rows_extracted.load() / 2);
    });
    transform_b.name("transform_b");

    auto load = tfl::AsyncTask([&] {
        std::osyncstream(std::cout) << "[Load] Writing to target warehouse (rows="
                  << rows_transformed.load() << ")\n";
    });
    load.name("load");

    auto audit = tfl::AsyncTask([&] {
        std::osyncstream(std::cout) << "[Audit] Audit log recording completed\n";
    });
    audit.name("audit");

    // Step 2: Declare dependencies via run(task, deps...)
    // 先提交根任务（无依赖），再提交下游（声明其依赖的上游）
    executor.run(extract);
    executor.run(transform_a, extract);
    executor.run(transform_b, extract);
    executor.run(load,       transform_a, transform_b);
    executor.run(audit,      transform_a, transform_b);

    // Step 3: Wait for the final downstream nodes
    audit.wait();
    load.wait();
    executor.wait_for_all();

    std::osyncstream(std::cout) << "\n[Done] ETL pipeline complete, processed " << rows_transformed.load()
              << " rows\n";

    // ======================================================================
    // Advanced: get() rethrows exceptions
    // ======================================================================
    std::osyncstream(std::cout) << "\n--- Advanced: get() rethrows exceptions ---\n";
    auto risky = tfl::AsyncTask([] {
        throw std::runtime_error("Simulated downstream service timeout");
    });
    executor.run(risky);

    try {
        risky.get();   // wait + rethrow
    } catch (const std::exception& e) {
        std::osyncstream(std::cout) << "[Caught] " << e.what() << "\n";
    }

    return 0;
}
