/// @file test_flow_builder.cpp
/// @brief FlowBuilder 测试 —— 占位符、重绑定、线性连边、删除和参数包。

#include "test_common.hpp"
#include <array>
#include <list>
#include <span>

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: 结构构建与管理
// ============================================================================

/// @test [builder][linearize] range、参数包、初始化列表以及空范围。
TEST_CASE("FlowBuilder: linearize overloads preserve order", "[builder][linearize]") {
    TestEnv env;
    const int mode = GENERATE(0, 1, 2, 3, 4);
    tfl::Flow flow;
    std::vector<int> order;
    auto a = flow.emplace([&] { order.push_back(1); });
    auto b = flow.emplace([&] { order.push_back(2); });
    auto c = flow.emplace([&] { order.push_back(3); });
    const std::array tasks{a, b, c};
    switch (mode) {
    case 0: flow.linearize(a, b, c); break;
    case 1: flow.linearize({a, b, c}); break;
    case 2: flow.linearize(tasks); break;
    case 3: flow.linearize(std::span{tasks}); break;
    case 4: flow.linearize(std::list<tfl::Task>{a, b, c}); break;
    }
    flow.linearize(std::array<tfl::Task, 0>{});
    flow.linearize(std::array{a});
    REQUIRE(a.num_successors() == 1);
    REQUIRE(c.num_predecessors() == 1);
    env.executor.async(flow).get();
    REQUIRE(order == std::vector<int>{1, 2, 3});
}

/// @test [builder][erase] 删除节点会清理双向边，空句柄和其他图节点被忽略。
TEST_CASE("FlowBuilder: erase maintains both edge directions", "[builder][erase]") {
    tfl::Flow flow, other;
    auto a = flow.placeholder(), b = flow.placeholder(), c = flow.placeholder();
    flow.linearize(a, b, c);
    const auto identity = flow.hash_value();
    flow.erase(b);
    REQUIRE(flow.size() == 2);
    REQUIRE(a.num_successors() == 0);
    REQUIRE(c.num_predecessors() == 0);
    flow.erase(tfl::Task{}, other.placeholder());
    REQUIRE(flow.size() == 2);
    REQUIRE(flow.hash_value() == identity);
    flow.erase(a, c);
    REQUIRE(flow.empty());
    REQUIRE(other.size() == 1);
}

/// @test [builder][placeholder] 占位符先连边再绑定 callable，节点身份和名称保持。
TEST_CASE("FlowBuilder: placeholders can be rebound after linking", "[builder][placeholder]") {
    TestEnv env;
    tfl::Flow flow;
    int result = 0;
    auto a = flow.placeholder().name("first");
    auto b = flow.placeholder().name("second");
    a.precede(b);
    const auto identity = a.hash_value();
    REQUIRE(a.type() == tfl::TaskType::Placeholder);
    a.work([&] { result = 20; });
    b.work([&] { result += 22; });
    REQUIRE(a.hash_value() == identity);
    REQUIRE(a.name() == "first");
    env.executor.async(flow).get();
    REQUIRE(result == 42);
}

// ============================================================================
// SECTION 2: pack、图访问和遍历
// ============================================================================

/// @test [builder][pack] pack 转发模块参数，子图调用次数和普通任务共同验证。
TEST_CASE("FlowBuilder: pack forwards module count and predicate", "[builder][pack]") {
    TestEnv env;
    tfl::Flow flow, inner;
    int count = 0, callbacks = 0;
    (void)inner.emplace([&] { ++count; });
    auto [first, second, third] = flow.emplace(
        tfl::pack{std::move(inner), 2ULL},
        tfl::pack{[&] { ++callbacks; }},
        tfl::pack{[] {}});
    flow.linearize(first, second, third);
    env.executor.async(flow).get();
    REQUIRE(count == 2);
    REQUIRE(callbacks == 1);
    REQUIRE(&std::as_const(flow).graph() == &flow.graph());
    int visited = 0;
    flow.for_each([&](tfl::Task task) { ++visited; REQUIRE(task.valid()); });
    REQUIRE(visited == 3);
    REQUIRE_THROWS_AS(flow.for_each([](tfl::Task) { throw std::runtime_error("visitor"); }), std::runtime_error);
}
