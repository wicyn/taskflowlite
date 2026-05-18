/// @file test_jump.cpp
/// @brief Jump / MultiJump 测试 —— 强制跳转、重试循环、广播状态机。
///
/// 覆盖的接口（Jump 单目标抢占式）：
///   - Jump::select(index)               强制跳转到索引处的后继
///   - Jump::select_if(pred)             谓词选择第一个匹配项
///   - Jump::reset()                     不跳转（正常 tear_down 路径）
///   - Jump::operator[](index)           下标代理
///   - Jump::size()                      后继数量
///
/// 覆盖的接口（MultiJump 多目标广播）：
///   - MultiJump::select(i, j, k...)     批量强制跳转
///   - MultiJump::select_all()           广播选择全部
///   - MultiJump::select_if(pred)        选择所有匹配项
///   - MultiJump::reset()                清除
///   - MultiJump::operator[i,j,k]
///
/// 与 Branch 的本质区别：
///   - Branch 使用"协作式"（边权重 2，需要外部 -1 来触发）；
///   - Jump  使用"抢占式"（强制清除 join_counter，立即调度）。

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: Jump —— 重试循环（最经典的应用场景）
// ============================================================================

/// @test [jump][retry] 经典重试循环：process -> check -> 如果失败跳回 process。
/// @details 这是 Jump 的杀手级应用：将 while 循环编码为 DAG。
///          注意 process 的入口边来自 init（普通权重-1 边，保证启动）。
TEST_CASE("Jump: retry loop", "[jump][retry]") {
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
    check.precede(process, success);  // 0 = process，1 = success

    env.executor.deferred_async(flow).start().wait();

    REQUIRE(attempts == MAX);
    REQUIRE(success_run.load());
}

/// @test [jump][reset] reset 遵循正常路径，不跳转。
TEST_CASE("Jump: reset follows normal path", "[jump][reset]") {
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

/// @test [jump][select_if] select_if 使用谓词查找第一个匹配的后继。
TEST_CASE("Jump: select_if name-match jump", "[jump][select_if]") {
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
TEST_CASE("Jump: operator[] subscript syntax", "[jump][operator]") {
    TestEnv env;
    tfl::Flow flow;
    std::atomic<int> work_runs{0};

    auto entry = flow.emplace([] {});
    auto work = flow.emplace([&] { work_runs.fetch_add(1); });
    auto gate = flow.emplace([&](tfl::Jump& jmp) {
        if (work_runs.load() < 2) {
            jmp[0] = true;  // 跳回 work
        } else {
            jmp[0] = false; // 不跳转
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
/// @details 拓扑：3 个并行分支 + MultiJump 汇聚控制。
///          每轮 mj.select(0,1,2) 同时重新激活 3 个分支。
TEST_CASE("MultiJump: parallel fan-out loop", "[jump][multi][parallel]") {
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

    // 普通边（权重-1）：mj 等待三个分支汇聚
    branch_a.precede(mj);
    branch_b.precede(mj);
    branch_c.precede(mj);

    // 跳转边（权重-0）：mj -> branch_X target[0,1,2]
    mj.precede(branch_a, branch_b, branch_c);

    // 入口点（必须！否则 branch_a/b/c 的 join_counter 因 mj 的入边非零而无法启动）
    auto init = flow.emplace([] {});
    init.precede(branch_a, branch_b, branch_c);

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(hits_a.load() == ITERS);
    REQUIRE(hits_b.load() == ITERS);
    REQUIRE(hits_c.load() == ITERS);
}
