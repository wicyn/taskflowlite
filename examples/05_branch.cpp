#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <random>

int main() {
    std::cout << "=== Example 05: Branch (Conditional Execution) ===\n";
    
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 2);
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 1);
    
    for (int i = 0; i < 3; ++i) {
        tfl::Flow flow;
        
        int randomChoice = dis(gen);
        
        auto branch = flow.emplace([randomChoice](tfl::Branch br) {
            std::cout << "Branch decision: " << randomChoice << "\n";
            br.allow(randomChoice);
        });
        
        auto path0 = flow.emplace([] { std::cout << "Path 0 (even)\n"; });
        auto path1 = flow.emplace([] { std::cout << "Path 1 (odd)\n"; });
        
        branch.precede(path0, path1);
        
        std::cout << "Run #" << i + 1 << ": ";
        executor.submit(flow).start().wait();
    }
    
    std::cout << "Branch example complete!\n";
    
    return 0;
}
