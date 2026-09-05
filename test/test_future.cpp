/// @file test_future.cpp
/// @brief AsyncFuture 测试 —— 共享结果、值/引用/void、状态、停止控制与依赖。
///
/// 覆盖的接口：
///   - get / wait / valid / operator bool / done / running / has_exception
///   - copy / move / reset / nullptr / use_count / hash_value / operator==
///   - request_stop / stop_requested / type / dump
///
/// 关键约束：get() 不消耗句柄，值结果返回 const R&；工作线程内先协作等待。

#include "test_common.hpp"

#include <sstream>
#include <type_traits>
#include <unordered_set>

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: 结果类型与非消费式 get
// ============================================================================

/// @test [future][basic] get 可重复调用且不改变共享结果地址。
TEST_CASE("AsyncFuture: repeated get shares a const result", "[future][basic]") {
    TestEnv env;
    auto future = env.executor.async([] { return std::string("result"); });
    STATIC_REQUIRE(std::same_as<decltype(future.get()), const std::string&>);
    REQUIRE(future.get() == "result");
    REQUIRE(&future.get() == &future.get());
    REQUIRE(future.valid());
    REQUIRE(static_cast<bool>(future));
    REQUIRE(future.done());
    REQUIRE_FALSE(future.running());
}

/// @test [future][result] 非默认构造、move-only、引用与 void 结果。
TEST_CASE("AsyncFuture: value reference and void results", "[future][result]") {
    TestEnv env;
    SECTION("move-only result") {
        auto future = env.executor.async([] { return std::make_unique<int>(42); });
        STATIC_REQUIRE(std::same_as<decltype(future.get()), const std::unique_ptr<int>&>);
        REQUIRE(*future.get() == 42);
        auto copy = future;
        REQUIRE(&copy.get() == &future.get());
    }
    SECTION("non-default-constructible result") {
        struct Value {
            explicit Value(int n) : number(n) {}
            int number;
        };
        auto future = env.executor.async([] { return Value{17}; });
        REQUIRE(future.get().number == 17);
    }
    SECTION("reference result") {
        int value = 7;
        auto future = env.executor.async([&]() -> int& { return value; });
        STATIC_REQUIRE(std::same_as<decltype(future.get()), int&>);
        REQUIRE(&future.get() == &value);
        future.get() = 9;
        REQUIRE(value == 9);
    }
    SECTION("void result") {
        int value = 0;
        auto future = env.executor.async([&] { value = 42; });
        STATIC_REQUIRE(std::same_as<decltype(future.get()), void>);
        REQUIRE_NOTHROW(future.get());
        REQUIRE_NOTHROW(future.get());
        REQUIRE(value == 42);
        REQUIRE(future.valid());
    }
}

// ============================================================================
// SECTION 2: 句柄生命周期与空状态
// ============================================================================

/// @test [future][lifecycle] 复制共享所有权；移动、reset 和 nullptr 只释放本句柄。
TEST_CASE("AsyncFuture: copy move reset and identity", "[future][lifecycle]") {
    TestEnv env;
    auto future = env.executor.async([] { return 42; });
    future.wait();
    env.executor.wait_for_all();  // 等待执行引用释放后再断言引用计数
    REQUIRE(future.use_count() == 1);
    auto copy = future;
    REQUIRE(copy == future);
    REQUIRE(copy.use_count() == 2);
    REQUIRE(copy.hash_value() == future.hash_value());
    REQUIRE(std::hash<tfl::AsyncFuture<int>>{}(copy) == future.hash_value());
    auto moved = std::move(copy);
    REQUIRE_FALSE(copy.valid());
    REQUIRE(moved == future);
    copy = moved;
    REQUIRE(future.use_count() == 3);
    copy = copy;
    REQUIRE(future.use_count() == 3);
    copy = nullptr;
    moved.reset();
    REQUIRE(future.use_count() == 1);
    moved = std::move(future);
    REQUIRE_FALSE(future.valid());
    REQUIRE(moved.get() == 42);
}

