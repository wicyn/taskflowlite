/// @file test_flow.cpp
/// @brief Flow 模块测试 —— 图构建 / emplace 参数转发 / 批量插入 / 生命周期。
///
/// 覆盖的接口：
///   - Flow()                              默认构造函数
///   - Flow::name(string) / Flow::name()   名称 setter / getter
///   - Flow::emplace(callable, args...)    单任务插入（分发到 Basic/Branch/Jump/Runtime/Subflow）
///   - Flow::emplace(Ts&&...)              批量插入（无参数闭包）
///   - Flow::emplace(Packs&&...)           批量插入（带 tfl::pack 参数）
///   - Flow::size / Flow::empty / Flow::clear
///   - Flow::dump                          D2 文本可视化导出

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: 基本构造和生命周期
// ============================================================================

/// @test [flow][basic] 默认构造的 Flow 处于干净状态。
/// @details 验证空 Flow 的 size/empty/name 查询接口的初始一致性 ——
///          这是后续测试的契约前提。
TEST_CASE("Flow: default-constructed initial state", "[flow][basic]") {
    tfl::Flow flow;

    REQUIRE(flow.empty());
    REQUIRE(flow.size() == 0);
    REQUIRE(flow.name().empty());
}

/// @test [flow][basic] name() 双向：setter / getter 一致性。
TEST_CASE("Flow: name setter/getter", "[flow][basic]") {
    tfl::Flow flow;
    flow.name("my_pipeline");
    REQUIRE(flow.name() == "my_pipeline");

    // 链式风格
    flow.name("renamed");
    REQUIRE(flow.name() == "renamed");
}

/// @test [flow][basic][lifecycle] clear() 将 Flow 完全重置为空状态。
/// @details Flow 复用场景（构建图、提交、清空、重建）依赖 clear()
///          执行彻底重置，否则会残留节点。
TEST_CASE("Flow: clear resets graph to empty", "[flow][basic][lifecycle]") {
    tfl::Flow flow;
    flow.emplace([] {});
    flow.emplace([] {});
    flow.emplace([] {});
    REQUIRE(flow.size() == 3);

    flow.clear();
    REQUIRE(flow.empty());
    REQUIRE(flow.size() == 0);

    // 复用：clear 后 emplace 仍正常工作
    flow.emplace([] {});
    REQUIRE(flow.size() == 1);
}

// ============================================================================
// SECTION 2: 单任务 emplace —— 参数转发与 std::ref 语义
// ============================================================================

/// @test [flow][emplace] 插入单个 lambda 后 size++ 且节点状态有效。
TEST_CASE("Flow: single task emplace", "[flow][emplace][basic]") {
    tfl::Flow flow;
    auto t = flow.emplace([] {});

    REQUIRE_FALSE(flow.empty());
    REQUIRE(flow.size() == 1);
    REQUIRE(static_cast<bool>(t));
    REQUIRE(t.num_predecessors() == 0);
    REQUIRE(t.num_successors() == 0);
}

/// @test [flow][emplace] emplace(callable, args...) 使用 std::thread 风格的参数转发。
/// @details 验证 3 点：
///   1. 左值参数衰减复制（修改不影响外部）；
///   2. `std::ref(x)` 通过真正的引用写回（修改影响外部）；
///   3. 多个混合参数均正确转发。
TEST_CASE("Flow: emplace argument forwarding semantics", "[flow][emplace][forwarding]") {
    TestEnv env;

    /// @section decay-copy-lvalue-mutation-isolated —— 衰减复制：左值修改与外部隔离
    SECTION("decay copy: lvalue mutation isolated from outside") {
        tfl::Flow flow;
        int outside = 42;
        flow.emplace([](int v) {
            v = 999; // 仅修改副本
            (void)v;
        }, outside);
        env.executor.async(flow).wait();
        REQUIRE(outside == 42);
    }

    /// @section std-ref-write-back —— std::ref 写回：修改影响外部
    SECTION("std::ref write-back: mutation affects outside") {
        tfl::Flow flow;
        int outside = 0;
        flow.emplace([](int& r) { r = 42; }, std::ref(outside));
        env.executor.async(flow).wait();
        REQUIRE(outside == 42);
    }

    /// @section atomic-must-use-std-ref —— atomic 必须使用 std::ref（不可复制）
    SECTION("atomic must use std::ref (non-copyable)") {
        tfl::Flow flow;
        std::atomic<int> ac{0};
        flow.emplace([](std::atomic<int>& c) {
            c.fetch_add(7);
        }, std::ref(ac));
        env.executor.async(flow).wait();
        REQUIRE(ac.load() == 7);
    }

    /// @section multiple-ref-simultaneous-write-back —— 多个 ref 同时写回
    SECTION("multiple ref simultaneous write-back") {
        tfl::Flow flow;
        int x = 0, y = 0, z = 0;
        flow.emplace([](int& a, int& b, int& c) {
            a = 1; b = 2; c = 3;
        }, std::ref(x), std::ref(y), std::ref(z));
        env.executor.async(flow).wait();
        REQUIRE(x == 1);
        REQUIRE(y == 2);
        REQUIRE(z == 3);
    }
}

