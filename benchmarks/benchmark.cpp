#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <atomic>
#include <chrono>
#include <print>

int main() {
    constexpr std::size_t LAYERS    = 100;
    constexpr std::size_t PER_LAYER = 100;
    constexpr std::size_t THREADS   = 4;
    constexpr std::size_t ITERS     = 10;

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, THREADS);
    tfl::Flow flow;
    std::atomic<int> counter{0};

    std::cout << "Building DAG with " << LAYERS << " layers, "
              << PER_LAYER << " tasks per layer...\n";

    // Build a mesh DAG
    std::vector<std::vector<tfl::Task>> layers(LAYERS);
    for (std::size_t layer = 0; layer < LAYERS; ++layer) {
        layers[layer].reserve(PER_LAYER);
        for (std::size_t i = 0; i < PER_LAYER; ++i) {
            layers[layer].push_back(
                flow.emplace([&]{ counter.fetch_add(1, std::memory_order_relaxed); })
            );
        }
        if (layer > 0) {
            for (auto& prev : layers[layer - 1])
                for (auto& curr : layers[layer])
                    prev.precede(curr); // Fully connected
        }
    }

    std::cout << "Running benchmark with " << ITERS << " iterations...\n";

    executor.submit(flow, 1).start().wait();   // Warm-up

    counter.store(0);
    auto t0 = std::chrono::high_resolution_clock::now();
    executor.submit(flow, ITERS).start().wait(); // Timed execution
    auto t1 = std::chrono::high_resolution_clock::now();

    auto ns    = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    auto total = LAYERS * PER_LAYER * ITERS;
    std::printf("tasks: %d / %zu | total: %.2f ms | per-task: %ld ns\n",
           counter.load(), total, ns / 1e6, ns / (long)total);

    return 0;
}
