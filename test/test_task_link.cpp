/// @file test_task_link.cpp
/// @brief 建边策略：默认校验、显式跳过、派生句柄及各入口转发。
#include "test_common.hpp"
#include "object_task_fixtures.hpp"
#include <array>
#include <list>

TEST_CASE("Task links: default validation rejects invalid edges without mutation", "[task][link][checked]") {
    tfl::Flow flow, other;
    auto a = flow.placeholder(), b = flow.placeholder(), c = flow.placeholder();
    flow.linearize(a, b, c);
    SECTION("duplicate") { REQUIRE_THROWS_AS(a.precede(b), tfl::Exception); }
    SECTION("explicit checked") { REQUIRE_THROWS_AS(a.precede<true>(b), tfl::Exception); }
    SECTION("cycle") { REQUIRE_THROWS_AS(c.precede(a), tfl::Exception); }
    SECTION("self loop") { REQUIRE_THROWS_AS(a.precede(a), tfl::Exception); }
    SECTION("different graph") { REQUIRE_THROWS_AS(a.precede(other.placeholder()), tfl::Exception); }
    SECTION("succeed") { REQUIRE_THROWS_AS(a.succeed(c), tfl::Exception); }
    SECTION("rvalue precede") { REQUIRE_THROWS_AS(tfl::Task{c}.precede(a), tfl::Exception); }
    SECTION("rvalue succeed") { REQUIRE_THROWS_AS(tfl::Task{a}.succeed(c), tfl::Exception); }
    SECTION("forward operator stays checked") { REQUIRE_THROWS_AS(a >> b, tfl::Exception); }
    SECTION("reverse operator stays checked") { REQUIRE_THROWS_AS(b << a, tfl::Exception); }
    REQUIRE(a.num_successors() == 1);
    REQUIRE(a.num_predecessors() == 0);
    REQUIRE(b.num_successors() == 1);
    REQUIRE(b.num_predecessors() == 1);
    REQUIRE(c.num_successors() == 0);
    REQUIRE(c.num_predecessors() == 1);
}

TEST_CASE("Task links: both policies preserve fan-out fan-in and derived handles", "[task][link]") {
    tfl_test::TestEnv env;
    const bool checked = GENERATE(true, false);
    const bool rvalue = GENERATE(true, false);
    tfl::Flow flow;
    int result = 0;
    auto root = flow.placeholder();
    auto a = flow.emplace_object<tfl_test::objects::Counter>(10);
    const auto b = flow.emplace_object<tfl_test::objects::Counter>(20);
    auto sink = flow.emplace([&] { result = a.object().value + b.object().value; });
    auto build = [&]<bool Check>() {
        if (rvalue) {
            auto source = tfl::Task{root}.precede<Check>(a, b);
            auto finish = tfl::Task{sink}.succeed<Check>(a, b);
            REQUIRE(source == root);
            REQUIRE(finish == sink);
        } else {
            REQUIRE(&root.precede<Check>(a, b) == &root);
            REQUIRE(&sink.succeed<Check>(a, b) == &sink);
        }
    };
    if (checked) build.template operator()<true>();
    else build.template operator()<false>();
    REQUIRE(root.num_successors() == 2);
    REQUIRE(sink.num_predecessors() == 2);
    env.executor.async(flow).get();
    REQUIRE(result == 32);
    root.remove_successor(tfl::Task{a});
    REQUIRE(a.num_predecessors() == 0);
    REQUIRE(root.num_successors() == 1);
}

TEST_CASE("Task links: unchecked linearize forwards every overload", "[builder][link][unchecked]") {
    tfl_test::TestEnv env;
    tfl::Flow flow;
    std::vector<int> order;
    struct Append {
        std::vector<int>& order;
        int value;
        void operator()() { order.push_back(value); }
    };
    auto a = flow.emplace_object<Append>(order, 1);
    auto b = flow.emplace_object<Append>(order, 2);
    auto c = flow.emplace_object<Append>(order, 3);
    SECTION("mixed parameter pack") { flow.linearize<false>(a, tfl::Task{b}, c); }
    SECTION("initializer list") { flow.linearize<false>({a, b, c}); }
    SECTION("const derived range") {
        const std::array tasks{a, b, c};
        flow.linearize<false>(tasks);
    }
    SECTION("forward range") { flow.linearize<false>(std::list{a, b, c}); }
    flow.linearize<false>({});
    flow.linearize<false>({a});
    flow.linearize<false>(std::array<tfl::Task, 0>{});
    flow.linearize<false>(std::array{a});
    env.executor.async(flow).get();
    REQUIRE(order == std::vector<int>{1, 2, 3});
    REQUIRE(b.num_predecessors() == 1);
    REQUIRE(b.num_successors() == 1);
    flow.erase(b);
    REQUIRE(a.num_successors() == 0);
    REQUIRE(c.num_predecessors() == 0);
}

TEST_CASE("Task links: linearize remains checked by default", "[builder][link][checked]") {
    tfl::Flow flow;
    auto a = flow.placeholder(), b = flow.placeholder();
    a.precede(b);
    SECTION("pack") { REQUIRE_THROWS_AS(flow.linearize(a, b), tfl::Exception); }
    SECTION("initializer list") { REQUIRE_THROWS_AS(flow.linearize({a, b}), tfl::Exception); }
    SECTION("range") { REQUIRE_THROWS_AS(flow.linearize(std::array{a, b}), tfl::Exception); }
    SECTION("explicit checked") { REQUIRE_THROWS_AS(flow.linearize<true>(a, b), tfl::Exception); }
    REQUIRE(a.num_successors() == 1);
    REQUIRE(b.num_predecessors() == 1);
}

TEST_CASE("Task links: Jump loops remain supported with both policies", "[task][link][jump]") {
    tfl_test::TestEnv env;
    tfl::Flow flow;
    int iterations = 0;
    auto entry = flow.placeholder();
    auto jump = flow.emplace([&](tfl::Jump& next) {
        ++iterations;
        next.select(iterations < 3 ? 0 : 1);
    });
    int result = 0;
    auto done = flow.emplace([&] { result = iterations; });
    entry.precede(jump);
    SECTION("checked") { jump.precede(jump, done); }
    SECTION("unchecked") { jump.precede<false>(jump, done); }
    env.executor.async(flow).get();
    REQUIRE(result == 3);
}
