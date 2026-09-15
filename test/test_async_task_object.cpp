/// @file test_async_task_object.cpp
/// @brief AsyncTaskObject：延迟启动、结果、四种节点、共享所有权和异常。
#include "test_common.hpp"
#include "object_task_fixtures.hpp"
#include <tuple>
#include <type_traits>

using namespace tfl_test::objects;

static_assert(std::same_as<tfl::AsyncTaskObject<int, RuntimeJob>::object_type, RuntimeJob>);
static_assert(std::derived_from<tfl::AsyncTaskObject<int, RuntimeJob>, tfl::AsyncTask<int>>);
static_assert(std::same_as<decltype(std::declval<const tfl::AsyncTaskObject<int, RuntimeJob>&>().object()), RuntimeJob&>);

TEST_CASE("AsyncTaskObject: deferred Basic supports configuration and shared start", "[async_task_object][basic]") {
    tfl_test::TestEnv env;
    auto task = env.executor.defer_async_object<Counter>(40);
    static_assert(std::same_as<decltype(task), tfl::AsyncTaskObject<void, Counter>>);
    task.name("object-counter");
    REQUIRE(task.type() == tfl::TaskType::Basic);
    REQUIRE(task.name() == "object-counter");
    env.executor.wait_for_all();
    REQUIRE(task.object().calls == 0);
    REQUIRE_FALSE(task.done());
    const auto copy = task;
    copy.object().value = 41;
    REQUIRE(&copy.object() == &task.object());
    task.start();
    copy.get();
    REQUIRE(task.done());
    REQUIRE(copy.object().value == 42);
    REQUIRE(copy.object().calls == 1);
    REQUIRE_THROWS(task.start());
}

TEST_CASE("AsyncTaskObject: result types remain distinct from object access", "[async_task_object][result]") {
    tfl_test::TestEnv env;
    SECTION("move-only value and repeated get") {
        struct Value : Immovable {
            int value;
            explicit Value(int n) : value(n) {}
            std::unique_ptr<int> operator()() { return std::make_unique<int>(value); }
        };
        auto task = env.executor.defer_async_object<Value>(42);
        static_assert(std::same_as<decltype(task), tfl::AsyncTaskObject<std::unique_ptr<int>, Value>>);
        task.start();
        auto copy = task;
        REQUIRE(*task.get() == 42);
        REQUIRE(&task.get() == &copy.get());
        REQUIRE(task.object().value == 42);
    }
    SECTION("reference aliases the stored business object") {
        struct Reference : Immovable {
            int value{42};
            int& operator()() { return value; }
        };
        auto task = env.executor.defer_async_object<Reference>();
        static_assert(std::same_as<decltype(task), tfl::AsyncTaskObject<int&, Reference>>);
        task.start();
        REQUIRE(&task.get() == &task.object().value);
        task.get() = 7;
        REQUIRE(task.object().value == 7);
    }
}

TEST_CASE("AsyncTaskObject: Runtime and SubFlow return values after child work", "[async_task_object][dynamic]") {
    tfl_test::TestEnv env(1);
    auto runtime = env.executor.defer_async_object<RuntimeJob>();
    auto subflow = env.executor.defer_async_object<SubflowJob>();
    static_assert(std::same_as<decltype(runtime), tfl::AsyncTaskObject<int, RuntimeJob>>);
    static_assert(std::same_as<decltype(subflow), tfl::AsyncTaskObject<int, SubflowJob>>);
    REQUIRE(runtime.type() == tfl::TaskType::Runtime);
    REQUIRE(subflow.type() == tfl::TaskType::Graph);
    runtime.start();
    subflow.start(runtime);
    REQUIRE(subflow.get() == 11);
    REQUIRE(runtime.get() == 7);
    REQUIRE(subflow.object().value == 11);
    REQUIRE(runtime.object().value == 7);
}

TEST_CASE("AsyncTaskObject: all Module overloads and completion callbacks", "[async_task_object][module]") {
    tfl_test::TestEnv env;
    std::atomic<int> callbacks{0};
    auto callback = [&] { ++callbacks; };
    auto verify = [&](auto task, int count, int step, int expected_callbacks) {
        static_assert(std::same_as<decltype(task), tfl::AsyncTaskObject<void, Module>>);
        REQUIRE(task.object().calls == 0);
        REQUIRE(task.type() == tfl::TaskType::Graph);
        task.start();
        task.get();
        REQUIRE(task.object().calls == count);
        REQUIRE(task.object().value == count * step);
        REQUIRE(callbacks.load() == expected_callbacks);
    };
    SECTION("default") { verify(env.executor.defer_async_object<Module>(), 1, 1, 0); }
    SECTION("default callback") { verify(env.executor.defer_async_object<Module>(callback), 1, 1, 1); }
    SECTION("count") { verify(env.executor.defer_async_object<Module>(3), 3, 1, 0); }
    SECTION("count callback") { verify(env.executor.defer_async_object<Module>(3, callback), 3, 1, 1); }
    SECTION("zero count still calls completion") { verify(env.executor.defer_async_object<Module>(0, callback), 0, 1, 1); }
    SECTION("predicate") {
        verify(env.executor.defer_async_object<Module>([n = 0]() mutable { return n++ == 2; }), 2, 1, 0);
    }
    SECTION("predicate callback") {
        verify(env.executor.defer_async_object<Module>([n = 0]() mutable { return n++ == 2; }, callback), 2, 1, 1);
    }
    SECTION("tuple") {
        verify(env.executor.defer_async_object<Module>(std::make_tuple(std::make_unique<int>(4))), 1, 4, 0);
    }
    SECTION("tuple callback") {
        verify(env.executor.defer_async_object<Module>(std::make_tuple(std::make_unique<int>(4)), callback), 1, 4, 1);
    }
    SECTION("tuple count") {
        verify(env.executor.defer_async_object<Module>(std::make_tuple(std::make_unique<int>(4)), 3), 3, 4, 0);
    }
    SECTION("tuple count callback") {
        verify(env.executor.defer_async_object<Module>(std::make_tuple(std::make_unique<int>(4)), 3, callback), 3, 4, 1);
    }
    SECTION("tuple predicate") {
        verify(env.executor.defer_async_object<Module>(std::make_tuple(std::make_unique<int>(4)),
            [n = 0]() mutable { return n++ == 2; }), 2, 4, 0);
    }
    SECTION("tuple predicate callback") {
        verify(env.executor.defer_async_object<Module>(std::make_tuple(std::make_unique<int>(4)),
            [n = 0]() mutable { return n++ == 2; }, callback), 2, 4, 1);
    }
    SECTION("empty lvalue tuple") {
        std::tuple<> args;
        verify(env.executor.defer_async_object<Module>(args, callback), 1, 1, 1);
    }
}

