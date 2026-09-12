/// @file async_task_allocation_failure.cpp
/// @brief 独立进程故障注入：延迟任务构造失败的捕获清理与重新创建。

#include <array>
#include <memory>
#include <cstdlib>
#include <iostream>
#include <new>

#include "../taskflowlite/taskflowlite.hpp"

namespace {

// 仅影响当前提交线程；工作线程及其它测试进程使用正常分配。
thread_local std::ptrdiff_t allocations_before_failure = -1;

struct FailAllocation {
    explicit FailAllocation(std::ptrdiff_t count) {
        allocations_before_failure = count;
    }
    ~FailAllocation() {
        allocations_before_failure = -1;
    }
};

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

} // namespace

#if !defined(TFL_SKIP_ALLOCATION_FAILURE)
void* operator new(std::size_t size) {
    if (allocations_before_failure == 0) {
        allocations_before_failure = -1;
        throw std::bad_alloc{};
    }
    if (allocations_before_failure > 0) --allocations_before_failure;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete[](void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}
#endif

int main() {
    tfl::Executor executor(2);
#if defined(TFL_SKIP_ALLOCATION_FAILURE)
    // MSVC ASan owns the process allocator; replacing global operator new would
    // bypass its quarantine and can deadlock during CRT/ASan shutdown. The
    // normal Release build below still exercises every injected failure point.
    auto task = executor.defer_async([] { return 42; });
    check(task.start().get() == 42, "ASan allocation probe failed");
    executor.wait_for_all();
    std::cout << "ASan allocation probe skipped global failure injection.\n";
    return 0;
#else
    int failures = 0, successes = 0;

    // 只在创建阶段注入失败；start 的分配失败回滚尚不属于当前 core 的保证。
    // 关闭任务池，使每次构造都实际经过分配；大捕获覆盖堆上 callable 存储。
    for (int fail_after = 0; fail_after < 8; ++fail_after) {
        auto token = std::make_shared<int>(42);
        try {
            FailAllocation fault(fail_after);
            auto task = executor.defer_async([token, padding = std::array<int, 256>{}] {
                return *token + padding[0];
            });
            check(!task.running() && !task.done(), "Deferred construction submitted a task");
            ++successes;
        } catch (const std::bad_alloc&) {
            ++failures;
        }
        check(token.use_count() == 1, "Construction leaked a callable capture");
        check(executor.num_topologies() == 0, "Construction leaked an active topology");
        auto retry = executor.defer_async([token] { return *token; });
        check(retry.start().get() == 42, "Creation retry failed");
        executor.wait_for_all();
    }
    check(failures > 0 && successes > 0, "Allocation sweep missed failure or success paths");
    std::cout << "Deferred task construction allocation checks passed.\n";
#endif
}
