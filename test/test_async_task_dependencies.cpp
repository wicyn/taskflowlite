/// @file test_async_task_dependencies.cpp
/// @brief defer_async / start 依赖校验、共享生命周期、作用域与并发回归。
#include "test_common.hpp"
#include <barrier>
#include <concepts>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using tfl_test::TestEnv;

namespace {
template <typename Dependency>
concept accepts_start = requires(tfl::AsyncTask<int>& task, Dependency&& dependency) {
    task.start(std::forward<Dependency>(dependency));
};
static_assert(accepts_start<tfl::AsyncTask<void>&>);
static_assert(accepts_start<const tfl::AsyncFuture<int&>&>);
static_assert(!accepts_start<int>);
static_assert(!std::constructible_from<tfl::AsyncTask<int>, decltype([] { return 42; })>);
} // namespace

TEST_CASE("AsyncTask: deferred creation binds executor without submitting", "[async][deps][lifecycle]") {
    TestEnv env(1);
    int first = 0, second = 0;
    auto a = env.executor.defer_async([&] { first = 20; });
    auto b = env.executor.defer_async([&] { second = 22; });
    auto c = env.executor.defer_async([&] { return first + second; });
    REQUIRE(env.executor.num_topologies() == 0);
    env.executor.wait_for_all();
    REQUIRE(first == 0);
    REQUIRE(second == 0);
    REQUIRE_FALSE(c.running());
    REQUIRE_FALSE(c.done());
    a.start();
    b.start();
    REQUIRE(&c.start(a, b) == &c);
    REQUIRE(c.get() == 42);
}

TEST_CASE("AsyncTask: invalid dependency rejection leaves task retryable", "[async][deps][error]") {
    TestEnv env;
    auto a = env.executor.defer_async([] { return 41; });
    auto c = env.executor.defer_async([a] { return a.get() + 1; });
    tfl::AsyncFuture<int> idle_future = a;
    tfl::AsyncTask<int> empty;
    REQUIRE_THROWS_AS(empty.start(), tfl::Exception);
    REQUIRE_THROWS_AS(c.start(a), tfl::Exception);
    REQUIRE_THROWS_AS(c.start(idle_future), tfl::Exception);
    REQUIRE_THROWS_AS(c.start(c), tfl::Exception);
    REQUIRE_THROWS_AS(std::move(c).start(a), tfl::Exception);
    REQUIRE(c);
    REQUIRE_FALSE(a.running());
    REQUIRE_FALSE(c.running());
    REQUIRE(env.executor.num_topologies() == 0);
    a.start().get();
    env.executor.wait_for_all();
    const auto refs = a.use_count();
    REQUIRE_THROWS_AS(c.start(a, c), tfl::Exception);
    REQUIRE(a.use_count() == refs);
    REQUIRE(c.start(a).get() == 42);
    REQUIRE_THROWS_AS(c.start(), tfl::Exception);
}

TEST_CASE("AsyncTask: empty dependencies are ignored", "[async][deps]") {
    TestEnv env;
    tfl::AsyncTask<void> empty_task;
    tfl::AsyncFuture<int> empty_future;
    auto a = env.executor.defer_async([] { return 41; }).start();
    auto c = env.executor.defer_async([a] { return a.get() + 1; });
    REQUIRE(c.start(empty_task, a, empty_future).get() == 42);
    REQUIRE(env.executor.defer_async([] { return 7; }).start(empty_task, empty_future).get() == 7);
}

