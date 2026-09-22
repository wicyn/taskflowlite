/// @file test_core_exception_contracts.cpp
/// @brief Regressions for result construction and the predicate overload actually invoked.
#include "test_common.hpp"
#include <stdexcept>

namespace {
struct ThrowingDefaultResult {
    ThrowingDefaultResult() { throw std::runtime_error("result constructor"); }
    explicit ThrowingDefaultResult(int) noexcept {}
    ThrowingDefaultResult(ThrowingDefaultResult&&) noexcept = default;
    ThrowingDefaultResult& operator=(ThrowingDefaultResult&&) noexcept = default;
};

struct OverloadedPredicate {
    bool operator()() const noexcept { return true; }
    bool operator()(tfl::TaskView) const { throw std::runtime_error("selected predicate"); }
};
} // namespace

TEST_CASE("Async result: throwing default constructor propagates without submitting",
          "[async][exception][core-regression]") {
    tfl::Executor executor(2);
    const auto callable = [] { return ThrowingDefaultResult(42); };
    REQUIRE_THROWS_AS(executor.async(callable), std::runtime_error);
    REQUIRE_THROWS_AS(executor.defer_async(callable), std::runtime_error);
    REQUIRE(executor.num_topologies() == 0);
    auto parent = executor.async([&](tfl::Runtime& rt) {
        int failures = 0;
        try { (void)rt.async(callable); } catch (const std::runtime_error&) { ++failures; }
        tfl::TaskGroup group(rt);
        try { (void)group.async(callable); } catch (const std::runtime_error&) { ++failures; }
        return failures;
    });
    REQUIRE(parent.get() == 2);
    REQUIRE(executor.async([] { return 42; }).get() == 42);
}

TEST_CASE("Branch and Jump: select_if propagates TaskView predicate exceptions",
          "[branch][jump][exception][core-regression]") {
    tfl::Executor executor(2);
    tfl::Flow flow;
    std::atomic<int> calls{0};
    auto target = flow.emplace([&] { ++calls; });
    SECTION("Branch") {
        auto branch = flow.emplace([](tfl::Branch& branch) { branch.select_if(OverloadedPredicate{}); });
        branch.precede(target);
    }
    SECTION("Jump") {
        auto jump = flow.emplace([](tfl::Jump& jump) { jump.select_if(OverloadedPredicate{}); });
        jump.precede(target);
    }
    REQUIRE_THROWS_AS(executor.async(flow).get(), std::runtime_error);
    REQUIRE(calls.load() == 0);
}
