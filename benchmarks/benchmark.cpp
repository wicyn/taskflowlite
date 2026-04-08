#include "../taskflowlite/taskflowlite.hpp"
#include "taskflow/taskflow.hpp"
#include <iostream>
#include <atomic>
#include <chrono>

constexpr size_t NUM_LAYERS = 100;
constexpr size_t NUM_TASKS_PER_LAYER = 100;
constexpr size_t NUM_THREADS = 8;
constexpr size_t NUM_ITERATIONS = 10;

void tfl_full_connected() {
    tfl::ResumeNever h;
    tfl::Executor executor(h, NUM_THREADS);
    tfl::Flow taskflow;
    std::atomic<int> counter{0};

    std::vector<std::vector<tfl::Task>> layers(NUM_LAYERS);
    for (size_t layer = 0; layer < NUM_LAYERS; ++layer) {
        layers[layer].reserve(NUM_TASKS_PER_LAYER);
        for (size_t i = 0; i < NUM_TASKS_PER_LAYER; ++i) {
            layers[layer].push_back(taskflow.emplace([&]() {
                counter.fetch_add(1, std::memory_order_relaxed);
            }));
        }
        if (layer > 0) {
            for (auto& prev : layers[layer - 1])
                for (auto& curr : layers[layer])
                    prev.precede(curr);
        }
    }

    auto async_task = executor.submit(taskflow, NUM_ITERATIONS);
    counter.store(0);
    auto start = std::chrono::high_resolution_clock::now();
    async_task.start().wait();
    auto end = std::chrono::high_resolution_clock::now();

    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    size_t total = NUM_LAYERS * NUM_TASKS_PER_LAYER;

    std::cout << "=== [TFL] Full-Connected ===" << std::endl;
    std::cout << "Total time: " << ns / 1e6 << " ms" << std::endl;
    std::cout << "Avg per run: " << ns / NUM_ITERATIONS << " ns" << std::endl;
    std::cout << "Avg per task: " << static_cast<double>(ns) / (total * NUM_ITERATIONS) << " ns" << std::endl;
    std::cout << "Counter: " << counter.load() << " (expected: " << total * NUM_ITERATIONS << ")\n" << std::endl;
}

void tfl_no_connection() {
    tfl::ResumeNever h;
    tfl::Executor executor(h, NUM_THREADS);
    tfl::Flow taskflow;
    std::atomic<int> counter{0};

    for (size_t layer = 0; layer < NUM_LAYERS; ++layer)
        for (size_t i = 0; i < NUM_TASKS_PER_LAYER; ++i)
            taskflow.emplace([&]() {
                counter.fetch_add(1, std::memory_order_relaxed);
            });

    auto async_task = executor.submit(taskflow, NUM_ITERATIONS);
    counter.store(0);
    auto start = std::chrono::high_resolution_clock::now();
    async_task.start().wait();
    auto end = std::chrono::high_resolution_clock::now();

    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    size_t total = NUM_LAYERS * NUM_TASKS_PER_LAYER;

    std::cout << "=== [TFL] No Connection (Pure Parallel) ===" << std::endl;
    std::cout << "Total time: " << ns / 1e6 << " ms" << std::endl;
    std::cout << "Avg per run: " << ns / NUM_ITERATIONS << " ns" << std::endl;
    std::cout << "Avg per task: " << static_cast<double>(ns) / (total * NUM_ITERATIONS) << " ns" << std::endl;
    std::cout << "Counter: " << counter.load() << " (expected: " << total * NUM_ITERATIONS << ")\n" << std::endl;
}

void tf_full_connected() {
    tf::Executor executor(NUM_THREADS);
    tf::Taskflow taskflow;
    std::atomic<int> counter{0};

    std::vector<std::vector<tf::Task>> layers(NUM_LAYERS);
    for (size_t layer = 0; layer < NUM_LAYERS; ++layer) {
        layers[layer].reserve(NUM_TASKS_PER_LAYER);
        for (size_t i = 0; i < NUM_TASKS_PER_LAYER; ++i) {
            layers[layer].push_back(taskflow.emplace([&]() {
                counter.fetch_add(1, std::memory_order_relaxed);
            }));
        }
        if (layer > 0) {
            for (auto& prev : layers[layer - 1])
                for (auto& curr : layers[layer])
                    prev.precede(curr);
        }
    }

    counter.store(0);
    auto start = std::chrono::high_resolution_clock::now();
    executor.run_n(taskflow, NUM_ITERATIONS).wait();
    auto end = std::chrono::high_resolution_clock::now();

    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    size_t total = NUM_LAYERS * NUM_TASKS_PER_LAYER;

    std::cout << "=== [TF] Full-Connected ===" << std::endl;
    std::cout << "Total time: " << ns / 1e6 << " ms" << std::endl;
    std::cout << "Avg per run: " << ns / NUM_ITERATIONS << " ns" << std::endl;
    std::cout << "Avg per task: " << static_cast<double>(ns) / (total * NUM_ITERATIONS) << " ns" << std::endl;
    std::cout << "Counter: " << counter.load() << " (expected: " << total * NUM_ITERATIONS << ")\n" << std::endl;
}

void tf_no_connection() {
    tf::Executor executor(NUM_THREADS);
    tf::Taskflow taskflow;
    std::atomic<int> counter{0};

    for (size_t layer = 0; layer < NUM_LAYERS; ++layer)
        for (size_t i = 0; i < NUM_TASKS_PER_LAYER; ++i)
            taskflow.emplace([&]() {
                counter.fetch_add(1, std::memory_order_relaxed);
            });

    counter.store(0);
    auto start = std::chrono::high_resolution_clock::now();
    executor.run_n(taskflow, NUM_ITERATIONS).wait();
    auto end = std::chrono::high_resolution_clock::now();

    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    size_t total = NUM_LAYERS * NUM_TASKS_PER_LAYER;

    std::cout << "=== [TF] No Connection (Pure Parallel) ===" << std::endl;
    std::cout << "Total time: " << ns / 1e6 << " ms" << std::endl;
    std::cout << "Avg per run: " << ns / NUM_ITERATIONS << " ns" << std::endl;
    std::cout << "Avg per task: " << static_cast<double>(ns) / (total * NUM_ITERATIONS) << " ns" << std::endl;
    std::cout << "Counter: " << counter.load() << " (expected: " << total * NUM_ITERATIONS << ")\n" << std::endl;
}

int main() {
    tfl_full_connected();
    tfl_no_connection();
    tf_full_connected();
    tf_no_connection();
    return 0;
}
