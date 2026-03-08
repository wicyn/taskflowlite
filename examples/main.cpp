#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <thread>

int main() {
    // Create a handler that does nothing special
    tfl::ResumeNever handler;
    
    // Create executor with 4 worker threads
    tfl::Executor executor(handler, 4);
    
    // Create a flow
    tfl::Flow flow;
    
    // Add tasks to the flow
    auto [A, B, C, D] = flow.emplace(
        [] { std::cout << "Task A (Init)\n"; },
        [] { std::cout << "Task B (Process 1)\n"; },
        [] { std::cout << "Task C (Process 2)\n"; },
        [] { std::cout << "Task D (Merge)\n"; }
    );
    
    // Define DAG: A runs first, then B and C in parallel, D runs after both B and C
    A.precede(B, C);
    D.succeed(B, C);
    
    // Submit the flow, start execution, and wait for completion
    executor.submit(flow).start().wait();
    
    std::cout << "All tasks completed!\n";
    
    return 0;
}
