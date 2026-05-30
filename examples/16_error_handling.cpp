/// @file 16_error_handling.cpp
/// @brief 演示异常处理：Future::get 传播异常、异常归档、ResumeAlways vs ResumeNever。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    std::cout << "=== Example 16: Error Handling ===\n\n";

    // ================================================================
    // Part 1: Future::get() 传播异常
    // ================================================================
    {
        std::cout << "--- Part 1: Future exception propagation ---\n";

        tfl::ResumeNever handler;
        tfl::Executor executor(handler, 4);

        auto fut = executor.async([]() -> int {
            throw std::runtime_error("Something went wrong in async task!");
            return 42;
            });

        try {
            int result = fut.get();  // exception rethrown here
            std::cout << "Result: " << result << "\n";  // never executed
        }
        catch (const std::exception& e) {
            std::cout << "Caught via Future::get(): " << e.what() << "\n";
        }

        executor.wait_for_all();
    }

    // ================================================================
    // Part 2: 图内异常传播与归档
    // ================================================================
    {
        std::cout << "\n--- Part 2: In-graph exception archiving ---\n";

        tfl::ResumeAlways handler;  // single task exception does not terminate worker
        tfl::Executor executor(handler, 4);

        tfl::Flow flow;
        flow.name("Exception_Flow");

        auto good = flow.emplace([] {
            std::cout << "  [Good] Running normally\n";
            }).name("good");

        auto bad = flow.emplace([] {
            std::cout << "  [Bad] About to throw...\n";
            throw std::logic_error("Invalid state in bad task!");
            }).name("bad");

        // This task will be skipped because bad's exception marks the chain
        auto downstream = flow.emplace([] {
            std::cout << "  [Downstream] Should NOT run after exception\n";
            }).name("downstream");

        auto catcher = flow.emplace([](tfl::Runtime& rt) {
            // Runtime node is an implicit anchor — exception archived here
            std::cout << "  [Catcher] Runtime anchor — "
                << (rt.has_exception() ? "exception detected" : "no exception")
                << "\n";
            }).name("catcher");

        good.precede(bad);
        bad.precede(downstream);   // bad throws, downstream skipped
        good.precede(catcher);     // catcher still runs
        bad.precede(catcher);      // exception propagates to catcher

        executor.detach(flow);
        executor.wait_for_all();

        std::cout << "  Graph completed (some tasks skipped due to exception)\n";
    }

    // ================================================================
    // Part 3: AsyncTask 链中的异常处理
    // ================================================================
    {
        std::cout << "\n--- Part 3: AsyncTask chain exception ---\n";

        tfl::ResumeAlways handler;
        tfl::Executor executor(handler, 4);

        bool downstream_ran = false;
        auto t1 = tfl::NonrepeatAsyncTask([] {
            throw std::runtime_error("Task 1 failed");
            });

        auto t2 = tfl::NonrepeatAsyncTask([&downstream_ran] {
            downstream_ran = true;
            std::cout << "  Task 2 running\n";
            });

        executor.submit(t1);       // submit t1 first (root, no deps)
        executor.submit(t2, t1);  // t2 depends on t1

        t2.wait();
        executor.wait_for_all();

        std::cout << "  Downstream ran: " << (downstream_ran ? "yes" : "no (skipped)") << "\n";
    }

    // ================================================================
    // Part 4: detach 中的异常 — 静默消费
    // ================================================================
    {
        std::cout << "\n--- Part 4: detach exception (silently consumed) ---\n";

        tfl::ResumeAlways handler;
        tfl::Executor executor(handler, 4);

        executor.detach([] {
            std::cout << "  [detach] This task throws but executor continues\n";
            throw std::runtime_error("silent failure");
            });

        executor.detach([] {
            std::cout << "  [detach] Next task still runs normally\n";
            });

        executor.wait_for_all();
        std::cout << "  Executor continues after exception (ResumeAlways)\n";
    }

    std::cout << "\n=== Error Handling Complete ===\n";
    return 0;
}