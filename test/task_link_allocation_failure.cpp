/// @file task_link_allocation_failure.cpp
/// @brief 独立故障注入：跳过 DFS 分配、双向建边失败回滚及 Jump 自环。
#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include "../taskflowlite/taskflowlite.hpp"

namespace {
thread_local int allocations_before_failure = -1;
struct FailAllocation {
    explicit FailAllocation(int count) { allocations_before_failure = count; }
    ~FailAllocation() { allocations_before_failure = -1; }
};
void check(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
}

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
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
#endif

template <bool Check>
void verify_rollback() {
    // Jump 两端不运行 DFS，让第 0/1 次失败分别精确命中两端邻接表扩容。
    for (bool self_loop : {false, true}) {
        for (bool existing_predecessor : {false, true}) {
            for (int fail_after : {0, 1}) {
                tfl::Flow flow;
                auto before = flow.placeholder();
                auto source = flow.emplace([](tfl::Jump&) {});
                auto target = self_loop ? source : flow.placeholder();
                if (existing_predecessor) before.precede(source);
                bool failed = false;
                try {
                    FailAllocation fault(fail_after);
                    source.template precede<Check>(target);
                } catch (const std::bad_alloc&) { failed = true; }
                check(failed, "Did not reach the requested adjacency allocation failure");
                check(source.num_successors() == 0, "Failure left a half-inserted successor");
                check(source.num_predecessors() == (existing_predecessor ? 1u : 0u), "Failure changed existing predecessors");
                if (!self_loop) check(target.num_predecessors() == 0, "Failure left a half-inserted predecessor");
                source.template precede<Check>(target);
                check(source.num_successors() == 1, "Retry failed to insert the successor");
                source.remove_successor(target);
                if (existing_predecessor) {
                    check(before.num_successors() == 1, "Rollback changed an existing edge");
                    source.for_each_predecessor([&](tfl::Task task) { check(task == before, "Rollback corrupted predecessor identity"); });
                }
            }
        }
    }
}

int main() {
#if defined(TFL_SKIP_ALLOCATION_FAILURE)
    // MSVC ASan 拥有全局分配器，故障注入由普通 Release 构建执行。
    std::cout << "ASan: global allocator replacement skipped.\n";
#else
    verify_rollback<true>();
    verify_rollback<false>();
    for (int entry = 0; entry < 7; ++entry) {
        tfl::Flow flow;
        auto a = flow.placeholder(), b = flow.placeholder();
        // 保留两端容量，以便检测校验路径是否仍偷偷分配 DFS 临时容器。
        a.precede(b);
        a.remove_successor(b);
        try {
            FailAllocation fault(0);
            switch (entry) {
            case 0: a.precede<false>(b); break;
            case 1: b.succeed<false>(a); break;
            case 2: (void)tfl::Task{a}.precede<false>(b); break;
            case 3: (void)tfl::Task{b}.succeed<false>(a); break;
            case 4: flow.linearize<false>(a, b); break;
            case 5: flow.linearize<false>({a, b}); break;
            case 6: flow.linearize<false>(std::array{a, b}); break;
            }
        } catch (...) { check(false, "Unchecked entry still allocated despite reserved adjacency capacity"); }
        check(a.num_successors() == 1 && b.num_predecessors() == 1, "Unchecked entry did not link both ends");
    }
    std::cout << "Task link allocation and rollback checks passed.\n";
#endif
}
