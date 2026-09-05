/// @file test_task_group.cpp
/// @brief TaskGroup 测试 —— 作用域等待、返回值、依赖、图提交和停止域。

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: 作用域内的结构化并发
// ============================================================================

/// @test [task-group][wait] 单 worker 下显式与析构等待均可执行全部子任务。
TEST_CASE("TaskGroup: explicit and destructor wait", "[task-group][wait]") {
    TestEnv env(1);
    std::atomic<int> count{0};
    const bool explicit_wait = GENERATE(false, true);
    auto parent = env.executor.async([&](tfl::Runtime& rt) {
        {
            tfl::TaskGroup group(rt);
            for (int i = 0; i < 16; ++i) group.silent_async([&] { ++count; });
            if (explicit_wait) group.wait();
        }
        return count.load();
    });
    REQUIRE(parent.get() == 16);
}

/// @test [task-group][async] Future 依赖、延迟任务、Runtime/SubFlow 返回值。
/// @note 当前 core 在无捕获 SubFlow 的 Graph 初始化路径崩溃；保留默认回归测试。
TEST_CASE("TaskGroup: result types and dependency fan-in", "[task-group][async][core-regression]") {
    TestEnv env(1);
    auto parent = env.executor.async([](tfl::Runtime& rt) {
        tfl::TaskGroup group(rt);
        auto first = group.async([] { return 20; });
        auto second = group.async([](tfl::Runtime&) { return 22; });
        auto sum = group.async([first, second] { return first.get() + second.get(); }, first, second);
        auto delayed = tfl::AsyncTask([] { return 7; });
        group.run(delayed);
        auto dynamic = group.async([](tfl::SubFlow& sf) {
            (void)sf.emplace([] {});
            sf.run();
            return 3;
        });
        group.wait();
        return sum.get() + delayed.get() + dynamic.get() + static_cast<int>(group.size());
    });
    REQUIRE(parent.get() == 52);
}

// ============================================================================
// SECTION 2: 图重载和局部异常恢复
// ============================================================================

/// @test [task-group][graph] 图的单次、次数、谓词和回调重载均真实实例化。
TEST_CASE("TaskGroup: graph overloads", "[task-group][graph]") {
    TestEnv env(1);
    const int mode = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
    int count = 0, callbacks = 0;
    auto parent = env.executor.async([&](tfl::Runtime& rt) {
        tfl::Flow flow;
        (void)flow.emplace([&] { ++count; });
        tfl::TaskGroup group(rt);
        auto callback = [&] { ++callbacks; };
        auto predicate = [&] { return count == 3; };
        switch (mode) {
        case 0: (void)group.async(flow); break;
        case 1: (void)group.async(flow, callback); break;
        case 2: (void)group.async(flow, 3ULL); break;
        case 3: (void)group.async(flow, 3ULL, callback); break;
        case 4: (void)group.async(flow, predicate); break;
        case 5: (void)group.async(flow, predicate, callback); break;
        case 6: group.silent_async(flow); break;
        case 7: group.silent_async(flow, callback); break;
        case 8: group.silent_async(flow, 3ULL); break;
        case 9: group.silent_async(flow, 3ULL, callback); break;
        case 10: group.silent_async(flow, predicate); break;
        case 11: group.silent_async(flow, predicate, callback); break;
        case 12: group.run(flow); break;
        }
        group.wait();  // flow 的生命周期必须覆盖全部子任务
    });
    REQUIRE_NOTHROW(parent.get());
    REQUIRE(count == ((mode % 6 < 2 || mode == 12) ? 1 : 3));
    REQUIRE(callbacks == ((mode < 12 && mode % 2 == 1) ? 1 : 0));
}

/// @test [task-group][exception] 析构重抛子异常，可在父 callable 内恢复。
TEST_CASE("TaskGroup: destructor exception is locally recoverable", "[task-group][exception]") {
    TestEnv env(1);
    auto parent = env.executor.async([](tfl::Runtime& rt) {
        try {
            tfl::TaskGroup group(rt);
            group.silent_async([] { throw std::runtime_error("child"); });
        } catch (const std::runtime_error&) {
            return 42;
        }
        return 0;
    });
    REQUIRE(parent.get() == 42);
}

/// @test [task-group][stop] 停止请求幂等，false 模板实参建立独立停止域。
TEST_CASE("TaskGroup: stop domain inheritance is explicit", "[task-group][stop]") {
    TestEnv env(1);
    auto parent = env.executor.async([](tfl::Runtime& rt) {
        tfl::TaskGroup group(rt);
        auto inherited = group.async([] {});
        auto independent = group.async<false>([] {});
        const bool first = group.request_stop();
        const bool second = group.request_stop();
        const bool inherited_stop = inherited.stop_requested();
        const bool independent_stop = independent.stop_requested();
        group.wait();
        return first && !second && group.stop_requested() && inherited_stop && !independent_stop;
    });
    REQUIRE(parent.get());
}