TEST_CASE("AsyncTaskObject: handle assignment transfers association and ownership", "[async_task_object][handle][lifetime]") {
    std::atomic<int> destroyed{0};
    tfl_test::TestEnv env;
    using Handle = tfl::AsyncTaskObject<int, Lifetime>;
    Handle empty;
    Handle null{nullptr};
    REQUIRE_FALSE(empty.valid());
    REQUIRE_FALSE(null.valid());
    auto first = env.executor.defer_async_object<Lifetime>(destroyed);
    auto* address = &first.object();
    auto copy = first;
    auto moved = std::move(first);
    REQUIRE_FALSE(first.valid());
    REQUIRE(&moved.object() == address);
    empty = copy;
    REQUIRE(&empty.object() == address);
    auto replaced = env.executor.defer_async_object<Lifetime>(destroyed);
    SECTION("copy assignment releases the previous object") { replaced = copy; }
    SECTION("move assignment releases the previous object") {
        replaced = std::move(moved);
        REQUIRE_FALSE(moved.valid());
    }
    REQUIRE(destroyed.load() == 1);
    REQUIRE(&replaced.object() == address);
    auto& alias = replaced;
    replaced = alias;
    replaced = std::move(alias);
    REQUIRE(&replaced.object() == address);
    copy.reset();
    empty = nullptr;
    moved.reset();
    REQUIRE(destroyed.load() == 1);
    replaced.start();
    REQUIRE(replaced.get() == 42);
    env.executor.wait_for_all();
    replaced.reset();
    REQUIRE(destroyed.load() == 2);
}

TEST_CASE("AsyncTaskObject: last idle handle destroys unstarted object", "[async_task_object][lifetime]") {
    std::atomic<int> destroyed{0};
    tfl_test::TestEnv env;
    auto task = env.executor.defer_async_object<Lifetime>(destroyed);
    auto copy = task;
    task = nullptr;
    REQUIRE(destroyed.load() == 0);
    copy.reset();
    REQUIRE(destroyed.load() == 1);
    env.executor.wait_for_all();
}

TEST_CASE("AsyncTaskObject: dependencies retain the business object", "[async_task_object][deps][lifetime]") {
    std::atomic<int> destroyed{0};
    tfl_test::TestEnv env;
    auto source = env.executor.defer_async_object<Lifetime>(destroyed);
    auto* object = &source.object();
    auto ordinary = env.executor.defer_async([object] { return (*object)() + 1; });
    source.start();
    ordinary.start(source);
    source.reset();
    REQUIRE(ordinary.get() == 43);
    env.executor.wait_for_all();
    REQUIRE(destroyed.load() == 0);
    ordinary.reset();
    REQUIRE(destroyed.load() == 1);
}

TEST_CASE("AsyncTaskObject: stop request and semaphore configuration are inherited", "[async_task_object][control]") {
    tfl_test::TestEnv env;
    tfl::Semaphore semaphore(1);
    auto task = env.executor.defer_async_object<Counter>();
    task.acquire(semaphore).release(semaphore);
    task.start();
    task.get();
    REQUIRE(task.done());
    REQUIRE(task.object().calls == 1);
    REQUIRE(task.request_stop());
    REQUIRE(task.stop_requested());
    REQUIRE_FALSE(task.request_stop());
}

TEST_CASE("AsyncTaskObject: construction and invocation exceptions", "[async_task_object][exception]") {
    tfl_test::TestEnv env;
    SECTION("failed construction leaves executor usable") {
        REQUIRE_THROWS_AS(env.executor.defer_async_object<ThrowingConstructor>(std::make_unique<int>(1)), std::runtime_error);
        env.executor.wait_for_all();
        auto task = env.executor.defer_async_object<Counter>();
        task.start();
        task.get();
        REQUIRE(task.object().calls == 1);
    }
    SECTION("invocation exception is shared and object remains accessible") {
        struct Failing : Immovable {
            int calls{0};
            void operator()() { ++calls; throw std::runtime_error("object execution failed"); }
        };
        auto task = env.executor.defer_async_object<Failing>();
        auto copy = task;
        task.start();
        task.wait();
        REQUIRE_THROWS_AS(task.get(), std::runtime_error);
        REQUIRE_THROWS_AS(copy.get(), std::runtime_error);
        REQUIRE(copy.object().calls == 1);
    }
}
