#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>

int main() {
    std::cout << "=== Example 01: Basic DAG ===\n";
    
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    tfl::Flow flow;
    
    auto [A, B, C, D] = flow.emplace(
        [] { std::cout << "Task A (Init)\n"; },
        [] { std::cout << "Task B (Process 1)\n"; },
        [] { std::cout << "Task C (Process 2)\n"; },
        [] { std::cout << "Task D (Merge)\n"; }
    );
    
    A.precede(B, C);
    D.succeed(B, C);
    
    executor.submit(flow).start().wait();
    
    std::cout << "All tasks completed!\n";
    
    return 0;
}
