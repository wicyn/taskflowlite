/// @file 36_corun.cpp
/// @brief 外部同步执行、单 Worker 嵌套执行和局部异常恢复。
#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    tfl::Executor executor(1);
    int count = 0;
    tfl::Flow graph;
    (void)graph.emplace([&] { ++count; });

    executor.corun(graph); // 外部线程阻塞等待，返回前图已经完成。
    executor.async([&](tfl::Runtime& rt) {
        executor.corun(graph); // 同一 Executor 的 Worker 协作执行，单 Worker 也可嵌套。
        rt.corun(graph);
        tfl::TaskGroup group(rt);
        group.corun(graph); // 只等待本次图；不要求先完成组内其他任务。

        tfl::Flow failing;
        (void)failing.emplace([] { throw std::runtime_error("child failed"); });
        try {
            group.corun(failing);
        } catch (const std::runtime_error& e) {
            std::cout << "Recovered: " << e.what() << '\n';
            group.corun(graph);
        }
        group.wait();
    }).get();

    std::cout << "Completed graph runs: " << count << '\n';
    return count == 5 ? 0 : 1;
}
