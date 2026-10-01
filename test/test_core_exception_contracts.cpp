/// @file test_core_exception_contracts.cpp
/// @brief 验证任务对象替换失败、异步结果构造失败及谓词重载的异常传播。
#include "test_common.hpp"
#include <atomic>
#include <stdexcept>

namespace {
struct ThrowingDefaultResult {
    ThrowingDefaultResult() {
        throw std::runtime_error("result constructor");
    }
    explicit ThrowingDefaultResult(int) noexcept {}
    ThrowingDefaultResult(ThrowingDefaultResult&&) noexcept = default;
    ThrowingDefaultResult& operator=(ThrowingDefaultResult&&) noexcept = default;
};

struct OverloadedPredicate {
    bool operator()() const noexcept {
        return true;
    }
    bool operator()(tfl::TaskView) const {
        throw std::runtime_error("selected predicate");
    }
};

struct Replacement {
    explicit Replacement(bool fail) {
        if (fail) throw std::runtime_error("replacement");
    }
    Replacement(const Replacement&) = delete;
    Replacement(Replacement&&) = delete;
    void operator()() const noexcept {}
};
} // namespace

TEST_CASE("Task: failed object replacement resets callable and preserves graph", "[task][exception][core-regression]") {
    tfl::Executor executor(2);
    tfl::Flow     flow;
    int           calls = 0;
    auto          task = flow.emplace([&] { ++calls; });
    auto          next = flow.emplace([&] { ++calls; });
    task.precede(next);

    // 替换失败时抛出异常并重置 callable，图连接保持不变。
    REQUIRE_THROWS_AS(task.work_object<Replacement>(true), std::runtime_error);
    REQUIRE(task.num_successors() == 1);
    REQUIRE(next.num_predecessors() == 1);
    REQUIRE(calls == 0);

    // 重置后重新原地构造不可复制、不可移动的 callable。
    REQUIRE_NOTHROW(task.work_object<Replacement>(false));
    REQUIRE(task.type() == tfl::TaskType::Basic);
    REQUIRE(task.num_successors() == 1);
    REQUIRE(next.num_predecessors() == 1);

    // Replacement 不增加 calls，只有后继任务增加一次。
    executor.corun(flow);
    REQUIRE(calls == 1);
}

TEST_CASE("Async result: throwing default constructor propagates without submitting", "[async][exception][core-regression]") {
    tfl::Executor executor(2);
    const auto    callable = [] { return ThrowingDefaultResult(42); };

    // 结果槽默认构造失败时直接抛出异常，不提交任务。
    REQUIRE_THROWS_AS(executor.async(callable), std::runtime_error);
    REQUIRE_THROWS_AS(executor.defer_async(callable), std::runtime_error);
    REQUIRE(executor.num_topologies() == 0);

    // Runtime 和 TaskGroup 的异步创建接口同样传播构造异常。
    auto parent = executor.async([&](tfl::Runtime& rt) {
        int failures = 0;
        try {
            (void)rt.async(callable);
        } catch (const std::runtime_error&) {
            ++failures;
        }
        tfl::TaskGroup group(rt);
        try {
            (void)group.async(callable);
        } catch (const std::runtime_error&) {
            ++failures;
        }
        return failures;
    });

    REQUIRE(parent.get() == 2);
    REQUIRE(executor.async([] { return 42; }).get() == 42);
}

TEST_CASE("Branch and Jump: select_if propagates TaskView predicate exceptions", "[branch][jump][exception][core-regression]") {
    tfl::Executor    executor(2);
    tfl::Flow        flow;
    std::atomic<int> calls{0};
    auto             target = flow.emplace([&] { ++calls; });

    SECTION("Branch") {
        auto branch = flow.emplace([](tfl::Branch& branch) { branch.select_if(OverloadedPredicate{}); });
        branch.precede(target);
    }

    SECTION("Jump") {
        auto jump = flow.emplace([](tfl::Jump& jump) { jump.select_if(OverloadedPredicate{}); });
        jump.precede(target);
    }

    // 实际调用 TaskView 重载，其异常通过 get() 传播，目标任务不执行。
    REQUIRE_THROWS_AS(executor.async(flow).get(), std::runtime_error);
    REQUIRE(calls.load() == 0);
}