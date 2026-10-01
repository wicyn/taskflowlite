/// @file no_exceptions.cpp
/// @brief Compiled with both explicit library opt-out and compiler exceptions disabled.
#include "../taskflowlite/taskflowlite.hpp"
#include <cstdlib>
#include <cstring>
#include <csignal>

static_assert(TFL_ENABLE_EXCEPTIONS == 0);
#ifdef TFL_TEST_COMPILER_NO_EXCEPTIONS
static_assert(TFL_HAS_EXCEPTIONS == 0);
#endif

int main(int argc, char** argv) {
    // Report the terminate contract without relying on platform crash dialogs/signals.
    std::set_terminate([] { std::_Exit(86); });
#if defined(_MSC_VER) && !_HAS_EXCEPTIONS
    // MSVC STL 的 _HAS_EXCEPTIONS=0 实现把 terminate 映射为 abort，set_terminate 是空操作。
    // Debug CRT 在触发 SIGABRT 前可能显示诊断窗口，阻塞无人值守的终止测试。
    // 禁用该弹窗和系统错误报告，让下面的 SIGABRT handler 确定性地返回退出码。
    ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::signal(SIGABRT, [](int) { std::_Exit(86); });
#endif
    if (argc == 2) {
        if (std::strcmp(argv[1], "future") == 0) {
            tfl::AsyncFuture<int> empty;
            (void)empty.get();
        } else if (std::strcmp(argv[1], "executor") == 0) {
            tfl::Executor invalid(0);
        } else if (std::strcmp(argv[1], "start") == 0) {
            tfl::AsyncTask<void> empty;
            empty.start();
        } else if (std::strcmp(argv[1], "link") == 0) {
            tfl::Flow flow;
            auto task = flow.emplace([] {});
            task.precede(task);
        } else if (std::strcmp(argv[1], "vector") == 0) {
            tfl::SmallVector<int, 2> values;
            (void)values.at(0);
        }
#if TFL_HAS_EXCEPTIONS
        else if (std::strcmp(argv[1], "callable") == 0) {
            tfl::Executor executor(1);
            executor.async([] {
                // MSVC 的 terminate handler 是线程局部配置。
                std::set_terminate([] { std::_Exit(86); });
                throw 42;
            }).wait();
        }
#endif
        return 1; // Every supported error path must terminate.
    }

    tfl::SmallVector<int, 2> values{1, 2};
    for (int i = 3; i <= 32; ++i) values.push_back(i);
    auto copy = values;
    if (copy.size() != 32 || copy.back() != 32) return 2;
    tfl::Semaphore gate(1);
    if (!gate.reset(2, 1) || gate.value() != 1) return 3;
    tfl::Executor executor(1);
    int count = 0;
    tfl::Flow graph;
    auto first = graph.emplace([&] { ++count; });
    auto second = graph.emplace([&] { ++count; });
    first.precede(second);
    first.acquire(gate).release(gate);
    executor.corun(graph);
    auto parent = executor.async([&](tfl::Runtime& rt) {
        rt.corun(graph);
        tfl::TaskGroup group(rt);
        group.corun(graph);
        auto a = group.async([] { return 20; });
        auto b = group.async([a] { return a.get() + 22; }, a);
        group.wait();
        return b.get();
    });
    if (parent.get() != 42 || count != 6) return 4;
    auto deferred = executor.defer_async([](tfl::SubFlow& sf) {
        (void)sf.emplace([] {});
        sf.run();
        return 7;
    });
    if (deferred.start().get() != 7) return 5;
    executor.wait_for_all();
    return executor.num_topologies() == 0 ? 0 : 6;
}