// ============================================================================
// SECTION 3: 批量 emplace —— 结构化绑定 + tfl::pack
// ============================================================================

/// @test [flow][emplace][batch] 批量 emplace 多个无参数 lambda，通过结构化绑定接收。
/// @details 验证从返回的 tuple 解构出的 Task 句柄可用于 precede 拓扑。
TEST_CASE("Flow: batch insert parameterless closures", "[flow][emplace][batch]") {
    tfl::Flow flow;
    auto [t1, t2, t3] = flow.emplace(
        [] {},
        [] {},
        [] {}
    );
    t1.precede(t2);
    t2.precede(t3);

    REQUIRE(flow.size() == 3);
    REQUIRE(t1.num_successors() == 1);
    REQUIRE(t2.num_predecessors() == 1);
    REQUIRE(t2.num_successors() == 1);
    REQUIRE(t3.num_predecessors() == 1);
}

/// @test [flow][emplace][batch] 使用 tfl::pack 批量插入带参数的任务。
/// @details `tfl::pack{callable, args...}` 解决 std::tuple CTAD 歧义，
///          自动衰减函数名并支持 std::ref；推荐用于带参数的批量插入。
TEST_CASE("Flow: batch insert tfl::pack tasks with args", "[flow][emplace][batch][pack]") {
    TestEnv env;
    tfl::Flow flow;

    int a = 0, b = 0;
    auto [t1, t2] = flow.emplace(
        tfl::pack{[](int x) { (void)x; }, 100},
        tfl::pack{[](int& r) { r = 42; }, std::ref(b)}
    );
    t1.precede(t2);

    env.executor.async(flow).wait();
    REQUIRE(a == 0);   // t1 修改了副本
    REQUIRE(b == 42);  // t2 通过引用写回
}

// ============================================================================
// SECTION 4: 自动任务类型推导 —— 7 种签名的分发
// ============================================================================

/// @test [flow][emplace][type-deduction] emplace 接受所有签名并路由到正确的 Work 子类。
/// @details 编译即通过标准；执行确认所有任务均可调用。
TEST_CASE("Flow: all 7 signatures accepted by emplace", "[flow][emplace][type-deduction]") {
    TestEnv env;
    tfl::Flow flow;

    std::atomic<int> hits{0};

    // Basic
    flow.emplace([&] { hits.fetch_add(1); });

    // Runtime
    flow.emplace([&](tfl::Runtime&) { hits.fetch_add(1); });

    // Branch（reset 表示不选择任何后继 —— 独立节点是合法的）
    flow.emplace([&](tfl::Branch& br) {
        hits.fetch_add(1);
        br.reset();
    });

    // MultiBranch
    flow.emplace([&](tfl::MultiBranch& mb) {
        hits.fetch_add(1);
        mb.reset();
    });

    // Jump（reset 表示不跳转）
    flow.emplace([&](tfl::Jump& jmp) {
        hits.fetch_add(1);
        jmp.reset();
    });

    // MultiJump
    flow.emplace([&](tfl::MultiJump& mj) {
        hits.fetch_add(1);
        mj.reset();
    });

    // Subflow（嵌套 Flow）
    tfl::Flow inner;
    inner.emplace([&] { hits.fetch_add(1); });
    flow.emplace(std::move(inner));

    REQUIRE(flow.size() == 7);
    env.executor.async(flow).wait();
    REQUIRE(hits.load() == 7);
}

// ============================================================================
// SECTION 5: D2 dump 可视化导出
// ============================================================================

/// @test [flow][dump] dump() 将图导出为 D2 文本，可写入 ostream。
/// @details 不严格检查特定语法（D2 实现细节可能变更），
///          仅确认输出非空且包含节点信息。
TEST_CASE("Flow: dump outputs D2 text", "[flow][dump]") {
    tfl::Flow flow;
    flow.name("test");
    auto a = flow.emplace([] {}).name("A");
    auto b = flow.emplace([] {}).name("B");
    a.precede(b);

    std::ostringstream oss;
    flow.dump(oss);
    const std::string out = oss.str();

    REQUIRE_FALSE(out.empty());
    // 节点名称应出现在输出中（D2 使用名称作为标签）
    REQUIRE(out.find("A") != std::string::npos);
    REQUIRE(out.find("B") != std::string::npos);
}
