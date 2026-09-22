/// @file core_failure_repro.cpp
/// @brief Opt-in defect repros. A fixed core should return 0; current defects fail or time out.
#include "../taskflowlite/taskflowlite.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
thread_local int fail_after = -1;
struct ThrowOnCopy {
    ThrowOnCopy() = default;
    ThrowOnCopy(const ThrowOnCopy&) { throw std::runtime_error("replacement construction"); }
    void operator()() const noexcept {}
};
}

// This target is never linked into Catch2 or an ASan binary. Only the submitting
// thread is faulted; the worker keeps running normally until the broken lock blocks it.
void* operator new(std::size_t size) {
    if (fail_after == 0) { fail_after = -1; throw std::bad_alloc{}; }
    if (fail_after > 0) --fail_after;
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2) return 2;
    if (std::strcmp(argv[1], "payload_replace") == 0) {
        tfl::Flow flow;
        auto token = std::make_shared<int>(42);
        auto task = flow.emplace([token] {});
        std::weak_ptr<int> old_callable = token;
        token.reset();
        ThrowOnCopy replacement;
        bool caught = false;
        try { task.work(replacement); } catch (const std::runtime_error&) { caught = true; }
        const bool preserved = !old_callable.expired();
        std::printf("caught=%d old_callable_preserved=%d\n", caught, preserved);
        // Restore a valid payload before any graph execution.
        task.work([] {});
        return caught && preserved ? 0 : 1;
    }

    tfl::Executor executor(2);
    if (std::strcmp(argv[1], "start_reserve") == 0) {
        auto predecessor = executor.async([] {});
        executor.wait_for_all();
        auto task = executor.defer_async([] {});
        bool caught = false;
        fail_after = 0; // First allocation is the deferred task's predecessor table.
        try { task.start(predecessor); } catch (const std::bad_alloc&) { caught = true; }
        fail_after = -1;
        std::printf("caught=%d running=%d topologies=%zu; retrying\n",
                    caught, task.running(), executor.num_topologies());
        if (!caught) return 3; // The intended fault was not exercised.
        task.start(predecessor).get(); // Broken core spins on the retained Idle lock.
        return 0;
    }
    if (std::strcmp(argv[1], "link_alloc") == 0) {
        std::atomic<bool> release{false};
        auto predecessor = executor.async([&] {
            while (!release.load()) std::this_thread::yield();
        });
        auto task = executor.defer_async([] {});
        bool caught = false;
        fail_after = 1; // Own edge table succeeds; predecessor successor table fails.
        try { task.start(predecessor); } catch (const std::bad_alloc&) { caught = true; }
        fail_after = -1;
        std::printf("caught=%d running=%d topologies=%zu; releasing predecessor\n",
                    caught, task.running(), executor.num_topologies());
        release = true;
        predecessor.get(); // Broken core cannot finish while LOCKED remains set.
        if (!caught) return 3;
        if (!task.running()) task.start(predecessor);
        task.get();
        executor.wait_for_all();
        return executor.num_topologies() == 0 ? 0 : 1;
    }
    return 2;
}
