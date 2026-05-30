/// @file test_branch.cpp
/// @brief Branch / MultiBranch 测试 —— 条件路由，协作式多路径释放。
/// @author wicyn
///
/// 覆盖的接口（Branch 单目标排他）：
///   - Branch::select(index)                  按索引选择单个后继
///   - Branch::select_if(pred)                选择第一个谓词匹配项
///   - Branch::reset()                        清除选择（释放 0 个后继）
///   - Branch::operator[](index)              下标代理，赋值 bool
///   - Branch::size()                         后继数量
///
/// 覆盖的接口（MultiBranch 多目标累积）：
///   - MultiBranch::select(i, j, k...)        多索引同时释放
///   - MultiBranch::select_if(pred)           选择所有匹配项
///   - MultiBranch::select_all()              广播选择全部
///   - MultiBranch::reset()                   清除累积的选择
///   - MultiBranch::operator[](i)             单索引代理
///   - MultiBranch::operator[](i, j, ...)     多索引代理（编译期长度检查）
///
/// Branch 使用"协作式（差值）"语义：未选中的后继不会永久阻塞；
/// 但本测试中 Branch 节点是其所有后继的唯一前驱，因此未选中即意味着不执行。

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: Branch —— 单目标选择
// ============================================================================

/// @test [branch][select] Branch::select(i) 仅释放索引 i 处的后继。
TEST_CASE("Branch: select(index) only releases a single successor", "[branch][select]") {
    TestEnv env;

    for (int target = 0; target < 3; ++target) {
        tfl::Flow flow;
        std::atomic<int> ran[3] = { 0, 0, 0 };

        auto br = flow.emplace([target](tfl::Branch& b) { b.select(target); });
        auto p0 = flow.emplace([&] { ran[0].store(1); });
        auto p1 = flow.emplace([&] { ran[1].store(1); });
        auto p2 = flow.emplace([&] { ran[2].store(1); });

        br.precede(p0, p1, p2);

        env.executor.async(flow).wait();

        for (int i = 0; i < 3; ++i) {
            REQUIRE(ran[i].load() == (i == target ? 1 : 0));
        }
    }
}

/// @test [branch][select_if] select_if 选择第一个谓词匹配的后继（短路求值）。
TEST_CASE("Branch: select_if selects first predicate match", "[branch][select_if]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<bool> matched{ false };
    std::atomic<bool> other{ false };

    auto br = flow.emplace([](tfl::Branch& b) {
        b.select_if([](tfl::TaskView tv) { return tv.name() == "match"; });
        });
    auto p_other = flow.emplace([&] { other.store(true); }).name("other");
    auto p_match = flow.emplace([&] { matched.store(true); }).name("match");

    br.precede(p_other, p_match);

    env.executor.async(flow).wait();
    REQUIRE(matched.load());
    REQUIRE_FALSE(other.load());
}

/// @test [branch][reset] Branch::reset() 释放 0 个后继。
TEST_CASE("Branch: reset blocks all successors", "[branch][reset]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<bool> any{ false };

    auto br = flow.emplace([](tfl::Branch& b) { b.reset(); });
    auto p0 = flow.emplace([&] { any.store(true); });
    auto p1 = flow.emplace([&] { any.store(true); });
    br.precede(p0, p1);

    env.executor.async(flow).wait();
    REQUIRE_FALSE(any.load());
}

/// @test [branch][operator] Branch::operator[i] = true 等价于 select(i)。
TEST_CASE("Branch: operator[] subscript syntax", "[branch][operator]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> ran[2] = { 0, 0 };

    auto br = flow.emplace([](tfl::Branch& b) {
        b[1] = true;  // 等价于 b.select(1)
        });
    auto p0 = flow.emplace([&] { ran[0].store(1); });
    auto p1 = flow.emplace([&] { ran[1].store(1); });
    br.precede(p0, p1);

    env.executor.async(flow).wait();
    REQUIRE(ran[0].load() == 0);
    REQUIRE(ran[1].load() == 1);
}

