/// @file test_task_object.cpp
/// @brief TaskObject：原地构造、八种节点、Module 重载和派生句柄拓扑操作。
#include "test_common.hpp"
#include "object_task_fixtures.hpp"
#include <array>
#include <tuple>
#include <type_traits>

using namespace tfl_test::objects;

static_assert(std::same_as<tfl::TaskObject<Counter>::object_type, Counter>);
static_assert(std::derived_from<tfl::TaskObject<Counter>, tfl::Task>);
static_assert(std::same_as<decltype(std::declval<const tfl::TaskObject<Counter>&>().object()), Counter&>);
static_assert(!std::is_copy_constructible_v<Counter> && !std::is_move_constructible_v<Counter>);

TEST_CASE("TaskObject: empty copy move reset and const access", "[task_object][handle]") {
    tfl::Flow flow;
    tfl::TaskObject<Counter> empty;
    REQUIRE_FALSE(empty.valid());
    auto original = flow.emplace_object<Counter>(10);
    auto* address = &original.object();
    const auto copy = original;
    copy.object().value = 20;
    REQUIRE(original.object().value == 20);
    REQUIRE(copy == original);
    REQUIRE(&copy.object() == address);

    auto moved = std::move(original);
    REQUIRE_FALSE(original.valid());
    REQUIRE(&moved.object() == address);
    empty = copy;
    REQUIRE(&empty.object() == address);
    empty = std::move(moved);
    REQUIRE_FALSE(moved.valid());
    REQUIRE(&empty.object() == address);
    auto& alias = empty;
    empty = std::move(alias);
    REQUIRE(&empty.object() == address);
    empty.reset();
    REQUIRE_FALSE(empty.valid());
    empty = copy;
    empty = nullptr;
    REQUIRE_FALSE(empty.valid());
    REQUIRE(copy.valid());
}

TEST_CASE("TaskObject: Basic object is constructed once and reused", "[task_object][basic]") {
    tfl_test::TestEnv env;
    tfl::Flow flow;
    auto task = flow.emplace_object<Counter>(40);
    task.name("counter");
    REQUIRE(task.name() == "counter");
    REQUIRE(task.type() == tfl::TaskType::Basic);
    auto* address = &task.object();
    env.executor.async(flow).get();
    env.executor.async(flow).get();
    REQUIRE(task.object().value == 42);
    REQUIRE(task.object().calls == 2);
    REQUIRE(&task.object() == address);
}

TEST_CASE("TaskObject: routing nodes execute selected successors", "[task_object][routing]") {
    tfl_test::TestEnv env;
    tfl::Flow flow;
    std::array<std::atomic<int>, 3> hits{};
    auto a = flow.emplace([&] { ++hits[0]; });
    auto b = flow.emplace([&] { ++hits[1]; });
    auto c = flow.emplace([&] { ++hits[2]; });
    auto exercise = [&]<typename T>(tfl::TaskType type, bool multiple) {
        auto route = flow.emplace_object<T>();
        route.precede(a, b, c);
        REQUIRE(route.type() == type);
        env.executor.async(flow).get();
        REQUIRE(route.object().calls == 1);
        REQUIRE(hits[0].load() == (multiple ? 1 : 0));
        REQUIRE(hits[1].load() == (multiple ? 0 : 1));
        REQUIRE(hits[2].load() == (multiple ? 1 : 0));
    };
    SECTION("Branch") { exercise.template operator()<Router<tfl::Branch>>(tfl::TaskType::Branch, false); }
    SECTION("MultiBranch") { exercise.template operator()<Router<tfl::MultiBranch, true>>(tfl::TaskType::MultiBranch, true); }
    SECTION("Jump") { exercise.template operator()<Router<tfl::Jump>>(tfl::TaskType::Jump, false); }
    SECTION("MultiJump") { exercise.template operator()<Router<tfl::MultiJump, true>>(tfl::TaskType::MultiJump, true); }
}

TEST_CASE("TaskObject: Runtime and SubFlow keep their business state", "[task_object][dynamic]") {
    tfl_test::TestEnv env(1);  // 子任务协作等待必须在单 worker 下也能完成。
    tfl::Flow flow;
    auto runtime = flow.emplace_object<RuntimeJob>();
    auto subflow = flow.emplace_object<SubflowJob>();
    flow.linearize(runtime, subflow);
    REQUIRE(runtime.type() == tfl::TaskType::Runtime);
    REQUIRE(subflow.type() == tfl::TaskType::Graph);
    env.executor.async(flow).get();
    env.executor.async(flow).get();
    REQUIRE(runtime.object().value == 14);
    REQUIRE(subflow.object().value == 22);
}

