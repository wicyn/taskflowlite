/// @brief External completion tokens and cancellation of a suspended coroutine.
#include <taskflowlite/taskflowlite.hpp>
#include <future>
#include <iostream>

// Registration publishes one token to the external completion source.
// A real I/O adapter must also synchronize cancellation and callback lifetime.
struct Completion {
    std::promise<tfl::CoroToken>& registered;
    bool await_ready() const noexcept { return false; }
    bool await_suspend(tfl::CoroToken token) {
        registered.set_value(std::move(token));
        return true;
    }
    void await_resume() const noexcept {}
};

int main() {
    std::promise<tfl::CoroToken> registered;
    auto notification = registered.get_future();
    tfl::Executor executor(1);
    auto task = executor.async([&](tfl::Coro& coro) -> tfl::CoroTask<bool> {
        co_await Completion{registered};
        co_return coro.stop_requested();
    });
    auto token = notification.get();
    // The only worker remains available while task is suspended.
    const int other = executor.async([] { return 42; }).get();
    const bool requested = task.request_stop();
    const bool observed = token.stop_requested();
    // Stop does not complete the external operation: its owner must still notify.
    const bool resumed = token.resume();
    const bool stopped = task.get();
    const bool expired = !token.resume() && token.stop_requested();
    std::cout << "Other work: " << other << ", stop observed: " << stopped << '\n';
    return other == 42 && requested && observed && resumed && stopped && expired ? 0 : 1;
}
