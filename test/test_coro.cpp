/// @file test_coro.cpp
/// @brief Coroutine submission, structured children, results and frame lifetime.
#include "test_common.hpp"

#include <array>
#include <type_traits>

namespace {

struct CoroObject {
    int value;
    explicit CoroObject(int n) : value(n) {}
    CoroObject(const CoroObject&) = delete;
    CoroObject(CoroObject&&) = delete;
    tfl::CoroTask<int> operator()(tfl::Coro&) { co_return ++value; }
};

tfl::CoroTask<int> argument_coroutine(int base, const tfl::Coro&, std::unique_ptr<int> value, tfl::Coro& coro) {
    // Lookup must skip the const context and leave the owned parameter intact.
    const int offset = co_await coro.call([base](tfl::Coro&) -> tfl::CoroTask<int> { co_return base; });
    co_return offset + *value;
}

struct MemberCoroutine {
    int base{20};
    tfl::CoroTask<int> compute(std::unique_ptr<int> value, tfl::Coro& coro) const {
        const int offset = co_await coro.call([this](tfl::Coro&) -> tfl::CoroTask<int> { co_return base; });
        co_return offset + *value;
    }
};

struct FrameGuard {
    int& alive;
    explicit FrameGuard(int& n) : alive(n) { ++alive; }
    ~FrameGuard() { --alive; }
};

tfl::CoroTask<int> recurse(tfl::Coro& coro, int depth, int& alive) {
    FrameGuard guard(alive);
    // Keep data live across suspension and force the Stacklet expansion path.
    std::array<int, 2048> data{};
    data.front() = depth;
    data.back() = depth + 1;
    if (depth == 0) co_return data.back();
    const int child = co_await coro.call([&](tfl::Coro& nested) {
        return recurse(nested, depth - 1, alive);
    });
    co_return child + data.back() - data.front();
}

} // namespace

TEST_CASE("Coro: callable concepts and non-owning public surface", "[coro][concepts]") {
    STATIC_REQUIRE(tfl::coro_task<tfl::CoroTask<int>>);
    STATIC_REQUIRE(tfl::coro_task<const tfl::CoroTask<int>&>);
    STATIC_REQUIRE_FALSE(tfl::coro_task<tfl::AsyncFuture<int>>);
    STATIC_REQUIRE(tfl::coro_invocable<CoroObject>);
    STATIC_REQUIRE(std::same_as<tfl::coro_result_t<CoroObject>, int>);
    STATIC_REQUIRE_FALSE(tfl::basic_invocable<CoroObject>);
    STATIC_REQUIRE_FALSE(tfl::runtime_invocable<CoroObject>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<tfl::CoroTask<>>);
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<tfl::CoroTask<>>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<tfl::CoroTask<>>);
    STATIC_REQUIRE(std::is_base_of_v<tfl::Context, tfl::Coro>);
}

TEST_CASE("Coro: captured lambdas bind their promise context", "[coro][promise]") {
    tfl::Executor executor(GENERATE(1, 4));
    // GCC 13.3 used to reject the promise constructor's forwarding reference
    // for the implicit object argument of both root and nested coroutines.
    auto constant = executor.async([base = 20](tfl::Coro& coro) -> tfl::CoroTask<int> {
        const int offset = co_await coro.call([value = 22](tfl::Coro&) -> tfl::CoroTask<int> {
            co_return value;
        });
        co_return base + offset;
    });
    REQUIRE(constant.get() == 42);
    auto mutable_capture = executor.async([value = 40](tfl::Coro& coro) mutable -> tfl::CoroTask<int> {
        value += co_await coro.call([increment = 1](tfl::Coro&) mutable -> tfl::CoroTask<int> {
            co_return ++increment;
        });
        co_return value;
    });
    REQUIRE(mutable_capture.get() == 42);
}

TEST_CASE("Coro: promise lookup preserves free and member coroutine arguments", "[coro][promise]") {
    tfl::Executor executor(GENERATE(1, 4));
    auto free_function = executor.async([](tfl::Coro& coro) {
        return argument_coroutine(20, std::as_const(coro), std::make_unique<int>(22), coro);
    });
    REQUIRE(free_function.get() == 42);
    const MemberCoroutine object;
    auto member_function = executor.async([&object](tfl::Coro& coro) {
        return object.compute(std::make_unique<int>(22), coro);
    });
    REQUIRE(member_function.get() == 42);
}

TEST_CASE("Coro: graph nodes repeat and preserve dependencies", "[coro][flow]") {
    tfl::Executor executor(GENERATE(1, 4));
    tfl::Flow flow;
    int value = 0;
    auto first = flow.emplace([&] { ++value; });
    auto middle = flow.emplace([&](tfl::Coro& coro) -> tfl::CoroTask<> {
        co_await coro.silent_fork([&] { ++value; });
        co_await coro.join();
    }).name("coroutine");
    auto last = flow.emplace([&] { ++value; });
    first.precede(middle);
    middle.precede(last);
    REQUIRE(middle.type() == tfl::TaskType::Coroutine);
    REQUIRE(tfl::to_string_view(middle.type()) == "coroutine");
    executor.async(flow, 20).get();
    REQUIRE(value == 60);
}

