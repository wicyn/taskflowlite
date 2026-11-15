/// @brief Coroutine graph nodes and sequential child calls.
#include <taskflowlite/taskflowlite.hpp>
#include <iostream>

int main() {
    tfl::Executor executor(1);
    tfl::Flow flow;
    int result = 0;
    auto compute = flow.emplace([&](tfl::Coro& coro) -> tfl::CoroTask<> {
        // call owns the callable and injects the current Coro into the child.
        auto twice = coro.call([](tfl::Coro&) -> tfl::CoroTask<int> { co_return 21; });
        const int first = co_await twice;
        result = first + co_await twice; // Sequential reuse is supported.
    }).name("compute");
    auto print = flow.emplace([&] { std::cout << "Result: " << result << '\n'; });
    compute.precede(print);
    executor.async(flow).get();
    return result == 42 ? 0 : 1;
}
