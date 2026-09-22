/// @file 34_dependency_errors.cpp
/// @brief Invalid-dependency retry, cleanup after failure and explicit error propagation.
#include "../taskflowlite/taskflowlite.hpp"
#include <atomic>
#include <iostream>
#include <stdexcept>

int main() {
    tfl::Executor executor(2);
    auto source = executor.defer_async([]() -> int { throw std::runtime_error("source failed"); });
    std::atomic<bool> cleaned{false};
    auto cleanup = executor.defer_async([&] { cleaned = true; });
    bool rejected = false;
    try { cleanup.start(source); }
    catch (const tfl::Exception&) { rejected = true; }
    if (!rejected || cleanup.running()) return 1;

    source.start();
    // Validation errors leave cleanup idle and retryable. This does not imply
    // allocation failure during start() can be recovered by the current core.
    cleanup.start(source);
    cleanup.get(); // A completion dependency does not automatically propagate errors.
    auto propagated = executor.async([source] { return source.get(); }, source);
    bool caught = false;
    try { (void)propagated.get(); }
    catch (const std::runtime_error& error) {
        caught = true;
        std::cout << "Explicitly propagated: " << error.what() << '\n';
    }
    executor.wait_for_all();
    return cleaned && caught && executor.num_topologies() == 0 ? 0 : 1;
}
