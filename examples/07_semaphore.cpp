#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>

int main() {
    std::cout << "=== Example 07: Semaphore (Concurrency Limit) ===\n";
    
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    
    tfl::Semaphore dbLimit(2);
    std::atomic<int> active{0};
    std::atomic<int> maxActive{0};
    
    std::mutex coutMutex;
    
    {
        tfl::Flow flow;
        
        for (int i = 0; i < 6; ++i) {
            auto task = flow.emplace([&dbLimit, &active, &maxActive, &coutMutex, i] {
                int current = active.fetch_add(1) + 1;
                int max = maxActive.load();
                while (!maxActive.compare_exchange_weak(max, std::max(max, current))) {}
                {
                    std::lock_guard lock(coutMutex);
                    std::cout << "Task " << i << " started (active: " << current << ")\n";
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                {
                    std::lock_guard lock(coutMutex);
                    std::cout << "Task " << i << " finished\n";
                }
                active.fetch_sub(1);
            });
            task.acquire(dbLimit).release(dbLimit);
        }
        
        executor.submit(flow).start().wait();
    }
    
    std::cout << "Max concurrent tasks: " << maxActive.load() << "\n";
    std::cout << "Semaphore example complete!\n";
    
    return 0;
}