TEST_CASE("Coro: async and deferred results retain value and reference types", "[coro][async]") {
    tfl::Executor executor(1);
    auto value = executor.async([](tfl::Coro&) -> tfl::CoroTask<std::unique_ptr<int>> {
        co_return std::make_unique<int>(42);
    });
    REQUIRE(*value.get() == 42);
    REQUIRE(*value.get() == 42);
    int number = 7;
    auto reference = executor.defer_async([&](tfl::Coro&) -> tfl::CoroTask<int&> {
        co_return number;
    });
    REQUIRE_FALSE(reference.running());
    REQUIRE_FALSE(reference.done());
    reference.start();
    REQUIRE(&reference.get() == &number);
    reference.get() = 9;
    REQUIRE(number == 9);
    std::atomic<int> count{0};
    executor.silent_async([&](tfl::Coro&) -> tfl::CoroTask<> {
        ++count;
        co_return;
    });
    executor.wait_for_all();
    REQUIRE(count == 1);
    REQUIRE(executor.num_topologies() == 0);
}

TEST_CASE("Coro: Runtime TaskGroup and object tasks accept coroutine callables", "[coro][object][runtime]") {
    tfl::Executor executor(1);
    auto result = executor.async([](tfl::Runtime& rt) {
        auto a = rt.async([](tfl::Coro&) -> tfl::CoroTask<int> { co_return 20; });
        tfl::TaskGroup group(rt);
        auto b = group.async([](tfl::Coro&) -> tfl::CoroTask<int> { co_return 22; }, a);
        group.cowait();
        rt.cowait();
        return a.get() + b.get();
    });
    REQUIRE(result.get() == 42);
    tfl::Flow flow;
    auto object = flow.emplace_object<CoroObject>(0);
    executor.async(flow, 3).get();
    REQUIRE(object.object().value == 3);
    auto deferred = executor.defer_async_object<CoroObject>(41);
    REQUIRE(deferred.start().get() == 42);
}

TEST_CASE("Coro: call reuses an operation and destroys nested frames in order", "[coro][lifetime]") {
    tfl::Executor executor(GENERATE(1, 4));
    int alive = 0;
    auto result = executor.async([&](tfl::Coro& coro) -> tfl::CoroTask<int> {
        auto operation = coro.call([&](tfl::Coro& nested) { return recurse(nested, 24, alive); });
        const int first = co_await operation;
        if (alive != 0) throw std::runtime_error("child frames retained after call");
        const int second = co_await operation;
        co_return first + second;
    });
    REQUIRE(result.get() == 50);
    REQUIRE(alive == 0);
}

TEST_CASE("Coro: call transfers move-only and reference results", "[coro][result]") {
    tfl::Executor executor(1);
    int number = 20;
    auto result = executor.async([&](tfl::Coro& coro) -> tfl::CoroTask<int> {
        auto value = co_await coro.call([](tfl::Coro&) -> tfl::CoroTask<std::unique_ptr<int>> {
            co_return std::make_unique<int>(22);
        });
        int& reference = co_await coro.call([&](tfl::Coro&) -> tfl::CoroTask<int&> {
            co_return number;
        });
        reference += *value;
        co_return reference;
    });
    REQUIRE(result.get() == 42);
    REQUIRE(number == 42);
}

TEST_CASE("Coro: child exceptions unwind call frames and permit recovery", "[coro][exception]") {
    tfl::Executor executor(1);
    int alive = 0;
    auto result = executor.async([&](tfl::Coro& coro) -> tfl::CoroTask<int> {
        try {
            co_await coro.call([&](tfl::Coro&) -> tfl::CoroTask<> {
                FrameGuard guard(alive);
                throw std::runtime_error("child error");
                co_return;
            });
        } catch (const std::runtime_error&) {
            if (alive != 0) throw;
        }
        co_return co_await coro.call([](tfl::Coro&) -> tfl::CoroTask<int> { co_return 42; });
    });
    REQUIRE(result.get() == 42);
    REQUIRE(alive == 0);
    auto failed = executor.async([](tfl::Coro&) -> tfl::CoroTask<> {
        throw std::runtime_error("root error");
        co_return;
    });
    failed.wait();
    REQUIRE_THROWS_WITH(failed.get(), "root error");
}