TEST_CASE("AsyncTask: mixed results and rvalue handles retain predecessors", "[async][deps][result]") {
    TestEnv env;
    int number = 20;
    auto a = env.executor.defer_async([&]() -> int& { return number; }).start();
    const auto ready = env.executor.defer_async([] {}).start();
    const auto future = env.executor.async([] { return std::string("22"); });
    auto c = env.executor.defer_async([a, future, value = std::make_unique<int>(0)] {
        return a.get() + std::stoi(future.get()) + *value;
    });
    STATIC_REQUIRE(std::same_as<decltype(a), tfl::AsyncTask<int&>>);
    STATIC_REQUIRE(std::same_as<decltype(std::move(c).start(a)), tfl::AsyncTask<int>>);
    auto result = std::move(c).start(std::move(a), ready, future);
    REQUIRE_FALSE(c);
    REQUIRE(a); // 依赖不会因右值传入而被移动。
    REQUIRE(result.get() == 42);
    REQUIRE(&a.get() == &number);
}

TEST_CASE("AsyncTask: duplicates retain references until the last dependent handle resets", "[async][deps][refcount]") {
    TestEnv env;
    auto a = env.executor.defer_async([] {}).start();
    a.get();
    env.executor.wait_for_all();
    auto alias = a;
    tfl::AsyncFuture<void> future = a;
    const auto before = a.use_count();
    auto c = env.executor.defer_async([] { return 42; }).start(a, alias, future);
    c.get();
    env.executor.wait_for_all();
    REQUIRE(a.use_count() == before + 3);
    auto copy = c;
    c.reset();
    tfl::AsyncFuture<int> survivor = std::move(copy);
    REQUIRE_FALSE(copy);
    REQUIRE(a.use_count() == before + 3);
    REQUIRE(survivor.get() == 42);
    survivor.reset();
    REQUIRE(a.use_count() == before);
}

TEST_CASE("AsyncTask: idle future conversion retains callable without submitting", "[async][refcount]") {
    TestEnv env;
    std::weak_ptr<int> lifetime;
    tfl::AsyncFuture<void> survivor;
    {
        auto token = std::make_shared<int>(42);
        lifetime = token;
        auto task = env.executor.defer_async([token] {});
        survivor = std::move(task);
    }
    env.executor.wait_for_all();
    REQUIRE_FALSE(lifetime.expired());
    REQUIRE_FALSE(survivor.running());
    survivor.reset();
    REQUIRE(lifetime.expired());
}

TEST_CASE("AsyncTask: completed predecessor is retained until dependent destruction", "[async][deps][refcount]") {
    TestEnv env;
    std::weak_ptr<int> lifetime;
    tfl::AsyncTask<void> dependent;
    {
        auto token = std::make_shared<int>(42);
        lifetime = token;
        auto a = env.executor.defer_async([token] {}).start();
        a.get();
        dependent = env.executor.defer_async([] {}).start(a);
    }
    dependent.get();
    env.executor.wait_for_all();
    REQUIRE_FALSE(lifetime.expired());
    dependent.reset();
    REQUIRE(lifetime.expired());
}

TEST_CASE("AsyncTask: deferred graph overloads accept mixed start dependencies", "[async][deps][module]") {
    TestEnv env(1);
    const int mode = GENERATE(0, 1, 2, 3, 4, 5);
    int value = 0, count = 0, callbacks = 0;
    bool order_ok = true;
    tfl::Flow flow;
    (void)flow.emplace([&] { order_ok = order_ok && value == 42; ++count; });
    auto callback = [&] { ++callbacks; };
    auto predicate = [&] { return count == 3; };
    auto a = env.executor.defer_async([&] { value = 42; }).start();
    auto future = env.executor.async([] { return 7; });
    tfl::AsyncTask<void> task;
    switch (mode) {
    case 0: task = env.executor.defer_async(flow); break;
    case 1: task = env.executor.defer_async(flow, callback); break;
    case 2: task = env.executor.defer_async(flow, 3ULL); break;
    case 3: task = env.executor.defer_async(flow, 3ULL, callback); break;
    case 4: task = env.executor.defer_async(flow, predicate); break;
    case 5: task = env.executor.defer_async(flow, predicate, callback); break;
    }
    task.start(a, future).get();
    REQUIRE(order_ok);
    REQUIRE(count == (mode < 2 ? 1 : 3));
    REQUIRE(callbacks == mode % 2);
}

