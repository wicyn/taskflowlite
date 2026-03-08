#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <atomic>

int main() {
    std::cout << "=== Example 02: Parallel Tasks ===\n";
    
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    tfl::Flow flow;
    
    std::atomic<int> counter{0};
    
    std::cout << "Starting parallel tasks...\n";
    
    auto [p1, p2, p3, p4] = flow.emplace(
        [&counter] { 
            std::cout << "Task 1 running\n"; 
            counter.fetch_add(1); 
        },
        [&counter] { 
            std::cout << "Task 2 running\n"; 
            counter.fetch_add(1); 
        },
        [&counter] { 
            std::cout << "Task 3 running\n"; 
            counter.fetch_add(1); 
        },
        [&counter] { 
            std::cout << "Task 4 running\n"; 
            counter.fetch_add(1); 
        }
    );
    
    executor.submit(flow).start().wait();
    
    std::cout << "Executed " << counter.load() << " parallel tasks\n";
    
    return 0;
}