/// @test [future][empty] 空句柄查询与 wait 安全；get/dump 明确拒绝无任务状态。
TEST_CASE("AsyncFuture: empty handle contract", "[future][empty]") {
    tfl::AsyncFuture<int> future;
    tfl::AsyncFuture<int> other{nullptr};
    REQUIRE(future == other);
    REQUIRE_FALSE(future.valid());
    REQUIRE_FALSE(future.done());
    REQUIRE_FALSE(future.running());
    REQUIRE_FALSE(future.stop_requested());
    REQUIRE_FALSE(future.request_stop());
    REQUIRE(future.use_count() == 0);
    REQUIRE(future.type() == tfl::TaskType::None);
    REQUIRE_NOTHROW(future.wait());
    REQUIRE_NOTHROW(future.reset());
    REQUIRE_THROWS_AS(future.get(), tfl::Exception);
    REQUIRE_THROWS_AS(future.dump(), tfl::Exception);
}

// ============================================================================
// SECTION 3: 确定性状态同步、停止请求与异常
// ============================================================================

/// @test [future][state] 用门闩保持任务运行，避免依赖 sleep 的时序断言。
TEST_CASE("AsyncFuture: running and cooperative stop", "[future][state][stop]") {
    TestEnv env(1);
    std::atomic<bool> entered{false}, finish{false};
    auto future = env.executor.async([&] {
        entered.store(true);
        entered.notify_one();
        finish.wait(false);
        return 11;
    });
    entered.wait(false);
    const bool running = future.running();
    const bool done = future.done();
    const bool first = future.request_stop();
    const bool second = future.request_stop();
    const bool stopped = future.stop_requested();
    finish.store(true);
    finish.notify_one();  // 在可能失败的断言之前释放任务，避免析构等待死锁
    REQUIRE(future.get() == 11);  // 停止请求不抢占已运行 callable
    REQUIRE(running);
    REQUIRE_FALSE(done);
    REQUIRE(first);
    REQUIRE_FALSE(second);
    REQUIRE(stopped);
}

/// @test [future][exception] wait 同步但不重抛；每次 get 均重抛原异常。
TEST_CASE("AsyncFuture: shared exception survives repeated get", "[future][exception]") {
    TestEnv env;
    auto future = env.executor.async([]() -> int { throw std::runtime_error("planned"); });
    auto copy = future;
    REQUIRE_NOTHROW(future.wait());
    REQUIRE_THROWS_AS(future.get(), std::runtime_error);
    REQUIRE_THROWS_AS(copy.get(), std::runtime_error);
    REQUIRE(future.valid());
}

// ============================================================================
// SECTION 4: 依赖、Runtime 协作等待与导出
// ============================================================================

/// @test [future][deps] async 接受 Future 前驱，包括已完成的前驱。
TEST_CASE("AsyncFuture: dependency fan-in and completed predecessors", "[future][deps]") {
    TestEnv env(1);
    auto first = env.executor.async([] { return 20; });
    auto second = env.executor.async([] { return 22; });
    auto sum = env.executor.async([first, second] { return first.get() + second.get(); }, first, second);
    REQUIRE(sum.get() == 42);
    auto next = env.executor.async([sum] { return sum.get() + 1; }, sum);
    REQUIRE(next.get() == 43);
}

/// @test [future][runtime] 单线程 Runtime 通过 wait_until 协作执行子任务。
TEST_CASE("AsyncFuture: Runtime cooperatively waits for a result", "[future][runtime]") {
    TestEnv env(1);
    auto parent = env.executor.async([](tfl::Runtime& rt) {
        auto future = rt.async([] { return 42; });
        rt.wait_until([&] { return future.done(); });
        return future.get();
    });
    REQUIRE(parent.get() == 42);
}

/// @test [future][dump] 字符串与流输出一致，并包含方向和任务名称。
TEST_CASE("AsyncFuture: dump and task type", "[future][dump]") {
    TestEnv env;
    auto task = tfl::AsyncTask([] { return 7; }).name("result_node");
    env.executor.run(task).wait();
    const tfl::AsyncFuture<int> future = task;
    REQUIRE(future.type() != tfl::TaskType::None);
    std::ostringstream stream;
    future.dump(stream, tfl::Direction::Right);
    REQUIRE(stream.str() == future.dump(tfl::Direction::Right));
    REQUIRE(stream.str().find("result_node") != std::string::npos);
}

/// @test [future][stress] 多个 Future 的值不互相覆盖。
TEST_CASE("AsyncFuture: 100 concurrent results", "[future][stress]") {
    TestEnv env(8);
    std::vector<tfl::AsyncFuture<int>> futures;
    for (int i = 0; i < 100; ++i) {
        futures.push_back(env.executor.async([i] { return i * i; }));
    }
    int sum = 0;
    for (const auto& future : futures) sum += future.get();
    REQUIRE(sum == 99 * 100 * 199 / 6);
}
