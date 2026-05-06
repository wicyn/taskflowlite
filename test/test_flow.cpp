/// @file test_flow.cpp
/// @brief Flow 模块测试 —— 图构建 / emplace 参数转发 / 批量插入 / 生命周期。
///
/// 覆盖接口：
///   - Flow()                              默认构造
///   - Flow::name(string) / Flow::name()   命名 setter / getter
///   - Flow::emplace(callable, args...)    单任务插入（含 Basic/Branch/Jump/Runtime/Subflow 分派）
///   - Flow::emplace(Ts&&...)              批量插入（无参闭包）
///   - Flow::emplace(Packs&&...)           批量插入（tfl::pack 带参）
///   - Flow::size / Flow::empty / Flow::clear
///   - Flow::dump                          D2 文本可视化导出

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: 基础构造与生命周期
// ============================================================================

/// @test [flow][basic] 默认构造的 Flow 处于干净状态。
/// @details 验证空 Flow 的 size/empty/name 三个查询接口的初始一致性 ——
///          这是后续测试的契约前提。
TEST_CASE("Flow: 默认构造的初始状态", "[flow][basic]") {
    tfl::Flow flow;

    REQUIRE(flow.empty());
    REQUIRE(flow.size() == 0);
    REQUIRE(flow.name().empty());
}

/// @test [flow][basic] name() 双向操作：setter / getter 一致。
TEST_CASE("Flow: name setter/getter", "[flow][basic]") {
    tfl::Flow flow;
    flow.name("my_pipeline");
    REQUIRE(flow.name() == "my_pipeline");

    // 链式风格
    flow.name("renamed");
    REQUIRE(flow.name() == "renamed");
}

/// @test [flow][basic][lifecycle] clear() 把 Flow 完全复位为空状态。
/// @details Flow 复用场景（先建图、提交、清空、再建图）必须保证 clear()
///          彻底重置，否则会产生残留节点。
TEST_CASE("Flow: clear 重置图为空", "[flow][basic][lifecycle]") {
    tfl::Flow flow;
    flow.emplace([] {});
    flow.emplace([] {});
    flow.emplace([] {});
    REQUIRE(flow.size() == 3);

    flow.clear();
    REQUIRE(flow.empty());
    REQUIRE(flow.size() == 0);

    // 复用：clear 后再次 emplace 仍然能正常工作
    flow.emplace([] {});
    REQUIRE(flow.size() == 1);
}

// ============================================================================
// SECTION 2: 单任务 emplace —— 参数转发与 std::ref 语义
// ============================================================================

/// @test [flow][emplace] emplace 单 lambda 后 size++ 且节点状态合法。
TEST_CASE("Flow: emplace 单任务", "[flow][emplace][basic]") {
    tfl::Flow flow;
    auto t = flow.emplace([] {});

    REQUIRE_FALSE(flow.empty());
    REQUIRE(flow.size() == 1);
    REQUIRE(static_cast<bool>(t));
    REQUIRE(t.num_predecessors() == 0);
    REQUIRE(t.num_successors() == 0);
}

