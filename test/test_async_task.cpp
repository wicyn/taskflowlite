/// @file test_async_task.cpp
/// @brief AsyncTask 测试 —— 引用计数句柄、依赖链、生命周期。
/// @author wicyn
///
/// 覆盖的接口：
///   - AsyncTask(callable)                              创建异步任务句柄
///   - Executor::run(task, deps...)                  提交任务并声明依赖
///   - Executor::async(flow)                            提交 Flow 并返回 AsyncFuture<void>
///   - AsyncTask::name(string)                          设置名称
///   - AsyncTask::acquire / release                     配置信号量
///   - AsyncTask::wait()                                阻塞直到完成（不重抛异常）
///   - AsyncTask::get()                                 等待 + 重抛异常
///   - AsyncTask::done() / running()                    状态查询
///   - AsyncTask::name() / type()                       元数据
///   - AsyncTask::stop_requested() / request_stop()     协作式取消
///
/// 关键约束：
///   1. AsyncTask 创建后处于就绪态，通过 Executor::run() 提交执行；
///   2. 可声明依赖：run(task, dep1, dep2, ...)；
///   3. 已提交的任务不可重复提交；
///   4. 依赖使用 AsyncTask 句柄，不传递迭代器范围。

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: 基本启动 / 等待
// ============================================================================

/// @test [async][basic] AsyncTask + run + wait 最小路径。
TEST_CASE("AsyncTask: create -> run -> wait", "[async][basic]") {
    TestEnv env;
    std::atomic<int> n{ 0 };

    auto t = tfl::AsyncTask([&] { n.store(42); });
    REQUIRE_FALSE(t.done());  // 提交前显然未完成

    env.executor.run(t);
    t.wait();
    REQUIRE(n.load() == 42);
    REQUIRE(t.done());
}

/// @test [async][basic] AsyncTask 可在提交前设置名称。
TEST_CASE("AsyncTask: name can be set before run", "[async][basic][name]") {
    TestEnv env;
    auto t = tfl::AsyncTask([] {});
    t.name("payload");
    REQUIRE(t.name() == "payload");

    env.executor.run(t);
    t.wait();
    REQUIRE(t.done());
}

// ============================================================================
// SECTION 2: 依赖链 —— 通过 run(task, deps...) 反向装配
// ============================================================================

/// @test [async][deps] 三级链：t3 依赖 t2，t2 依赖 t1。
/// @brief 反向装配：先声明依赖，再提交底层任务触发级联。
TEST_CASE("AsyncTask: three-level dependency chain", "[async][deps][chain]") {
    TestEnv env;
    std::atomic<int> step{ 0 };
    std::atomic<bool> ok1{ false }, ok2{ false }, ok3{ false };

    auto t1 = tfl::AsyncTask([&] {
        ok1.store(step.exchange(1) == 0);
        });
    auto t2 = tfl::AsyncTask([&] {
        ok2.store(step.exchange(2) == 1);
        });
    auto t3 = tfl::AsyncTask([&] {
        ok3.store(step.exchange(3) == 2);
        });

    // 反向装配：先提交上层（声明依赖），最后提交 t1 触发级联
    env.executor.run(t3, t2);
    env.executor.run(t2, t1);
    env.executor.run(t1);

    t3.wait();
    env.executor.wait_for_all();

    REQUIRE(step.load() == 3);
    REQUIRE(ok1.load());
    REQUIRE(ok2.load());
    REQUIRE(ok3.load());
}

/// @test [async][deps] 扇入：t_sink 依赖 N 个并行任务。
TEST_CASE("AsyncTask: fan-in dependencies", "[async][deps][fan-in]") {
    TestEnv env;
    constexpr int N = 8;
    std::atomic<int> producer_count{ 0 };
    std::atomic<int> sink_seen{ -1 };

    std::vector<tfl::AsyncTask<void>> producers;
    producers.reserve(N);
    for (int i = 0; i < N; ++i) {
        producers.push_back(tfl::AsyncTask([&] {
            producer_count.fetch_add(1);
            }));
    }

    auto sink = tfl::AsyncTask([&] {
        sink_seen.store(producer_count.load());
        });

    // 当前 API 逐个传入前驱句柄，sink 等待所有生产者完成。
    env.executor.run(sink, producers[0], producers[1], producers[2], producers[3],
                     producers[4], producers[5], producers[6], producers[7]);
    for (auto& p : producers) env.executor.run(p);

    sink.wait();
    REQUIRE(producer_count.load() == N);
    REQUIRE(sink_seen.load() == N);
}

// ============================================================================
// SECTION 3: 重复启动 / 状态错误
// ============================================================================

/// @test [async][error] 重复 run 应抛出异常。
TEST_CASE("AsyncTask: duplicate run throws exception", "[async][error][lifecycle]") {
    TestEnv env;
    auto t = tfl::AsyncTask([] {});
    env.executor.run(t);
    REQUIRE_THROWS(env.executor.run(t));  // 第二次提交抛出异常
    t.wait();
}

// ============================================================================
// SECTION 4: 异常传播 —— wait vs get
// ============================================================================

