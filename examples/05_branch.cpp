/// @file 05_branch.cpp
/// @brief 演示基于 Branch 的条件路由，通过 Lambda 捕获共享随机数引擎。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <random>

int main() {
    std::cout << "=== Example 05: Branch (Conditional Execution) ===\n";

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 2);

    // 外部随机数生成器
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 2);

    for (int i = 0; i < 3; ++i) {
        tfl::Flow flow;

        auto start = flow.emplace([] { std::cout << "Start Node\n"; });

        // 分支节点：引用捕获随机数引擎 [&gen, &dis]
        auto branch = flow.emplace([&gen, &dis](tfl::Branch& br) {
            int route = dis(gen);
            std::cout << "Routing to path: " << route << "\n";
            br.allow(route); // 决定激活哪一条后继边
        });

        auto path0 = flow.emplace([] { std::cout << "  -> Executing Path 0\n"; });
        auto path1 = flow.emplace([] { std::cout << "  -> Executing Path 1\n"; });
        auto path2 = flow.emplace([] { std::cout << "  -> Executing Path 2\n"; });
        auto end   = flow.emplace([] { std::cout << "End Node\n"; });

        start.precede(branch);
        branch.precede(path0, path1, path2); // 后继顺序即为索引 0, 1, 2
        path0.precede(end);
        path1.precede(end);
        path2.precede(end);

        std::cout << "\n--- Run #" << i + 1 << " ---\n";
        executor.submit(flow).start().wait();
    }

    return 0;
}
