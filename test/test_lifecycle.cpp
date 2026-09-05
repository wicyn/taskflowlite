/// @file test_lifecycle.cpp
/// @brief 对象生命周期测试 — RAII 探针验证无内存泄漏、正确析构。
///
/// 核心思路：
/// 使用静态原子计数器 `Probe::alive`，在构造/拷贝构造时递增，析构时递减。
/// 最终如果 alive != 0，说明有对象未析构 = 资源泄漏。
///
/// 这种"存活计数"技术是 C++ 测试中的标准实践
/// （参见《Effective Modern C++》、《C++ Templates》以及各大库的测试套件）。

#include "test_common.hpp"

using tfl_test::TestEnv;

namespace {

/// @brief 生命周期探针 — 静态计数器跟踪存活对象数量。
///
/// @details
/// - `alive`：当前存活实例数；测试后必须归零，否则存在泄漏。
/// - `total_constructed`：自上次重置以来的累计构造对象数。
/// - 必须 noexcept 可移动，以满足 std::function/仅移动闭包的语义要求。
struct Probe {
    static inline std::atomic<int> alive{0};
    static inline std::atomic<int> total_constructed{0};
    static inline std::atomic<int> moves{0};

    bool active = true;

    Probe() {
        alive.fetch_add(1, std::memory_order_relaxed);
        total_constructed.fetch_add(1, std::memory_order_relaxed);
    }

    Probe(const Probe&) {
        alive.fetch_add(1, std::memory_order_relaxed);
        total_constructed.fetch_add(1, std::memory_order_relaxed);
    }

    Probe(Probe&& o) noexcept : active(o.active) {
        o.active = false;            // 转移存活计数
        moves.fetch_add(1, std::memory_order_relaxed);
        // alive 不变（本对象 +1，源对象 -1）
    }

    Probe& operator=(const Probe&)            = delete;
    Probe& operator=(Probe&&) noexcept        = delete;

