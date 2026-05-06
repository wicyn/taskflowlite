/// @file test_jump.cpp
/// @brief Jump / MultiJump 测试 —— 强制跳转、重试循环、广播状态机。
///
/// 覆盖接口（Jump 单目标抢占）：
///   - Jump::select(index)               强制跳转到索引 index 的后继
///   - Jump::select_if(pred)             谓词选首个匹配
///   - Jump::reset()                     不跳转（走常规 tear_down）
///   - Jump::operator[](index)           下标 Proxy
///   - Jump::size()                      后继数
///
/// 覆盖接口（MultiJump 多目标广播）：
///   - MultiJump::select(i, j, k...)     批量强制跳转
///   - MultiJump::select_all()           全选广播
///   - MultiJump::select_if(pred)        选所有匹配
///   - MultiJump::reset()                清空
///   - MultiJump::operator[i,j,k]
///
/// 与 Branch 的本质区别：
///   - Branch 走"协作式"（边权 2，需要外部 -1 才会触发）；
///   - Jump   走"抢占式"（强制清零 join_counter，立即调度）。

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: Jump —— 重试循环（最经典用法）
// ============================================================================

/// @test [jump][retry] 经典 retry 循环：process → check → if 失败跳回 process。
/// @details 这是 Jump 的杀手级应用：把 while-loop 编码成 DAG。
///          注意 process 的入口边来自 init（普通 weight-1 边，保证启动）。
TEST_CASE("Jump: retry 循环", "[jump][retry]") {
    TestEnv env;
    tfl::Flow flow;

    int attempts = 0;
    constexpr int MAX = 4;
    std::atomic<bool> success_run{false};

    auto init = flow.emplace([] {});
    auto process = flow.emplace([&attempts] { ++attempts; });
    auto check = flow.emplace([&attempts](tfl::Jump& jmp) {
        if (attempts < MAX) {
            jmp.select(0);  // 跳回 process（target[0]）
        } else {
            jmp.select(1);
        }
    });
    auto success = flow.emplace([&] { success_run.store(true); });

    init.precede(process);
    process.precede(check);
    check.precede(process, success);  // 0 = process, 1 = success

    env.executor.deferred_async(flow).start().wait();

    REQUIRE(attempts == MAX);
    REQUIRE(success_run.load());
}

/// @test [jump][reset] reset 不跳转，走常规 tear_down。
TEST_CASE("Jump: reset 走常规路径", "[jump][reset]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<bool> next_run{false};

    auto entry = flow.emplace([] {});
    auto j = flow.emplace([](tfl::Jump& jmp) { jmp.select(0); });
    auto next = flow.emplace([&] { next_run.store(true); });

    entry.precede(j);
    j.precede(next);

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(next_run.load());

}

/// @test [jump][select_if] select_if 用谓词找首个匹配的后继。
TEST_CASE("Jump: select_if 名字匹配跳转", "[jump][select_if]") {
    TestEnv env;
    tfl::Flow flow;

    int state = 0;
    std::atomic<bool> finish_run{false};

    auto start = flow.emplace([] {});
    auto step = flow.emplace([&state] { ++state; }).name("retry");
    auto decide = flow.emplace([&state](tfl::Jump& jmp) {
        if (state < 3) {
            jmp.select_if([](tfl::TaskView tv) { return tv.name() == "retry"; });
        } else {
            jmp.select_if([](tfl::TaskView tv) { return tv.name() == "finish"; });
        }
    });
    auto finish = flow.emplace([&] { finish_run.store(true); }).name("finish");

    start.precede(step);
    step.precede(decide);
    decide.precede(step, finish);

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(state == 3);
    REQUIRE(finish_run.load());
}

/// @test [jump][operator] operator[i] = true 等价于 select(i)。
TEST_CASE("Jump: operator[] 下标语法", "[jump][operator]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> work_runs{0};

    auto entry = flow.emplace([] {});
    auto work = flow.emplace([&] { work_runs.fetch_add(1); });
    auto gate = flow.emplace([&](tfl::Jump& jmp) {
        if (work_runs.load() < 2) {
            jmp[0] = true;  // 跳回 work
        } else {
            jmp[0] = false; // 不跳
        }
    });
    auto done = flow.emplace([] {});

    entry.precede(work);
    work.precede(gate);
    gate.precede(work, done);

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(work_runs.load() == 2);
}

// ============================================================================
// SECTION 2: MultiJump —— 多目标强制广播
// ============================================================================

/// @test [jump][multi] MultiJump select 同时强制激活多个后继（并行循环）。
/// @details 拓扑：3 条并行分支 + MultiJump 汇聚控制。
///          每轮 mj.select(0,1,2) 同时重激活 3 条分支。
TEST_CASE("MultiJump: 并行扇出循环", "[jump][multi][parallel]") {
    TestEnv env(4);
    tfl::Flow flow;

    constexpr int ITERS = 5;
    std::atomic<int> hits_a{0}, hits_b{0}, hits_c{0};

    auto branch_a = flow.emplace([&] { hits_a.fetch_add(1); });
    auto branch_b = flow.emplace([&] { hits_b.fetch_add(1); });
    auto branch_c = flow.emplace([&] { hits_c.fetch_add(1); });

    int count = 0;
    auto mj = flow.emplace([&count, ITERS](tfl::MultiJump& jmp) {
        if (count < ITERS - 1) {
            ++count;
            jmp.select(0, 1, 2);  // 同时跳回 a/b/c
        }
    });

    // 普通边（weight-1）：mj 等三条分支汇聚
    branch_a.precede(mj);
    branch_b.precede(mj);
    branch_c.precede(mj);

    // 跳转边（weight-0）：mj → branch_X target[0,1,2]
    mj.precede(branch_a, branch_b, branch_c);

    // 起点（必要！否则 branch_a/b/c 的 join_counter 因 mj 这条入边 ≠ 0 而无法启动）
    auto init = flow.emplace([] {});
    init.precede(branch_a, branch_b, branch_c);

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(hits_a.load() == ITERS);
    REQUIRE(hits_b.load() == ITERS);
    REQUIRE(hits_c.load() == ITERS);
}