TEST_CASE("TaskObject: Module construction and repetition overloads", "[task_object][module]") {
    tfl_test::TestEnv env;
    tfl::Flow flow;
    auto verify = [&](auto task, int expected, int step) {
        REQUIRE(task.type() == tfl::TaskType::Graph);
        env.executor.async(flow).get();
        REQUIRE(task.object().calls == expected);
        REQUIRE(task.object().value == expected * step);
    };
    SECTION("default") { verify(flow.emplace_object<Module>(), 1, 1); }
    SECTION("default count resets on each outer execution") {
        auto task = flow.emplace_object<Module>(3);
        verify(task, 3, 1);
        verify(task, 6, 1);
    }
    SECTION("zero count") { verify(flow.emplace_object<Module>(0), 0, 1); }
    SECTION("default predicate") {
        verify(flow.emplace_object<Module>([n = 0]() mutable { return n++ == 2; }), 2, 1);
    }
    SECTION("tuple forwards move-only argument") {
        verify(flow.emplace_object<Module>(std::make_tuple(std::make_unique<int>(4))), 1, 4);
    }
    SECTION("tuple count resets") {
        auto task = flow.emplace_object<Module>(std::make_tuple(std::make_unique<int>(4)), 3);
        verify(task, 3, 4);
        verify(task, 6, 4);
    }
    SECTION("tuple predicate") {
        verify(flow.emplace_object<Module>(std::make_tuple(std::make_unique<int>(4)),
            [n = 0]() mutable { return n++ == 2; }), 2, 4);
    }
    SECTION("lvalue tuple preserves reference") {
        std::atomic<int> destroyed{0};
        auto args = std::tie(destroyed);
        struct RefModule : Immovable {
            tfl::Flow flow;
            explicit RefModule(std::atomic<int>& value) { flow.emplace([&value] { ++value; }); }
            tfl::Graph& graph() noexcept { return flow.graph(); }
            const tfl::Graph& graph() const noexcept { return flow.graph(); }
        };
        auto task = flow.emplace_object<RefModule>(args);
        env.executor.async(flow).get();
        REQUIRE(destroyed.load() == 1);
        flow.erase(task);
    }
}

TEST_CASE("TaskObject: linearize accepts ranges and mixed derived handles", "[task_object][topology]") {
    tfl_test::TestEnv env;
    tfl::Flow flow;
    std::vector<int> order;
    struct Append : Immovable {
        std::vector<int>& order;
        int value;
        Append(std::vector<int>& out, int n) : order(out), value(n) {}
        void operator()() { order.push_back(value); }
    };
    auto a = flow.emplace_object<Append>(order, 1);
    auto b = flow.emplace_object<Append>(order, 2);
    auto c = flow.emplace_object<Append>(order, 3);
    SECTION("const range of derived handles") {
        const std::array tasks{a, b, c};
        flow.linearize(tasks);
    }
    SECTION("empty and singleton ranges") {
        const std::array<tfl::TaskObject<Append>, 0> empty{};
        flow.linearize(empty);
        flow.linearize(std::array{a});
        REQUIRE(a.num_successors() == 0);
        flow.linearize(a, b, c);
    }
    SECTION("mixed Task and derived handles") {
        tfl::Task plain = b;
        flow.linearize(a, plain, c);
    }
    SECTION("initializer list") { flow.linearize({a, b, c}); }
    env.executor.async(flow).get();
    REQUIRE(order == std::vector<int>{1, 2, 3});
    flow.erase(b);
    REQUIRE(a.num_successors() == 0);
    REQUIRE(c.num_predecessors() == 0);
    auto plain = flow.emplace([] {});
    flow.erase(a, plain, c);
    REQUIRE(flow.empty());
}

TEST_CASE("TaskObject: reset is non-owning and erase destroys the object", "[task_object][lifetime]") {
    std::atomic<int> destroyed{0};
    {
        tfl::Flow flow;
        auto task = flow.emplace_object<Lifetime>(destroyed);
        auto copy = task;
        task.reset();
        REQUIRE(destroyed.load() == 0);
        flow.erase(copy);
        REQUIRE(destroyed.load() == 1);
        // erase 后 copy 是失效的弱句柄，不能访问 object()。
        copy.reset();
        auto remaining = flow.emplace_object<Lifetime>(destroyed);
        remaining = nullptr;
        REQUIRE(destroyed.load() == 1);
    }
    REQUIRE(destroyed.load() == 2);
}

TEST_CASE("TaskObject: failed construction leaves the graph usable", "[task_object][exception]") {
    tfl_test::TestEnv env;
    tfl::Flow flow;
    REQUIRE_THROWS_AS(flow.emplace_object<ThrowingConstructor>(std::make_unique<int>(1)), std::runtime_error);
    REQUIRE(flow.empty());
    auto task = flow.emplace_object<Counter>();
    env.executor.async(flow).get();
    REQUIRE(task.object().value == 1);
}
