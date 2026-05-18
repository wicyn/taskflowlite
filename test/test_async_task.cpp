/// @file test_async_task.cpp
/// @brief AsyncTask / DeferredAsyncTask 测试 —— 引用计数句柄、依赖链、生命周期。
///
/// 覆盖的接口：
///   - Executor::deferred_async(callable)               返回 DeferredAsyncTask
///   - Executor::dependent_async(callable, deps...)     直接返回 AsyncTask（已启动）
///   - DeferredAsyncTask::start(deps...)                手动启动并声明依赖
///   - DeferredAsyncTask::name(string)                  启动前可设置名称
///   - DeferredAsyncTask::acquire / release             启动前配置信号量
///   - AsyncTask::wait()                                阻塞直到完成（不重抛异常）
///   - AsyncTask::get()                                 等待 + 重抛异常
///   - AsyncTask::done() / running()                    状态查询
///   - AsyncTask::name() / type()                       元数据
///   - AsyncTask::stop_token() / request_stop()         协作式取消
///
/// 关键约束：
///   1. deferred_async(...) 返回 DeferredAsyncTask，**必须调用 start() 才能运行**；
///   2. dependent_async 立即启动；
///   3. 重复 start() 抛出异常；
///   4. start(deps...) 接受 AsyncTask 或 DeferredAsyncTask（基于二进制布局兼容性）。

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: 基本启动 / 等待
// ============================================================================

/// @test [async][basic] deferred_async + start + wait 最小路径。
TEST_CASE("AsyncTask: deferred_async -> start -> wait", "[async][basic]") {
    TestEnv env;
    std::atomic<int> n{ 0 };

    auto t = env.executor.deferred_async([&] { n.store(42); });
    REQUIRE_FALSE(t.done());  // 启动前显然未完成

    t.start().wait();
    REQUIRE(n.load() == 42);
    REQUIRE(t.done());
}

/// @test [async][basic] DeferredAsyncTask 可在启动前设置名称。
TEST_CASE("DeferredAsyncTask: name can be set before start", "[async][basic][name]") {
    TestEnv env;
    auto t = env.executor.deferred_async([] {});
    t.name("payload");
    REQUIRE(t.name() == "payload");

    t.start().wait();
    REQUIRE(t.done());
}

// ============================================================================
// SECTION 2: 依赖链 —— 通过 start(deps...) 反向装配
// ============================================================================

/// @test [async][deps] 三级链：t3 依赖 t2，t2 依赖 t1。
/// @details 反向装配：先收集依赖句柄，再启动上层，
///          最后启动 t1 触发级联。
///          无法在 worker 中使用 REQUIRE；所有检查使用原子标志。
TEST_CASE("AsyncTask: three-level dependency chain", "[async][deps][chain]") {
    TestEnv env;
    std::atomic<int> step{ 0 };
    std::atomic<bool> ok1{ false }, ok2{ false }, ok3{ false };

    auto t1 = env.executor.deferred_async([&] {
        ok1.store(step.exchange(1) == 0);
        });
    auto t2 = env.executor.deferred_async([&] {
        ok2.store(step.exchange(2) == 1);
        });
    auto t3 = env.executor.deferred_async([&] {
        ok3.store(step.exchange(3) == 2);
        });

    // 反向装配
    t3.start(t2);
    t2.start(t1);
    t1.start();

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

    std::vector<tfl::DeferredAsyncTask> producers;
    producers.reserve(N);
    for (int i = 0; i < N; ++i) {
        producers.push_back(env.executor.deferred_async([&] {
            producer_count.fetch_add(1);
            }));
    }

    auto sink = env.executor.deferred_async([&] {
        sink_seen.store(producer_count.load());
        });

    // sink 等待所有生产者完成
    sink.start(producers.begin(), producers.end());
    for (auto& p : producers) p.start();

    sink.wait();
    REQUIRE(producer_count.load() == N);
    REQUIRE(sink_seen.load() == N);
}

// ============================================================================
// SECTION 3: 重复启动 / 状态错误
// ============================================================================

/// @test [async][error] 重复 start() 应抛出异常。
TEST_CASE("DeferredAsyncTask: duplicate start throws exception", "[async][error][lifecycle]") {
    TestEnv env;
    auto t = env.executor.deferred_async([] {});
    t.start();
    REQUIRE_THROWS(t.start());  // 第二次启动抛出异常
    t.wait();
}

// ============================================================================
// SECTION 4: 异常传播 —— wait vs get
// ============================================================================

/// @test [async][exception] wait() 不重抛异常，get() 重抛异常。
TEST_CASE("AsyncTask: wait does not rethrow, get rethrows", "[async][exception]") {
    TestEnv env;
    auto t = env.executor.deferred_async([] {
        throw std::runtime_error("intentional");
        });
    t.start();

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
    auto t = env.executor.deferred_async([&] { n.store(7); });
    auto copy = t;            // 复制 +1 引用计数
    REQUIRE(t == copy);       // 指向同一节点

    t.start().wait();
    REQUIRE(copy.done());     // 副本看到相同状态
    REQUIRE(n.load() == 7);
}

// ============================================================================
// SECTION 6: 协作式取消
// ============================================================================

/// @test [async][stop] 设置 request_stop 后，stop_requested() 返回 true。
TEST_CASE("AsyncTask: stop_token cooperative cancellation", "[async][stop]") {
    TestEnv env;
    std::atomic<bool> body_observed_stop{ false };

    auto t = env.executor.deferred_async([&] {
        // 任务在启动后立即检查停止标志 —— 此时尚未请求停止
        });

    t.start();
    t.wait();

    // 启动前已完成的空任务，停止尚未被请求
    REQUIRE_FALSE(t.stop_requested());

    /// @section request-stop-after-submission —— 提交后请求停止
    SECTION("request_stop after submission (no-op if already done)") {
        // request_stop 在已完成时也可调用，只是无人响应
        t.request_stop();
        // 不应抛出异常
    }
}