TEST_CASE("AsyncTask: runtime and subflow dependencies include nested children", "[async][deps][runtime][subflow]") {
    TestEnv env(1);
    int value = 0;
    auto a = env.executor.defer_async([&] { value = 10; }).start();
    auto b = env.executor.defer_async([&](tfl::Runtime& rt) {
        rt.silent_async([&] { value += 20; });
    }).start(a);
    auto c = env.executor.defer_async([&](tfl::SubFlow& sf) {
        (void)sf.emplace([&] { value += 12; });
        sf.run();
    }).start(b);
    auto result = env.executor.defer_async([&] { return value; }).start(c);
    REQUIRE(result.get() == 42);
}

TEST_CASE("Submission: all contexts validate and retain mixed dependencies", "[async][deps][runtime][task-group]") {
    TestEnv env(1);
    const int context = GENERATE(0, 1, 2);
    const bool inherit = GENERATE(false, true);
    auto a = env.executor.defer_async([] { return 20; }).start();
    a.get();
    env.executor.wait_for_all();
    auto idle = env.executor.defer_async([] {});
    tfl::AsyncFuture<void> empty;
    tfl::AsyncFuture<int> child;
    auto exercise = [&](auto& submitter) {
        REQUIRE_THROWS_AS(submitter.async([] {}, a, idle), tfl::Exception);
        REQUIRE(a.use_count() == 1);
        if (inherit)
            child = submitter.template async<true>([] { return 42; }, a, empty, a);
        else
            child = submitter.async([] { return 42; }, a, empty, a);
    };
    if (context == 0) {
        REQUIRE_THROWS_AS(env.executor.async([] {}, a, idle), tfl::Exception);
        REQUIRE(a.use_count() == 1);
        child = env.executor.async([] { return 42; }, a, empty, a);
    } else {
        env.executor.async([&](tfl::Runtime& rt) {
            if (context == 1) {
                exercise(rt);
                rt.wait();
            } else {
                tfl::TaskGroup group(rt);
                exercise(group);
                group.wait();
                REQUIRE(group.size() == 0);
            }
        }).get();
    }
    REQUIRE(child.get() == 42);
    env.executor.wait_for_all();
    REQUIRE(a.use_count() == 3);
    child.reset();
    REQUIRE(a.use_count() == 1);
}

TEST_CASE("AsyncTask: predecessor failure releases completion dependency", "[async][deps][exception]") {
    TestEnv env;
    bool cleanup_ran = false;
    auto a = env.executor.defer_async([]() -> int { throw std::runtime_error("predecessor failed"); }).start();
    auto cleanup = env.executor.defer_async([&] { cleanup_ran = true; }).start(a);
    auto result = env.executor.defer_async([a] { return a.get(); }).start(a);
    cleanup.get();
    REQUIRE(cleanup_ran);
    REQUIRE_THROWS_AS(result.get(), std::runtime_error);
}

TEST_CASE("AsyncTask: dependencies can cross bound executors", "[async][deps][executor]") {
    TestEnv first(1), second(1);
    auto a = first.executor.defer_async([] { return 41; }).start();
    auto b = second.executor.defer_async([a](tfl::Runtime& rt) {
        return std::pair{a.get() + 1, &rt.executor()};
    }).start(a);
    REQUIRE(b.get().first == 42);
    REQUIRE(b.get().second == &second.executor);
}

TEST_CASE("AsyncTask: submitted temporary keeps dependencies without external handles", "[async][deps][refcount]") {
    TestEnv env(1);
    int value = 0;
    auto a = env.executor.defer_async([&] { value = 20; }).start();
    env.executor.defer_async([&] { value += 22; }).start(a);
    a.reset();
    env.executor.wait_for_all();
    REQUIRE(value == 42);
}

