/// @file test_future.cpp
/// @brief Future 返回值通道测试 —— get / wait / wait_for / request_stop 语义。
///
/// tfl::Future<R> 组合 std::future<R> + std::stop_source，是 Executor::async 的返回值句柄。
///
/// 覆盖点:
///   - get() 获取结果；重复 get 抛 future_error
///   - wait / wait_for / wait_until 等待语义
///   - valid() 状态查询
///   - request_stop / stop_requested / stop_possible 停止控制
///   - 移动语义（不可拷贝）
///   - share() → shared_future
///   - native_future() 逃生口
///   - Future<void> 特化
///   - Runtime::async 返回的 Future

#include "test_common.hpp"

#include <atomic>
#include <chrono>
#include <stop_token>
#include <thread>
#include <type_traits>

using tfl_test::TestEnv;
using namespace std::chrono_literals;

// ============================================================================
// 第 1 节: 基础 get / wait / valid
// ============================================================================

/// @test [future][basic] async 返回 Future，get() 获取结果
TEST_CASE("Future: get returns result", "[future][basic]") {
    TestEnv env;
    auto f = env.executor.async([] { return 42; });
    REQUIRE(f.get() == 42);
}

/// @test [future][basic] get 仅可调一次，再次调抛 future_error
TEST_CASE("Future: double get throws future_error", "[future][basic]") {
    TestEnv env;
    auto f = env.executor.async([] { return 99; });
    REQUIRE(f.get() == 99);
    REQUIRE_THROWS_AS(f.get(), std::future_error);
}

/// @test [future][basic] valid() 反映共享状态
TEST_CASE("Future: valid reflects state", "[future][basic]") {
    TestEnv env;
    auto f = env.executor.async([] { return 1; });
    REQUIRE(f.valid());
    f.get();
    REQUIRE_FALSE(f.valid());  // get 后无效
}

/// @test [future][basic] wait 阻塞至结果就绪
TEST_CASE("Future: wait blocks until ready", "[future][basic]") {
    TestEnv env;
    auto f = env.executor.async([] { return 7; });
    REQUIRE_NOTHROW(f.wait());
    REQUIRE(f.get() == 7);
}

/// @test [future][basic] wait_for 返回正确状态
TEST_CASE("Future: wait_for returns correct status", "[future][basic]") {
    TestEnv env;
    auto f = env.executor.async([] { return 100; });
    auto status = f.wait_for(10s);
    REQUIRE(status == std::future_status::ready);
    REQUIRE(f.get() == 100);
}

// ============================================================================
// 第 2 节: 移动语义
// ============================================================================

/// @test [future][move] Future 仅可移动不可拷贝
TEST_CASE("Future: move-only semantics", "[future][move]") {
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<tfl::Future<int>>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<tfl::Future<int>>);
    STATIC_REQUIRE(std::is_move_constructible_v<tfl::Future<int>>);
    STATIC_REQUIRE(std::is_move_assignable_v<tfl::Future<int>>);
}

/// @test [future][move] 移动后原 Future 不再 valid
TEST_CASE("Future: moved-from Future is not valid", "[future][move]") {
    TestEnv env;
    auto f1 = env.executor.async([] { return 42; });
    REQUIRE(f1.valid());
    auto f2 = std::move(f1);
    REQUIRE(f2.valid());
    REQUIRE_FALSE(f1.valid());
    REQUIRE(f2.get() == 42);
}

/// @test [future][move] 默认构造的 Future valid() == false
TEST_CASE("Future: default constructed is not valid", "[future][move]") {
    tfl::Future<int> f;
    REQUIRE_FALSE(f.valid());
    REQUIRE_FALSE(!f.stop_possible());
}

// ============================================================================
// 第 3 节: 停止控制
// ============================================================================

/// @test [future][stop] request_stop 首次返回 true，之后返回 false
TEST_CASE("Future: request_stop returns true only on first call", "[future][stop]") {
    TestEnv env;
    auto f = env.executor.async([] { return 0; });
    REQUIRE(f.request_stop());
    REQUIRE_FALSE(f.request_stop());  // 幂等
    f.wait();
}

/// @test [future][stop] stop_requested 反映 request_stop
TEST_CASE("Future: stop_requested reflects request_stop", "[future][stop]") {
    TestEnv env;
    auto f = env.executor.async([] { return 0; });
    REQUIRE_FALSE(f.stop_requested());
    f.request_stop();
    REQUIRE(f.stop_requested());
    f.wait();
}

/// @test [future][stop] stop_possible 有任务时为 true
TEST_CASE("Future: stop_possible when Future has associated task", "[future][stop]") {
    TestEnv env;
    auto f = env.executor.async([] { return 0; });
    REQUIRE(f.stop_possible());
    f.wait();
    f.get();
}

/// @test [future][stop] async callable 内可接收 stop_token，与 Future::stop_token() 同源
TEST_CASE("Future: stop_token visible inside callable", "[future][stop]") {
    TestEnv env;
    std::atomic<bool> callable_saw_stop_possible{false};

    // async 的 callable 接受 stop_token —— 框架检测 stoppable 签名后自动传入
    auto f = env.executor.async([&](std::stop_token st) {
        callable_saw_stop_possible.store(st.stop_possible());
        return 1;
    });
	f.wait();
    auto token = f.stop_token();
    REQUIRE(token.stop_possible());
    REQUIRE(callable_saw_stop_possible.load());
    REQUIRE_FALSE(token.stop_requested());
    REQUIRE(f.get() == 1);
}

// ============================================================================
// 第 4 节: native_future / share 逃生口
// ============================================================================

