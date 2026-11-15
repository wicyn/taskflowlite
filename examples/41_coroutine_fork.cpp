/// @brief Coroutine child work, completion dependencies and cooperative join.
#include <taskflowlite/taskflowlite.hpp>
#include <iostream>

int main() {
    tfl::Executor executor(1);
    auto result = executor.async([](tfl::Coro& coro) -> tfl::CoroTask<int> {
        auto left = co_await coro.fork([] { return 20; });
        auto right = co_await coro.fork([](tfl::Coro&) -> tfl::CoroTask<int> { co_return 22; });
        auto sum = co_await coro.fork([left, right] { return left.get() + right.get(); }, left, right);
        // fork returns a Future after submission, before completion is guaranteed.
        co_await coro.join();
        co_return sum.get(); // Read results and propagate errors after joining.
    });
    const int value = result.get();
    std::cout << "Fork/join result: " << value << '\n';
    return value == 42 ? 0 : 1;
}