    ~Probe() {
        if (active) {
            alive.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    static void reset() {
        alive.store(0);
        total_constructed.store(0);
        moves.store(0);
    }

    /// @brief 允许 Probe 作为可调用对象被 emplace。
    void operator()() const noexcept { /* nothing */ }
};

}  // namespace

// ============================================================================
// SECTION 1: 基础闭包生命周期
// ============================================================================

/// @section closure-released-after-flow-destruction
/// @test [lifecycle][probe] Flow 销毁后 alive == 0
TEST_CASE("Lifecycle: closure objects released after Flow destruction", "[lifecycle][probe]") {
    Probe::reset();

    {
        TestEnv env;
        tfl::Flow flow;
        for (int i = 0; i < 16; ++i) {
            flow.emplace(Probe{});
        }
        env.executor.async(flow).wait();
        REQUIRE(Probe::alive.load() > 0);  // Flow 内部仍有存活对象
    }
    // Flow 和 Executor 均已销毁

    REQUIRE(Probe::alive.load() == 0);
}

/// @section no-leaks-multiple-submissions
/// @test [lifecycle][probe][reuse] 多次提交不影响生命周期。
TEST_CASE("Lifecycle: no leaks after multiple Flow submissions", "[lifecycle][probe][reuse]") {
    Probe::reset();

    {
        TestEnv env;
        tfl::Flow flow;
        for (int i = 0; i < 8; ++i) flow.emplace(Probe{});

        for (int run = 0; run < 10; ++run) {
            env.executor.async(flow).wait();
        }
    }

    REQUIRE(Probe::alive.load() == 0);
}

// ============================================================================
// SECTION 2: AsyncTask 引用计数
// ============================================================================

/// @section async-task-multi-handle
/// @test [lifecycle][refcount] 多个 AsyncTask 句柄共享同一节点；全部销毁后释放。
TEST_CASE("Lifecycle: multiple AsyncTask handles share a node", "[lifecycle][refcount]") {
    Probe::reset();

    {
        TestEnv env;
        auto t = tfl::AsyncTask(Probe{});
        auto copy1 = t;
        auto copy2 = t;
        // 三个句柄指向同一节点；仅 1 个 Probe 实例（在节点内部）

        env.executor.run(t);
        t.wait();
    }

    REQUIRE(Probe::alive.load() == 0);
}

/// @section silent_async-released-after-execution
/// @test [lifecycle][refcount] silent_async 任务执行后即释放。
TEST_CASE("Lifecycle: silent_async tasks released after execution", "[lifecycle][silent_async]") {
    Probe::reset();

    {
        TestEnv env;
        for (int i = 0; i < 100; ++i) {
            env.executor.silent_async(Probe{});
        }
        env.executor.wait_for_all();
    }

    REQUIRE(Probe::alive.load() == 0);
}

// ============================================================================
// SECTION 3: Subflow 嵌套生命周期
// ============================================================================

/// @section subflow-nesting-no-leak
/// @test [lifecycle][subflow] Subflow 内部的 Probe 也会被释放。
TEST_CASE("Lifecycle: Subflow nesting does not leak", "[lifecycle][subflow]") {
    Probe::reset();

    {
        TestEnv env;
        tfl::Flow inner;
        for (int i = 0; i < 5; ++i) inner.emplace(Probe{});

        tfl::Flow outer;
        outer.emplace(Probe{});
        outer.emplace(std::move(inner), 3ULL);  // 子图运行 3 次
        outer.emplace(Probe{});

        env.executor.async(outer).wait();
    }

    REQUIRE(Probe::alive.load() == 0);
}

// ============================================================================
// SECTION 4: 异常路径上的清理
// ============================================================================

/// @section exception-path-no-leak
/// @test [lifecycle][exception] 发生异常后所有 Probe 均被释放。
/// @details 防止"异常导致部分对象泄漏"这一常见 bug。
TEST_CASE("Lifecycle: exception path does not leak", "[lifecycle][exception]") {
    Probe::reset();

    {
        TestEnv env;
        tfl::Flow flow;
        // 5 个正常节点
        for (int i = 0; i < 5; ++i) {
            flow.emplace([p = Probe{}]() noexcept { (void)p; });
        }
        // 1 个抛异常节点（也持有 Probe）
        flow.emplace([p = Probe{}]() {
            (void)p;
            throw std::runtime_error("planned");
        });

        env.executor.async(flow).wait();
    }

    REQUIRE(Probe::alive.load() == 0);
}

// ============================================================================
// SECTION 5: 大批量构造/析构
// ============================================================================

/// @section mass-construction-destruction-stress
/// @test [lifecycle][stress] 1000 次构造/析构循环
/// @details 在循环中反复"构造 Flow -> 提交 -> 销毁 Flow"。
///          总构造数应等于 RUNS x N，最终 alive == 0。
TEST_CASE("Lifecycle: mass construction/destruction stress", "[lifecycle][stress]") {
    Probe::reset();

    constexpr int RUNS = 1000;
    constexpr int N = 4;

    {
        TestEnv env;
        for (int r = 0; r < RUNS; ++r) {
            tfl::Flow flow;
            for (int i = 0; i < N; ++i) flow.emplace(Probe{});
            env.executor.async(flow).wait();
        }
    }

    REQUIRE(Probe::alive.load() == 0);
    // 至少 RUNS x N 次构造（实际次数可能因移动构造而更高）
    REQUIRE(Probe::total_constructed.load() >= RUNS * N);
}

// ============================================================================
// SECTION 6: 移动语义 — Flow / Executor 不可拷贝特性
// ============================================================================

/// @section type-trait-checks
/// @test [lifecycle][move-only] Flow 仅可移动，Executor 不可移动。
/// @details 编译期断言；无运行时检查。
TEST_CASE("Lifecycle: type trait checks", "[lifecycle][traits]") {
    // Flow：不可拷贝
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<tfl::Flow>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<tfl::Flow>);

    // Executor：不可拷贝、不可移动（持有线程池）
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<tfl::Executor>);
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<tfl::Executor>);

    // Task / TaskView：可拷贝（弱引用）
    STATIC_REQUIRE(std::is_copy_constructible_v<tfl::Task>);
    STATIC_REQUIRE(std::is_copy_assignable_v<tfl::Task>);

    // Semaphore：同样不可拷贝/移动（节点间共享指针）
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<tfl::Semaphore>);
}