/// @test [async][exception] wait() 不重抛异常，get() 重抛异常。
TEST_CASE("AsyncTask: wait does not rethrow, get rethrows", "[async][exception]") {
    TestEnv env;
    auto t = tfl::AsyncTask([] {
        throw std::runtime_error("intentional");
        });
    env.executor.run(t);

    /// @section wait-silent —— wait 静默丢弃异常
    SECTION("wait silent") {
        REQUIRE_NOTHROW(t.wait());
    }
    /// @section get-rethrows —— get 重抛异常
    SECTION("get rethrows") {
        REQUIRE_THROWS_AS(t.get(), std::runtime_error);
    }
}

// ============================================================================
// SECTION 5: 句柄值语义 —— 引用计数
// ============================================================================

/// @test [async][refcount] 复制句柄不影响生命周期；多个句柄共享同一节点。
TEST_CASE("AsyncTask: copied handles share the same node", "[async][refcount]") {
    TestEnv env;
    std::atomic<int> n{ 0 };
    auto t = tfl::AsyncTask([&] { n.store(7); });
    auto copy = t;            // 复制 +1 引用计数
    REQUIRE(t == copy);       // 指向同一节点

    env.executor.run(t);
    t.wait();
    REQUIRE(copy.done());     // 副本看到相同状态
    REQUIRE(n.load() == 7);
}

// ============================================================================
// SECTION 6: 协作式取消
// ============================================================================

/// @test [async][stop] 设置 request_stop 后，stop_requested() 返回 true。
TEST_CASE("AsyncTask: request_stop after completion", "[async][stop]") {
    TestEnv env;

    auto t = tfl::AsyncTask([&] {
        // 任务在提交后立即检查停止标志 —— 此时尚未请求停止
        });

    env.executor.run(t);
    t.wait();

    // 提交前已完成的空任务，停止尚未被请求
    REQUIRE_FALSE(t.stop_requested());

    /// @section request-stop-after-submission —— 提交后请求停止
    SECTION("request_stop after submission (no-op if already done)") {
        // request_stop 在已完成时也可调用，只是无人响应
        REQUIRE(t.request_stop());
        REQUIRE(t.stop_requested());
        REQUIRE_FALSE(t.request_stop());
    }
}

// ============================================================================
// SECTION 7: 返回值、空句柄与右值链式配置
// ============================================================================

/// @test [async][result] CTAD 推导结果类型，run 保留左值引用或返回右值句柄。
TEST_CASE("AsyncTask: result deduction and run forwarding", "[async][result]") {
    TestEnv env;
    auto task = tfl::AsyncTask([] { return 42; });
    STATIC_REQUIRE(std::same_as<decltype(task), tfl::AsyncTask<int>>);
    STATIC_REQUIRE(std::same_as<decltype(env.executor.run(task)), tfl::AsyncTask<int>&>);
    REQUIRE(&env.executor.run(task) == &task);
    REQUIRE(task.get() == 42);
    auto result = env.executor.run(tfl::AsyncTask([] { return 7; }).name("temporary"));
    REQUIRE(result.get() == 7);
    REQUIRE(result.name() == "temporary");
    tfl::AsyncTask<int> empty{nullptr};
    REQUIRE_THROWS_AS(env.executor.run(empty), tfl::Exception);
}

/// @test [async][semaphore] 右值配置、计数查询、访问、移除和清空。
TEST_CASE("AsyncTask: semaphore configuration overloads", "[async][semaphore]") {
    TestEnv env;
    tfl::Semaphore first{3}, second{2};
    auto task = tfl::AsyncTask([] {}).acquire(first, 2).release(first, 2)
        .acquire(second).release(second).name("limited");
    REQUIRE(task.num_acquires() == 2);
    REQUIRE(task.num_releases() == 2);
    std::size_t permits = 0;
    std::as_const(task).for_each_acquire([&](const tfl::Semaphore&, std::size_t count) { permits += count; });
    REQUIRE(permits == 3);
    task.remove_acquire(second).remove_release(second);
    REQUIRE(task.num_acquires() == 1);
    REQUIRE(task.num_releases() == 1);
    env.executor.run(task).get();
    REQUIRE(first.value() == 3);
    task.clear_acquires().clear_releases();
    REQUIRE(task.num_acquires() == 0);
    REQUIRE(task.num_releases() == 0);
}

/// @test [async][module] 延迟模块任务覆盖单次/次数/谓词及回调构造。
TEST_CASE("AsyncTask: graph constructor overloads", "[async][module]") {
    TestEnv env;
    const int mode = GENERATE(0, 1, 2, 3, 4, 5);
    int count = 0, callbacks = 0;
    tfl::Flow flow;
    (void)flow.emplace([&] { ++count; });
    auto callback = [&] { ++callbacks; };
    auto predicate = [&] { return count == 3; };
    tfl::AsyncTask<void> task;
    switch (mode) {
    case 0: task = tfl::AsyncTask(flow); break;
    case 1: task = tfl::AsyncTask(flow, callback); break;
    case 2: task = tfl::AsyncTask(flow, 3ULL); break;
    case 3: task = tfl::AsyncTask(flow, 3ULL, callback); break;
    case 4: task = tfl::AsyncTask(flow, predicate); break;
    case 5: task = tfl::AsyncTask(flow, predicate, callback); break;
    }
    REQUIRE_FALSE(task.running());
    REQUIRE_FALSE(task.done());
    env.executor.run(task).get();
    REQUIRE(count == (mode < 2 ? 1 : 3));
    REQUIRE(callbacks == (mode % 2));
}
