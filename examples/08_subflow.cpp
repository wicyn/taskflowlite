#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <atomic>

int main() {
    std::cout << "=== Example 08: Subflow (Nested Flow) ===\n";
    
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    tfl::Flow flow;
    
    std::atomic<int> counter{0};
    
    flow.name("MainFlow");
    
    auto start = flow.emplace([&counter] { 
        std::cout << "Main: Starting\n";
        counter.fetch_add(1);
    });
    
    tfl::Flow subflow;
    subflow.name("SubFlow");
    subflow.emplace([&counter] { 
        std::cout << "  Sub: Step A\n";
        counter.fetch_add(1);
    });
    subflow.emplace([&counter] { 
        std::cout << "  Sub: Step B\n";
        counter.fetch_add(1);
    });
    subflow.emplace([&counter] { 
        std::cout << "  Sub: Step C\n";
        counter.fetch_add(1);
    });
    
    auto subflowTask = flow.emplace(subflow);
    
    auto end = flow.emplace([&counter] { 
        std::cout << "Main: Finished\n";
        counter.fetch_add(1);
    });
    
    start.precede(subflowTask);
    subflowTask.precede(end);
    
    executor.submit(flow).start().wait();
    
    std::cout << "Total tasks executed: " << counter.load() << "\n";
    std::cout << "Subflow example complete!\n";
    
    return 0;
}