TEST_CASE("Runtime: async joins children after local handles leave scope", "[async][deps][runtime][refcount]") {
    TestEnv env(1);
    tfl::AsyncFuture<int> result;
    auto parent = env.executor.async([&](tfl::Runtime& rt) {
        auto a = rt.async([] { return 41; });
        result = rt.async([a] { return a.get() + 1; }, a);
    });
    parent.get();
    REQUIRE(result.done());
    REQUIRE(result.get() == 42);
}

TEST_CASE("Runtime: independent deferred tasks use explicit cooperative wait", "[async][runtime]") {
    TestEnv env(1);
    auto parent = env.executor.async([](tfl::Runtime& rt) {
        auto task = rt.executor().defer_async([] { return 42; }).start();
        rt.wait_until([&] { return task.done(); });
        return task.get();
    });
    REQUIRE(parent.get() == 42);
}

TEST_CASE("AsyncTask: successor growth preserves a live predecessor suffix", "[async][deps][refcount]") {
    TestEnv env(1);
    tfl::Semaphore gate{1, 0};
    auto root = env.executor.defer_async([] { return 20; }).acquire(gate).start();
    auto a = env.executor.defer_async([root] { return root.get() + 2; }).start(root);
    std::vector<tfl::AsyncTask<int>> successors;
    for (int i = 0; i < 256; ++i) {
        successors.push_back(env.executor.defer_async([a, root] { return a.get() + root.get(); }).start(a, root, a));
    }
    env.executor.defer_async([] {}).release(gate).start();
    for (const auto& successor : successors) REQUIRE(successor.get() == 42);
    env.executor.wait_for_all();
    successors.clear();
    a.reset();
    REQUIRE(root.use_count() == 1);
}

TEST_CASE("AsyncTask: concurrent copies acquire a single start right", "[async][deps][concurrent]") {
    TestEnv env;
    std::atomic<int> executions{0}, submitted{0}, rejected{0};
    auto predecessor = env.executor.defer_async([] {}).start();
    auto task = env.executor.defer_async([&] { ++executions; });
    std::barrier start{8};
    std::vector<std::jthread> submitters;
    for (int i = 0; i < 8; ++i) {
        submitters.emplace_back([&, copy = task]() mutable {
            start.arrive_and_wait();
            try {
                copy.start(predecessor);
                ++submitted;
            } catch (const tfl::Exception&) {
                ++rejected;
            }
        });
    }
    submitters.clear();
    task.get();
    REQUIRE(submitted.load() == 1);
    REQUIRE(rejected.load() == 7);
    REQUIRE(executions.load() == 1);
}

TEST_CASE("AsyncTask: concurrent fan-in registration races predecessor completion", "[async][deps][concurrent]") {
    TestEnv env;
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto a = env.executor.defer_async([] { return 20; }).start();
        auto b = env.executor.defer_async([] { return 22; }).start();
        auto c = env.executor.defer_async([a, b] { return a.get() + b.get(); });
        auto d = env.executor.defer_async([a, b] { return a.get() + b.get(); });
        std::barrier start{3};
        std::jthread left([&] { start.arrive_and_wait(); c.start(a, b); });
        std::jthread right([&] { start.arrive_and_wait(); d.start(b, a); });
        start.arrive_and_wait();
        left.join();
        right.join();
        auto e = env.executor.defer_async([c, d] { return c.get() + d.get(); }).start(c, d);
        REQUIRE(e.get() == 84);
    }
}

TEST_CASE("AsyncTask: long retained dependency chains are reclaimed iteratively", "[async][deps][refcount]") {
    TestEnv env(1);
    std::weak_ptr<int> lifetime;
    auto token = std::make_shared<int>(42);
    lifetime = token;
    tfl::AsyncFuture<void> tail = env.executor.defer_async([token] {}).start();
    token.reset();
    for (int i = 0; i < 20000; ++i) tail = env.executor.defer_async([] {}).start(tail);
    tail.get();
    env.executor.wait_for_all();
    REQUIRE_FALSE(lifetime.expired());
    tail.reset();
    REQUIRE(lifetime.expired());
}
