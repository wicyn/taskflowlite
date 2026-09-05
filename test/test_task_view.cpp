/// @file test_task_view.cpp
/// @brief TaskView 测试 —— const 遍历、身份、信号量、异常和 D2 导出。

#include "test_common.hpp"
#include <sstream>

using tfl_test::TestEnv;

/// @test [task-view][metadata] 从 const Task 遍历取得只读视图并验证邻接关系。
TEST_CASE("TaskView: metadata and const traversal", "[task-view][metadata]") {
    tfl::Flow flow;
    tfl::Semaphore semaphore{3};
    auto a = flow.emplace([] {}).name("a");
    auto b = flow.emplace([] {}).name("b").acquire(semaphore, 2).release(semaphore, 2);
    auto c = flow.emplace([] {}).name("c");
    flow.linearize(a, b, c);
    int visited = 0;
    std::as_const(a).for_each_successor([&](tfl::TaskView view) {
        ++visited;
        REQUIRE(view.name() == "b");
        REQUIRE(view.type() == tfl::TaskType::Basic);
        REQUIRE(view.hash_value() == b.hash_value());
        REQUIRE(std::hash<tfl::TaskView>{}(view) == view.hash_value());
        REQUIRE(view.num_predecessors() == 1);
        REQUIRE(view.num_successors() == 1);
        REQUIRE(view.num_acquires() == 1);
        REQUIRE(view.num_releases() == 1);
        REQUIRE(view.num_observers() == 0);
        REQUIRE_FALSE(view.has_exception());
        REQUIRE_FALSE(view.exception());
        view.for_each_predecessor([](tfl::TaskView pred) { REQUIRE(pred.name() == "a"); });
        view.for_each_successor([](tfl::TaskView next) { REQUIRE(next.name() == "c"); });
        view.for_each_acquire([&](const tfl::Semaphore& sem, std::size_t count) {
            REQUIRE(&sem == &semaphore);
            REQUIRE(count == 2);
        });
        view.for_each_release([&](const tfl::Semaphore& sem) { REQUIRE(&sem == &semaphore); });
        std::ostringstream stream;
        view.dump(stream, tfl::Direction::Left);
        REQUIRE(stream.str() == view.dump(tfl::Direction::Left));
        REQUIRE(stream.str().find("direction: left") != std::string::npos);
    });
    REQUIRE(visited == 1);
}

/// @test [task-view][exception] 节点保留异常标记，异常对象由提交 Future 归档。
TEST_CASE("TaskView: exception inspection after synchronization", "[task-view][exception]") {
    TestEnv env;
    tfl::Flow flow;
    auto bad = flow.emplace([] { throw std::runtime_error("node"); });
    auto next = flow.emplace([] {});
    bad.precede(next);
    auto future = env.executor.async(flow);
    future.wait();
    REQUIRE_THROWS_AS(future.get(), std::runtime_error);
    std::as_const(next).for_each_predecessor([](tfl::TaskView view) {
        REQUIRE(view.has_exception());
        REQUIRE_FALSE(view.exception());  // 异常已向上移动到显式锚点
    });
}
