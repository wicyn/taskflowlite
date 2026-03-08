#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <atomic>

int main() {
    std::cout << "=== Example 03: Loop Execution ===\n";
    
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    tfl::Flow flow;
    
    std::atomic<int> counter{0};
    constexpr int iterations = 5;
    
    flow.emplace([&counter] { 
        std::cout << "Iteration #" << counter.load() + 1 << "\n";
        counter.fetch_add(1); 
    });
    
    std::cout << "Running flow " << iterations << " times...\n";
    executor.submit(flow, iterations).start().wait();
    
    std::cout << "Total executions: " << counter.load() << "\n";
    
    return 0;
}
