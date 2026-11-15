/// @file test_iteration_count.cpp
/// @brief 重复次数的整数约束、按值捕获和图节点计数复位。

#include "test_common.hpp"
#include "object_task_fixtures.hpp"
#include <concepts>
#include <cstdint>
#include <tuple>

using tfl_test::objects::Module;

namespace {

enum LegacyCount { three = 3 };
struct ConvertibleCount {
    operator std::uint64_t() const { return 3; }
};

template <typename S, typename I, bool Accepted>
constexpr bool submission_counts =
    (requires(S& s, Module& g, I n) { s.silent_async(g, n); } == Accepted) &&
    (requires(S& s, Module& g, I n) { s.silent_async(g, n, tfl::noop_callback{}); } == Accepted) &&
    (requires(S& s, Module& g, I n, tfl::AsyncFuture<void>& dep) { s.async(g, n, dep); } == Accepted) &&
    (requires(S& s, Module& g, I n, tfl::AsyncFuture<void>& dep) {
        s.async(g, n, tfl::noop_callback{}, dep);
    } == Accepted);

template <typename I, bool Accepted>
consteval bool count_overloads() {
    static_assert(submission_counts<tfl::Executor, I, Accepted>);
    static_assert(submission_counts<tfl::Runtime, I, Accepted>);
    static_assert(submission_counts<tfl::TaskGroup, I, Accepted>);
    static_assert(requires(tfl::Flow& f, I n) { f.emplace_object<Module>(n); } == Accepted);
    static_assert(requires(tfl::Flow& f, I n) { f.emplace_object<Module>(std::tuple{}, n); } == Accepted);
    static_assert(requires(tfl::Task& t, Module& g, I n) { t.work(g, n); } == Accepted);
    static_assert(requires(tfl::Task& t, I n) { t.work_object<Module>(n); } == Accepted);
    static_assert(requires(tfl::Task& t, I n) { t.work_object<Module>(std::tuple{}, n); } == Accepted);
    static_assert(requires(tfl::Executor& e, Module& g, I n) { e.defer_async(g, n); } == Accepted);
    static_assert(requires(tfl::Executor& e, Module& g, I n) {
        e.defer_async(g, n, tfl::noop_callback{});
    } == Accepted);
    static_assert(requires(tfl::Executor& e, I n) { e.defer_async_object<Module>(n); } == Accepted);
    static_assert(requires(tfl::Executor& e, I n) {
        e.defer_async_object<Module>(std::tuple{}, n, tfl::noop_callback{});
    } == Accepted);
    static_assert(requires(tfl::Coro& c, Module& g, I n) { c.silent_fork(g, n); } == Accepted);
    static_assert(requires(tfl::Coro& c, Module& g, I n, tfl::AsyncFuture<void>& dep) {
        c.fork(g, n, dep);
    } == Accepted);
    static_assert(requires(tfl::Coro& c, Module& g, I n, tfl::AsyncFuture<void>& dep) {
        c.fork(g, n, tfl::noop_callback{}, dep);
    } == Accepted);
    return true;
}

static_assert(count_overloads<bool, true>());
static_assert(count_overloads<std::uint8_t, true>());
static_assert(count_overloads<int, true>());
static_assert(count_overloads<int&, true>());
static_assert(count_overloads<const int&, true>());
static_assert(count_overloads<std::uint64_t, true>());
static_assert(count_overloads<double, false>());
static_assert(count_overloads<LegacyCount, false>());
static_assert(count_overloads<ConvertibleCount, false>());

}  // namespace

TEMPLATE_TEST_CASE("Module counts: snapshot integer values and reset on each execution",
                   "[module][count]", bool, std::uint8_t, int, std::uint64_t) {
    tfl_test::TestEnv env(1);
    tfl::Flow outer;
    auto num = static_cast<TestType>(GENERATE(0, 3));
    const int expected = static_cast<int>(num);
    auto verify = [&](Module& module) {
        num = TestType{};
        env.executor.async(outer).get();
        REQUIRE(module.calls == expected);
        env.executor.async(outer).get();
        REQUIRE(module.calls == expected * 2);
    };

    SECTION("emplace borrowed module") {
        Module module;
        auto task = outer.emplace(module, num);
        static_assert(std::same_as<decltype(task), tfl::Task>);
        verify(module);
    }
    SECTION("emplace object") {
        verify(outer.emplace_object<Module>(num).object());
    }
    SECTION("emplace object with tuple") {
        verify(outer.emplace_object<Module>(std::tuple{}, num).object());
    }
    SECTION("replace borrowed module") {
        Module module;
        outer.placeholder().work(module, num);
        verify(module);
    }
    SECTION("replace object") {
        verify(outer.placeholder().work_object<Module>(num));
    }
    SECTION("replace object with tuple") {
        verify(outer.placeholder().work_object<Module>(std::tuple{}, num));
    }
}

TEMPLATE_TEST_CASE("Deferred counts: snapshot integer values before start",
                   "[async_task][count]", bool, std::uint8_t, int, std::uint64_t) {
    tfl_test::TestEnv env(1);
    auto num = static_cast<TestType>(GENERATE(0, 3));
    const int expected = static_cast<int>(num);
    int callbacks = 0;
    auto callback = [&] { ++callbacks; };
    auto verify = [&](auto task, Module& module) {
        num = TestType{};
        REQUIRE(module.calls == 0);
        task.start().get();
        REQUIRE(module.calls == expected);
        REQUIRE(callbacks == 1);
    };

    SECTION("borrowed module") {
        Module module;
        verify(env.executor.defer_async(module, num, callback), module);
    }
    SECTION("object") {
        auto task = env.executor.defer_async_object<Module>(num, callback);
        verify(task, task.object());
    }
    SECTION("object with tuple") {
        auto task = env.executor.defer_async_object<Module>(std::tuple{}, num, callback);
        verify(task, task.object());
    }
}

TEMPLATE_TEST_CASE("Coro counts: integer values select fork and silent fork overloads",
                   "[coro][count]", bool, std::uint8_t, int, std::uint64_t) {
    tfl_test::TestEnv env(1);
    Module module;
    const auto num = static_cast<TestType>(GENERATE(0, 3));
    int callbacks = 0;
    env.executor.async([&](tfl::Coro& coro) -> tfl::CoroTask<> {
        auto first = co_await coro.fork(module, num);
        co_await coro.join();
        first.get();
        auto second = co_await coro.fork(module, num, [&] { ++callbacks; }, first);
        co_await coro.join();
        second.get();
        co_await coro.silent_fork(module, num, [&] { ++callbacks; });
        co_await coro.join();
    }).get();
    REQUIRE(module.calls == static_cast<int>(num) * 3);
    REQUIRE(callbacks == 2);
}