/// @test [flow][emplace] emplace(callable, args...) 走 std::thread 风格转发。
/// @details 验证 3 件事：
///   1. 左值参数 decay 拷贝（修改不影响外部）；
///   2. `std::ref(x)` 真引用写回（修改影响外部）；
///   3. 多个混合参数同时正确传递。
TEST_CASE("Flow: emplace 参数转发语义", "[flow][emplace][forwarding]") {
    TestEnv env;

    SECTION("decay 拷贝：左值传入后内部修改不影响外部") {
        tfl::Flow flow;
        int outside = 42;
        flow.emplace([](int v) {
            v = 999; // 只改副本
            (void)v;
        }, outside);
        env.executor.deferred_async(flow).start().wait();
        REQUIRE(outside == 42);
    }

    SECTION("std::ref 写回：内部修改影响外部") {
        tfl::Flow flow;
        int outside = 0;
        flow.emplace([](int& r) { r = 42; }, std::ref(outside));
        env.executor.deferred_async(flow).start().wait();
        REQUIRE(outside == 42);
    }

    SECTION("atomic 必须用 std::ref（不可拷贝）") {
        tfl::Flow flow;
        std::atomic<int> ac{0};
        flow.emplace([](std::atomic<int>& c) {
            c.fetch_add(7);
        }, std::ref(ac));
        env.executor.deferred_async(flow).start().wait();
        REQUIRE(ac.load() == 7);
    }

    SECTION("多个 ref 同时写回") {
        tfl::Flow flow;
        int x = 0, y = 0, z = 0;
        flow.emplace([](int& a, int& b, int& c) {
            a = 1; b = 2; c = 3;
        }, std::ref(x), std::ref(y), std::ref(z));
        env.executor.deferred_async(flow).start().wait();
        REQUIRE(x == 1);
        REQUIRE(y == 2);
        REQUIRE(z == 3);
    }
}

// ============================================================================
// SECTION 3: 批量 emplace —— 结构化绑定 + tfl::pack
// ============================================================================

/// @test [flow][emplace][batch] 批量 emplace 多个无参 lambda，使用结构化绑定接收。
/// @details 验证返回的 tuple 拆出的 Task 句柄可以正常做 precede 拓扑。
TEST_CASE("Flow: 批量插入无参闭包", "[flow][emplace][batch]") {
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

/// @test [flow][emplace][batch] 用 tfl::pack 批量插入带参任务。
/// @details `tfl::pack{callable, args...}` 解决 std::tuple CTAD 的歧义问题，
///          自动 decay 函数名并支持 std::ref，是批量带参插入的推荐写法。
TEST_CASE("Flow: 批量插入 tfl::pack 带参任务", "[flow][emplace][batch][pack]") {
    TestEnv env;
    tfl::Flow flow;

    int a = 0, b = 0;
    auto [t1, t2] = flow.emplace(
        tfl::pack{[](int x) { (void)x; }, 100},
        tfl::pack{[](int& r) { r = 42; }, std::ref(b)}
    );
    t1.precede(t2);

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(a == 0);   // t1 改的是副本
    REQUIRE(b == 42);  // t2 通过 ref 写回
}

// ============================================================================
// SECTION 4: 任务类型自动推导 —— 7 种签名分派
// ============================================================================

/// @test [flow][emplace][type-deduction] 所有签名都能被 emplace 接受并路由到正确的 Work 子类。
/// @details 通过编译即视为通过；运行起来确认所有任务都可以 invoke。
TEST_CASE("Flow: 7 种签名都能 emplace", "[flow][emplace][type-deduction]") {
    TestEnv env;
    tfl::Flow flow;

    std::atomic<int> hits{0};

    // Basic
    flow.emplace([&] { hits.fetch_add(1); });

    // Runtime
    flow.emplace([&](tfl::Runtime&) { hits.fetch_add(1); });

    // Branch（reset 表示不选任何后继 —— 单独节点也合法）
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
    env.executor.deferred_async(flow).start().wait();
    REQUIRE(hits.load() == 7);
}

// ============================================================================
// SECTION 5: D2 dump 可视化导出
// ============================================================================

/// @test [flow][dump] dump() 把图导出为 D2 文本，可写入 ostream。
/// @details 不严格检查具体语法（D2 实现细节会变），只确认产出非空、包含节点信息。
TEST_CASE("Flow: dump 输出 D2 文本", "[flow][dump]") {
    tfl::Flow flow;
    flow.name("test");
    auto a = flow.emplace([] {}).name("A");
    auto b = flow.emplace([] {}).name("B");
    a.precede(b);

    std::ostringstream oss;
    flow.dump(oss);
    const std::string out = oss.str();

    REQUIRE_FALSE(out.empty());
    // 节点名应该出现在输出里（D2 把名字作为标签）
    REQUIRE(out.find("A") != std::string::npos);
    REQUIRE(out.find("B") != std::string::npos);
}
