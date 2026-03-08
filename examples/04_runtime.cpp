#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <future>

int main() {
    std::cout << "=== Example 04: Runtime (Dynamic Dispatch) ===\n";
    
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    tfl::Flow flow;
    
    flow.emplace([](tfl::Runtime& rt) {
        std::cout << "Runtime task: spawning async tasks\n";
        
        auto fut1 = rt.async([] { 
            std::cout << "Async task 1 running\n";
            return 1; 
        });
        
        auto fut2 = rt.async([] { 
            std::cout << "Async task 2 running\n";
            return 2; 
        });
        
        rt.wait_until([&] { 
            return fut1.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
                   fut2.wait_for(std::chrono::seconds(0)) == std::future_status::ready; 
        });
        
        std::cout << "Results: " << fut1.get() << ", " << fut2.get() << "\n";
    });
    
    executor.submit(flow).start().wait();
    
    std::cout << "Runtime example complete!\n";
    
    return 0;
}