// ============================================================================
// SECTION 2: MultiBranch —— 多目标累积
// ============================================================================

/// @test [branch][multi][select] MultiBranch::select(i, j) 同时释放多个后继。
TEST_CASE("MultiBranch: select(...) multi-index broadcast", "[branch][multi][select]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> ran[4] = { 0, 0, 0, 0 };

    auto mb = flow.emplace([](tfl::MultiBranch& m) {
        m.select(0, 2);  // 释放 0 和 2，跳过 1 和 3
        });
    auto p0 = flow.emplace([&] { ran[0].store(1); });
    auto p1 = flow.emplace([&] { ran[1].store(1); });
    auto p2 = flow.emplace([&] { ran[2].store(1); });
    auto p3 = flow.emplace([&] { ran[3].store(1); });
    mb.precede(p0, p1, p2, p3);

    env.executor.async(flow).wait();
    REQUIRE(ran[0].load() == 1);
    REQUIRE(ran[1].load() == 0);
    REQUIRE(ran[2].load() == 1);
    REQUIRE(ran[3].load() == 0);
}

/// @test [branch][multi][select_all] select_all 广播选择全部后继。
TEST_CASE("MultiBranch: select_all broadcasts all successors", "[branch][multi][select_all]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> hits{ 0 };

    auto mb = flow.emplace([](tfl::MultiBranch& m) { m.select_all(); });
    for (int i = 0; i < 5; ++i) {
        auto p = flow.emplace([&] { hits.fetch_add(1); });
        mb.precede(p);
    }

    env.executor.async(flow).wait();
    REQUIRE(hits.load() == 5);
}

/// @test [branch][multi][select_if] select_if 选择所有匹配项（与 Branch::select_if 不同）。
TEST_CASE("MultiBranch: select_if selects all matches", "[branch][multi][select_if]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> ok_hits{ 0 }, skip_hits{ 0 };

    auto mb = flow.emplace([](tfl::MultiBranch& m) {
        m.select_if([](tfl::TaskView tv) {
            return tv.name().starts_with("ok_");
            });
        });
    auto a = flow.emplace([&] { ok_hits.fetch_add(1); }).name("ok_a");
    auto b = flow.emplace([&] { skip_hits.fetch_add(1); }).name("skip");
    auto c = flow.emplace([&] { ok_hits.fetch_add(1); }).name("ok_c");
    mb.precede(a, b, c);

    env.executor.async(flow).wait();
    REQUIRE(ok_hits.load() == 2);
    REQUIRE(skip_hits.load() == 0);
}

/// @test [branch][multi][reset] reset() 清除累积的选择。
TEST_CASE("MultiBranch: reset clears accumulated selections", "[branch][multi][reset]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> hits{ 0 };

    auto mb = flow.emplace([](tfl::MultiBranch& m) {
        m.select(0, 1, 2);  // 先选全部
        m.reset();          // 再清除
        });
    auto p0 = flow.emplace([&] { hits.fetch_add(1); });
    auto p1 = flow.emplace([&] { hits.fetch_add(1); });
    auto p2 = flow.emplace([&] { hits.fetch_add(1); });
    mb.precede(p0, p1, p2);

    env.executor.async(flow).wait();
    REQUIRE(hits.load() == 0);
}

/// @test [branch][multi][operator] 多索引 operator[i, j, k] = {bool, bool, bool}。
TEST_CASE("MultiBranch: operator[multi-index] compile-time length check", "[branch][multi][operator]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> ran[3] = { 0, 0, 0 };

    auto mb = flow.emplace([](tfl::MultiBranch& m) {
        m.select(0, 2).unselect(1);
        });
    auto p0 = flow.emplace([&] { ran[0].store(1); });
    auto p1 = flow.emplace([&] { ran[1].store(1); });
    auto p2 = flow.emplace([&] { ran[2].store(1); });
    mb.precede(p0, p1, p2);

    env.executor.async(flow).wait();
    REQUIRE(ran[0].load() == 1);
    REQUIRE(ran[1].load() == 0);
    REQUIRE(ran[2].load() == 1);
}
          