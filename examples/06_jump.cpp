#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <atomic>

int main() {
    std::cout << "=== Example 06: Jump (Retry Loop) ===\n";
    
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 2);
    tfl::Flow flow;
    
    std::atomic<int> attempts{0};
    constexpr int maxAttempts = 3;
    
    auto start = flow.emplace([&attempts] { 
        std::cout << "Starting operation (attempt " << attempts.load() + 1 << ")\n";
        attempts.fetch_add(1);
    });
    
    auto check = flow.emplace([&attempts](tfl::Jump jmp) {
        if (attempts.load() < maxAttempts) {
            std::cout << "Check failed, retrying...\n";
            jmp.to(0);
        } else {
            std::cout << "Check passed!\n";
        }
    });
    
    auto success = flow.emplace([] { std::cout << "Operation succeeded!\n"; });
    
    start.precede(check);
    check.precede(success);
    
    executor.submit(flow).start().wait();
    
    std::cout << "Total attempts: " << attempts.load() << "\n";
    std::cout << "Jump example complete!\n";
    
    return 0;
}