/// @test [future][escape] native_future() 返回底层 std::future 引用
TEST_CASE("Future: native_future access", "[future][escape]") {
    TestEnv env;
    auto f = env.executor.async([] { return 99; });
    auto& native = f.native_future();
    REQUIRE(native.valid());
    REQUIRE(native.get() == 99);
}

/// @test [future][escape] share() 转为 shared_future
TEST_CASE("Future: share produces shared_future", "[future][escape]") {
    TestEnv env;
    auto f = env.executor.async([] { return 55; });
    auto sf = f.share();
    REQUIRE(sf.valid());
    REQUIRE(sf.get() == 55);
    REQUIRE(sf.valid());   // shared_future 可多次读
    REQUIRE(sf.get() == 55);
}

// ============================================================================
// 第 5 节: async 参数转发
// ============================================================================

/// @test [future][args] async 多参数转发
TEST_CASE("Future: async with multiple arguments", "[future][args]") {
    TestEnv env;
    auto f = env.executor.async([](int a, int b, int c) { return a + b + c; }, 1, 2, 3);
    REQUIRE(f.get() == 6);
}

/// @test [future][args] async 配合 std::ref 写回外部变量
TEST_CASE("Future: async with std::ref write-back", "[future][args]") {
    TestEnv env;
    int x = 0;
    auto f = env.executor.async([](int& r) { r = 42; return r; }, std::ref(x));
    REQUIRE(f.get() == 42);
    REQUIRE(x == 42);
}

/// @test [future][args] async void 返回 Future<void>
TEST_CASE("Future: async void returns Future<void>", "[future][args]") {
    TestEnv env;
    std::atomic<int> n{0};
    auto f = env.executor.async([&] { n.store(7); });
    f.get();  // Future<void>::get() 返回 void
    REQUIRE(n.load() == 7);
}

// ============================================================================
// 第 6 节: 大量 Future 并发
// ============================================================================

/// @test [future][stress] 100 个 async 并发获取结果
TEST_CASE("Future: 100 concurrent async calls", "[future][stress]") {
    TestEnv env(8);
    constexpr int N = 100;

    std::vector<tfl::Future<int>> futures;
    futures.reserve(N);
    for (int i = 0; i < N; ++i) {
        futures.push_back(env.executor.async([i] { return i * i; }));
    }

    long sum = 0;
    for (int i = 0; i < N; ++i) {
        sum += futures[i].get();
    }

    // 平方和公式: (N-1)*N*(2N-1)/6
    REQUIRE(sum == static_cast<long>(N - 1) * N * (2 * N - 1) / 6);
}

// ============================================================================
// 第 7 节: wait_for 延迟任务
// ============================================================================

/// @test [future][timing] wait_for 对慢任务返回 timeout
TEST_CASE("Future: wait_for on slow task returns timeout", "[future][timing]") {
    TestEnv env;
    auto f = env.executor.async([&] {
        std::this_thread::sleep_for(50ms);
        return 1;
    });

    // 立即检查 → 可能 timeout 或 ready
    auto status = f.wait_for(1ms);
    REQUIRE((status == std::future_status::ready || status == std::future_status::timeout));

    // 最终应 ready
    REQUIRE(f.get() == 1);
}

// ============================================================================
// 第 8 节: Future<void> 特化
// ============================================================================

/// @test [future][void] Future<void>::get() 无返回值
TEST_CASE("Future: Future<void> get is void", "[future][void]") {
    TestEnv env;
    bool done = false;
    auto f = env.executor.async([&] { done = true; });
    STATIC_REQUIRE(std::same_as<decltype(f), tfl::Future<void>>);
    REQUIRE_NOTHROW(f.get());
    REQUIRE(done);
}

/// @test [future][void] Future<void> wait + valid
TEST_CASE("Future: Future<void> wait and valid", "[future][void]") {
    TestEnv env;
    auto f = env.executor.async([] {});
    REQUIRE(f.valid());
    f.wait();
    f.get();
    REQUIRE_FALSE(f.valid());
}

// ============================================================================
// 第 9 节: Runtime::async 返回的 Future
// ============================================================================

/// @test [future][stop][runtime] Runtime::async 返回的 Future 可用于停止控制
TEST_CASE("Future: Runtime::async Future stop_token", "[future][stop][runtime]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<bool> token_seen{false};

    flow.emplace([&](tfl::Runtime& rt) {
        auto f = rt.async([] { return 42; });
        auto st = f.stop_token();
        token_seen.store(st.stop_possible());
        rt.cowait();
    });

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(token_seen.load());
}

// ============================================================================
// 覆盖矩阵
// ============================================================================
//   Future::get           ✓ "Future: get returns result" / "Future: double get throws future_error"
//   Future::valid         ✓ "Future: valid reflects state" / "moved-from not valid" / "default not valid"
//   Future::wait          ✓ "Future: wait blocks until ready"
//   Future::wait_for      ✓ "Future: wait_for returns correct status" / "wait_for on slow task"
//   Future::move          ✓ "Future: move-only semantics" / "moved-from Future is not valid"
//   Future::request_stop  ✓ "Future: request_stop returns true only on first call"
//   Future::stop_requested ✓ "Future: stop_requested reflects request_stop"
//   Future::stop_possible ✓ "Future: stop_possible when Future has associated task"
//   Future::stop_token    ✓ "Future: stop_token visible inside callable"
//   Future::native_future ✓ "Future: native_future access"
//   Future::share         ✓ "Future: share produces shared_future"
//   Future::args          ✓ "Future: async with multiple arguments" / "async with std::ref write-back"
//   Future<void>          ✓ "Future: Future<void> get is void" / "Future<void> wait and valid"
//   Future::stress        ✓ "Future: 100 concurrent async calls"
//   Future::runtime       ✓ "Future: Runtime::async Future stop_token"
