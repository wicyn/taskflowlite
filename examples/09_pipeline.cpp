#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <vector>
#include <atomic>
#include <thread>

int main() {
    std::cout << "=== Example 09: Pipeline (Data Processing) ===\n";
    
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    
    std::atomic<int> processed{0};
    
    std::vector<int> data(12);
    for (int i = 0; i < 12; ++i) data[i] = i + 1;
    
    constexpr int batchSize = 4;
    
    tfl::Flow flow;
    
    auto split = flow.emplace([&data, &processed](tfl::Runtime& rt) {
        std::cout << "Split: Distributing data\n";
        for (size_t i = 0; i < data.size(); i += batchSize) {
            rt.silent_async([&data, i, &processed]() {
                int sum = 0;
                for (int j = 0; j < batchSize && (i + j) < static_cast<int>(data.size()); ++j) {
                    sum += data[i + j];
                }
                std::cout << "  Batch " << (i / batchSize) << ": sum = " << sum << "\n";
                processed.fetch_add(sum);
            });
        }
    });
    
    auto aggregate = flow.emplace([&processed] {
        std::cout << "Aggregate: Final result = " << processed.load() << "\n";
    });
    
    split.precede(aggregate);
    
    executor.submit(flow).start().wait();
    
    std::cout << "Pipeline example complete!\n";
    
    return 0;
}
