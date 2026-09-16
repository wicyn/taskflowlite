/// @file 33_unchecked_task_links.cpp
/// @brief 已知合法的 DAG 使用编译期建边策略；对比纯连边耗时。
#include "../taskflowlite/taskflowlite.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <vector>

namespace {
constexpr std::size_t task_count = 1000;

template <bool Check>
double build_and_run(tfl::Executor& executor) {
    tfl::Flow flow;
    std::vector<tfl::Task> tasks;
    tasks.reserve(task_count);
    std::atomic<std::size_t> completed{0};
    for (std::size_t i = 0; i < task_count; ++i) {
        tasks.push_back(flow.emplace([&] { ++completed; }));
    }

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < task_count; ++i) {
        // 同一 Flow 中的有效节点，每对索引只连接一次，且 i < j 保证无环。
        // 这三个条件由构图算法保证，才可以安全选择 Check=false。
        for (std::size_t j = i + 1; j < std::min(task_count, i + 5); ++j) {
            tasks[i].precede<Check>(tasks[j]);
        }
    }
    const auto elapsed = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - start).count();
    executor.async(flow).get();
    if (completed != task_count) throw std::runtime_error("Incomplete DAG execution");
    return elapsed;
}
}

int main() {
    tfl::Executor executor(2);
    std::array<double, 5> checked{}, unchecked{};
    for (std::size_t round = 0; round < checked.size(); ++round) {
        // 交替顺序，取中位数；这里只计时建边，不包含创建节点及运行任务。
        if (round % 2 == 0) {
            checked[round] = build_and_run<true>(executor);
            unchecked[round] = build_and_run<false>(executor);
        } else {
            unchecked[round] = build_and_run<false>(executor);
            checked[round] = build_and_run<true>(executor);
        }
    }
    std::sort(checked.begin(), checked.end());
    std::sort(unchecked.begin(), unchecked.end());
    std::cout << "1000 tasks, 3990 edges; median link time (microseconds)\n"
              << "Checked: " << checked[2] << "\nUnchecked: " << unchecked[2] << '\n';

    tfl::Flow chain;
    auto a = chain.placeholder(), b = chain.placeholder(), c = chain.placeholder();
    // 原有 a.precede(b)、b.succeed(a)、linearize(...) 仍默认校验。
    // 新建且未连边的链可使用三种形式之一，不能对同一条边重复调用。
    chain.linearize<false>({a, b, c});
    executor.async(chain).get();
    return 0;
}
