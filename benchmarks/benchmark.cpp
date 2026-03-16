#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <atomic>
#include <chrono>

constexpr size_t NUM_LAYERS = 100;           // 层数
constexpr size_t NUM_TASKS_PER_LAYER = 100;  // 每层任务数
constexpr size_t NUM_THREADS =8;
constexpr size_t NUM_ITERATIONS = 10;

int main() {
{
        tfl::ResumeNever h;
        tfl::Executor executor(h, NUM_THREADS);
        tfl::Flow taskflow;
        std::atomic<int> counter{0};

        int ss =0;

        // 存储所有层的任务
        std::vector<std::vector<tfl::Task>> layers(NUM_LAYERS);

        // 创建每一层
        for (size_t layer = 0; layer < NUM_LAYERS; ++layer) {
            layers[layer].reserve(NUM_TASKS_PER_LAYER);

            for (size_t i = 0; i < NUM_TASKS_PER_LAYER; ++i) {
                auto task = taskflow.emplace([&](){
                    counter.fetch_add(1, std::memory_order_relaxed);
                });
                layers[layer].push_back(task);
            }

            // 全连接：上一层每个任务 -> 当前层每个任务
            if (layer > 0) {
                for (auto& prev : layers[layer - 1]) {
                    for (auto& curr : layers[layer]) {
                        prev.precede(curr);
                    }
                }
            }
        }

        // 预热
        auto async_task = executor.submit(taskflow, NUM_ITERATIONS);

        // 正式测试
        counter.store(0);
        auto start = std::chrono::high_resolution_clock::now();

        async_task.start().wait();

        auto end = std::chrono::high_resolution_clock::now();
        auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

        size_t total_tasks = NUM_LAYERS * NUM_TASKS_PER_LAYER;
        size_t total_edges = (NUM_LAYERS - 1) * NUM_TASKS_PER_LAYER * NUM_TASKS_PER_LAYER;
        size_t total_task_executions = total_tasks * NUM_ITERATIONS;
        int expected = static_cast<int>(total_tasks * NUM_ITERATIONS);

        double avg_per_run_ns = static_cast<double>(duration_ns) / NUM_ITERATIONS;
        double avg_per_task_ns = static_cast<double>(duration_ns) / total_task_executions;

        std::cout << "=== Full-Connected Neural Network Style ===" << std::endl;
        std::cout << "Threads: " << NUM_THREADS << std::endl;
        std::cout << "Layers: " << NUM_LAYERS << ", Tasks/Layer: " << NUM_TASKS_PER_LAYER << std::endl;
        std::cout << "Total tasks: " << total_tasks << std::endl;
        std::cout << "Total edges: " << total_edges << std::endl;
        std::cout << "Iterations: " << NUM_ITERATIONS << std::endl;
        std::cout << "--------------------------------------------" << std::endl;
        std::cout << "Counter: " << counter.load() << " (expected: " << expected << ")" << std::endl;
        std::cout << "Total time: " << duration_ns / 1000000.0 << " ms" << std::endl;
        std::cout << "Avg per run: " << avg_per_run_ns << " ns" << std::endl;
        std::cout << "Avg per task: " << avg_per_task_ns << " ns" << std::endl;
    }
    return 0;

        // === Full-Connected Neural Network Style ===
        // Threads: 8
        // Layers: 100, Tasks/Layer: 100
        // Total tasks: 10000
        // Total edges: 990000
        // Iterations: 100
        // --------------------------------------------
        // Counter: 1000000 (expected: 1000000)
        // Total time: 513.066 ms
        // Avg per run: 5.13066e+06 ns
        // Avg per task: 513.066 ns
}




// #include <taskflow/taskflow.hpp>
// #include <chrono>
// #include <iostream>
// #include <atomic>


// constexpr size_t NUM_LAYERS = 100;      // 层数
// constexpr size_t NUM_TASKS_PER_LAYER = 100;  // 每层任务数
// constexpr size_t NUM_THREADS = 8;
// constexpr size_t NUM_ITERATIONS = 100;

// int main() {
//     {
//         tf::Executor executor(NUM_THREADS);
//         tf::Taskflow taskflow;
//         std::atomic<int> counter{0};

//         // 存储所有层的任务
//         std::vector<std::vector<tf::Task>> layers(NUM_LAYERS);


//         // 创建每一层
//         for (size_t layer = 0; layer < NUM_LAYERS; ++layer) {
//             layers[layer].reserve(NUM_TASKS_PER_LAYER);

//             for (size_t i = 0; i < NUM_TASKS_PER_LAYER; ++i) {
//                 auto task = taskflow.emplace([&]{
//                     counter.fetch_add(1, std::memory_order_relaxed);
//                 });
//                 layers[layer].push_back(task);
//             }

//             // 全连接：上一层每个任务 -> 当前层每个任务
//             if (layer > 0) {
//                 for (auto& prev : layers[layer - 1]) {
//                     for (auto& curr : layers[layer]) {
//                         prev.precede(curr);
//                     }
//                 }
//             }
//         }


//         // 正式测试
//         counter.store(0);
//         auto start = std::chrono::high_resolution_clock::now();

//         executor.run_n(taskflow, NUM_ITERATIONS).wait();


//         auto end = std::chrono::high_resolution_clock::now();
//         auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

//         size_t total_tasks = NUM_LAYERS * NUM_TASKS_PER_LAYER;
//         size_t total_edges = (NUM_LAYERS - 1) * NUM_TASKS_PER_LAYER * NUM_TASKS_PER_LAYER;
//         size_t total_task_executions = total_tasks * NUM_ITERATIONS;
//         int expected = static_cast<int>(total_tasks * NUM_ITERATIONS);

//         double avg_per_run_ns = static_cast<double>(duration_ns) / NUM_ITERATIONS;
//         double avg_per_task_ns = static_cast<double>(duration_ns) / total_task_executions;

//         std::cout << "=== Full-Connected Neural Network Style ===" << std::endl;
//         std::cout << "Threads: " << NUM_THREADS << std::endl;
//         std::cout << "Layers: " << NUM_LAYERS << ", Tasks/Layer: " << NUM_TASKS_PER_LAYER << std::endl;
//         std::cout << "Total tasks: " << total_tasks << std::endl;
//         std::cout << "Total edges: " << total_edges << std::endl;
//         std::cout << "Iterations: " << NUM_ITERATIONS << std::endl;
//         std::cout << "--------------------------------------------" << std::endl;
//         std::cout << "Counter: " << counter.load() << " (expected: " << expected << ")" << std::endl;
//         std::cout << "Total time: " << duration_ns / 1000000.0 << " ms" << std::endl;
//         std::cout << "Avg per run: " << avg_per_run_ns << " ns" << std::endl;
//         std::cout << "Avg per task: " << avg_per_task_ns << " ns" << std::endl;

//         // taskflow.dump(std::cout);
//     }
//     return 0;

//     // === Full-Connected Neural Network Style ===
//     // Threads: 8
//     // Layers: 100, Tasks/Layer: 100
//     // Total tasks: 10000
//     // Total edges: 990000
//     // Iterations: 100
//     // --------------------------------------------
//     // Counter: 1000000 (expected: 1000000)
//     // Total time: 791.706 ms
//     // Avg per run: 7.91706e+06 ns
//     // Avg per task: 791.706 ns
// }