TEST_CASE("Coro: fork dependencies join and graph overloads", "[coro][fork]") {
    tfl::Executor executor(GENERATE(1, 4));
    std::atomic<int> count{0};
    tfl::Flow graph;
    graph.emplace([&] { ++count; });
    auto result = executor.async([&](tfl::Coro& coro) -> tfl::CoroTask<int> {
        co_await coro.join(); // Joining an empty scope is valid.
        auto first = co_await coro.fork([](tfl::Coro&) -> tfl::CoroTask<int> { co_return 20; });
        tfl::AsyncFuture<void> empty;
        auto last = co_await coro.fork([first] { return first.get() + 22; }, first, first, empty);
        auto module = co_await coro.fork(graph, 3, [&] { ++count; }, last);
        co_await coro.silent_fork([](tfl::Runtime& rt) { rt.silent_async([] {}); });
        co_await coro.silent_fork([](tfl::SubFlow& sf) { sf.emplace([] {}); sf.run(); });
        co_await coro.join();
        module.get();
        auto completed = co_await coro.fork([] { return 1; }, last);
        co_await coro.join();
        co_return last.get() + completed.get();
    });
    REQUIRE(result.get() == 43);
    REQUIRE(count == 4);
}

TEST_CASE("Coro: fork rejects idle dependencies without submitting the child", "[coro][fork][exception]") {
    tfl::Executor executor(1);
    auto idle = executor.defer_async([] {});
    // AsyncTask currently hides m_work from Coro; use its public Future base.
    tfl::AsyncFuture<void> dependency = idle;
    std::atomic<int> count{0};
    auto parent = executor.async([&](tfl::Coro& coro) -> tfl::CoroTask<> {
        auto child = co_await coro.fork([&] { ++count; }, dependency);
        co_await coro.join();
        child.get();
    });
    REQUIRE_THROWS_AS(parent.get(), tfl::Exception);
    REQUIRE(count == 0);
    idle.start().get();
    executor.wait_for_all();
    REQUIRE(executor.num_topologies() == 0);
}

TEST_CASE("Coro: a pending fork dependency lets the parent continue", "[coro][fork]") {
    tfl::Executor executor(GENERATE(1, 4));
    tfl::Semaphore gate(1, 0);
    auto predecessor = executor.defer_async([] { return 20; }).acquire(gate).start();
    tfl::AsyncFuture<int> dependency = predecessor;
    auto parent = executor.async([&](tfl::Coro& coro) -> tfl::CoroTask<int> {
        auto child = co_await coro.fork([dependency] { return dependency.get() + 22; }, dependency);
        // No child can finish until this statement executes after fork's await.
        executor.defer_async([] {}).release(gate).start();
        co_await coro.join();
        co_return child.get();
    });
    REQUIRE(parent.get() == 42);
}

TEST_CASE("Coro: join waits but future get propagates a fork error", "[coro][fork][exception]") {
    tfl::Executor executor(GENERATE(1, 4));
    bool joined = false;
    auto parent = executor.async([&](tfl::Coro& coro) -> tfl::CoroTask<> {
        auto child = co_await coro.fork([] { throw std::runtime_error("fork error"); });
        co_await coro.join();
        joined = true;
        child.get();
    });
    REQUIRE_THROWS_WITH(parent.get(), "fork error");
    REQUIRE(joined);
}

TEST_CASE("Coro: join rethrows scope errors and permits recovery", "[coro][fork][join][exception]") {
    tfl::Executor executor(GENERATE(1, 4));
    int caught = 0;
    auto parent = executor.async([&](tfl::Coro& coro) -> tfl::CoroTask<int> {
        for (int round = 0; round < 2; ++round) {
            co_await coro.silent_fork([] { throw std::runtime_error("silent fork error"); });
            try {
                co_await coro.join();
            } catch (const std::runtime_error& error) {
                if (std::string_view(error.what()) != "silent fork error") throw;
                ++caught;
            }
            // The previous error was consumed, and an empty join remains valid.
            co_await coro.join();
        }

        auto child = co_await coro.fork([] { return 42; });
        co_await coro.join();
        co_return child.get();
    });
    REQUIRE(parent.get() == 42);
    REQUIRE(caught == 2);
    executor.wait_for_all();
    REQUIRE(executor.num_topologies() == 0);
}

TEST_CASE("Coro: unawaited operations release captures without executing", "[coro][lifetime]") {
    tfl::Executor executor(1);
    auto owner = std::make_shared<int>(0);
    std::weak_ptr<int> weak = owner;
    auto parent = executor.async([&](tfl::Coro& coro) -> tfl::CoroTask<> {
        auto call = coro.call([owner](tfl::Coro&) -> tfl::CoroTask<> { ++*owner; co_return; });
        auto fork = coro.fork([owner] { ++*owner; });
        auto silent = coro.silent_fork([owner] { ++*owner; });
        owner.reset();
        co_return;
    });
    parent.get();
    REQUIRE(weak.expired());
}
