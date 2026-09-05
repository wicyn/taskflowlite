/// @file 05_branch.cpp
/// @brief 演示 Branch（单目标条件路由）与 MultiBranch（多目标广播路由）。
///   覆盖 select(index)、operator()、select_if、select_all、unselect 等 API。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <random>
#include <syncstream>

int main() {
    std::osyncstream(std::cout) << "=== Example 05: Branch & MultiBranch ===\n\n";

    tfl::Executor executor(4);

    // ================================================================
    // Part 1: Branch -- select(index) 单目标条件路由
    // ================================================================
    {
        std::osyncstream(std::cout) << "--- Part 1: Branch select(index) ---\n";

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 2);

        for (int i = 0; i < 3; ++i) {
            tfl::Flow flow;

            auto start = flow.emplace([] { std::osyncstream(std::cout) << "  [Start]\n"; });

            auto branch = flow.emplace([&gen, &dis](tfl::Branch& br) {
                int route = dis(gen);
                std::osyncstream(std::cout) << "  [Branch] select(" << route << ")\n";
                br.select(route);
            });

            auto path0 = flow.emplace([] { std::osyncstream(std::cout) << "    -> Path 0\n"; });
            auto path1 = flow.emplace([] { std::osyncstream(std::cout) << "    -> Path 1\n"; });
            auto path2 = flow.emplace([] { std::osyncstream(std::cout) << "    -> Path 2\n"; });
            auto end   = flow.emplace([] { std::osyncstream(std::cout) << "  [End]\n"; });

            start.precede(branch);
            branch.precede(path0, path1, path2);
            path0.precede(end);
            path1.precede(end);
            path2.precede(end);

            std::osyncstream(std::cout) << "\n  Run #" << i + 1 << ":\n";
            executor.async(flow).wait();
        }
    }

    // ================================================================
    // Part 2: Branch -- operator() 语法
    // ================================================================
    {
        std::osyncstream(std::cout) << "\n--- Part 2: Branch operator() ---\n";

        tfl::Flow flow;

        auto start = flow.emplace([] { std::osyncstream(std::cout) << "  [Start]\n"; });

        auto branch = flow.emplace([](tfl::Branch& br) {
            std::osyncstream(std::cout) << "  [Branch] br(2) -- select successor 2\n";
            br(2);
        });

        auto path0 = flow.emplace([] { std::osyncstream(std::cout) << "    -> Path 0 (should NOT run)\n"; });
        auto path1 = flow.emplace([] { std::osyncstream(std::cout) << "    -> Path 1 (should NOT run)\n"; });
        auto path2 = flow.emplace([] { std::osyncstream(std::cout) << "    -> Path 2 (should run)\n"; });
        auto end   = flow.emplace([] { std::osyncstream(std::cout) << "  [End]\n"; });

        start.precede(branch);
        branch.precede(path0, path1, path2);
        path0.precede(end);
        path1.precede(end);
        path2.precede(end);

        executor.async(flow).wait();
    }

    // ================================================================
    // Part 3: Branch -- select_if 谓词选择
    // ================================================================
    {
        std::osyncstream(std::cout) << "\n--- Part 3: Branch select_if ---\n";

        tfl::Flow flow;

        auto start = flow.emplace([] { std::osyncstream(std::cout) << "  [Start]\n"; });

        auto branch = flow.emplace([](tfl::Branch& br) {
            std::osyncstream(std::cout) << "  [Branch] select_if: select successor named 'target'\n";
            br.select_if([](tfl::TaskView tv) {
                return tv.name() == "target";
            });
        });

        auto path0 = flow.emplace([] { std::osyncstream(std::cout) << "    -> decoy (should NOT run)\n"; });
        auto path1 = flow.emplace([] { std::osyncstream(std::cout) << "    -> target (should run)\n"; });
        auto path2 = flow.emplace([] { std::osyncstream(std::cout) << "    -> decoy2 (should NOT run)\n"; });
        auto end   = flow.emplace([] { std::osyncstream(std::cout) << "  [End]\n"; });

        path0.name("decoy");
        path1.name("target");
        path2.name("decoy2");

        start.precede(branch);
        branch.precede(path0, path1, path2);
        path0.precede(end);
        path1.precede(end);
        path2.precede(end);

        executor.async(flow).wait();
    }

    // ================================================================
    // Part 4: MultiBranch -- 多目标广播路由
    // ================================================================
    {
        std::osyncstream(std::cout) << "\n--- Part 4: MultiBranch ---\n";

        tfl::Flow flow;

        auto start = flow.emplace([] { std::osyncstream(std::cout) << "  [Start]\n"; });

        // 方式 1: select(indices...)
        auto mb1 = flow.emplace([](tfl::MultiBranch& mb) {
            std::osyncstream(std::cout) << "  [MB] select(0, 2) -- select 0 and 2\n";
            mb.select(0, 2);
        });

        auto a0 = flow.emplace([] { std::osyncstream(std::cout) << "    -> A0 (should run)\n"; });
        auto a1 = flow.emplace([] { std::osyncstream(std::cout) << "    -> A1 (should NOT run)\n"; });
        auto a2 = flow.emplace([] { std::osyncstream(std::cout) << "    -> A2 (should run)\n"; });
        auto mid = flow.emplace([] { std::osyncstream(std::cout) << "  [Mid]\n"; });

        start.precede(mb1);
        mb1.precede(a0, a1, a2);
        a0.precede(mid);
        a1.precede(mid);
        a2.precede(mid);

        // 方式 2: operator() 多索引
        auto mb2 = flow.emplace([](tfl::MultiBranch& mb) {
            std::osyncstream(std::cout) << "  [MB] mb(0, 1) -- select 0 and 1\n";
            mb(0, 1);
        });

        auto b0 = flow.emplace([] { std::osyncstream(std::cout) << "    -> B0 (should run)\n"; });
        auto b1 = flow.emplace([] { std::osyncstream(std::cout) << "    -> B1 (should run)\n"; });
        auto b2 = flow.emplace([] { std::osyncstream(std::cout) << "    -> B2 (should NOT run)\n"; });
        auto mid2 = flow.emplace([] { std::osyncstream(std::cout) << "  [Mid2]\n"; });

        mid.precede(mb2);
        mb2.precede(b0, b1, b2);
        b0.precede(mid2);
        b1.precede(mid2);
        b2.precede(mid2);

        // 方式 3: operator() 选择 0 和 2
        auto mb3 = flow.emplace([](tfl::MultiBranch& mb) {
            std::osyncstream(std::cout) << "  [MB] mb(0, 2) -- select 0 and 2\n";
            mb(0, 2);
        });

        auto c0 = flow.emplace([] { std::osyncstream(std::cout) << "    -> C0 (should run)\n"; });
        auto c1 = flow.emplace([] { std::osyncstream(std::cout) << "    -> C1 (should NOT run)\n"; });
        auto c2 = flow.emplace([] { std::osyncstream(std::cout) << "    -> C2 (should run)\n"; });
        auto c3 = flow.emplace([] { std::osyncstream(std::cout) << "    -> C3 (should NOT run)\n"; });
        auto end = flow.emplace([] { std::osyncstream(std::cout) << "  [End]\n"; });

        mid2.precede(mb3);
        mb3.precede(c0, c1, c2, c3);
        c0.precede(end);
        c1.precede(end);
        c2.precede(end);
        c3.precede(end);

        executor.async(flow).wait();
    }

    // ================================================================
    // Part 5: MultiBranch -- select_all / select_if
    // ================================================================
    {
        std::osyncstream(std::cout) << "\n--- Part 5: MultiBranch select_all / select_if ---\n";

        // select_all 广播
        {
            tfl::Flow flow;
            auto start = flow.emplace([] { std::osyncstream(std::cout) << "  [Start]\n"; });

            auto mb = flow.emplace([](tfl::MultiBranch& mb) {
                std::osyncstream(std::cout) << "  [MB] select_all() -- broadcast to all successors\n";
                mb.select_all();
            });

            auto d0 = flow.emplace([] { std::osyncstream(std::cout) << "    -> D0 (should run)\n"; });
            auto d1 = flow.emplace([] { std::osyncstream(std::cout) << "    -> D1 (should run)\n"; });
            auto d2 = flow.emplace([] { std::osyncstream(std::cout) << "    -> D2 (should run)\n"; });
            auto end = flow.emplace([] { std::osyncstream(std::cout) << "  [End]\n"; });

            start.precede(mb);
            mb.precede(d0, d1, d2);
            d0.precede(end);
            d1.precede(end);
            d2.precede(end);

            executor.async(flow).wait();
        }

        // select_if 谓词筛选
        {
            tfl::Flow flow;
            auto start = flow.emplace([] { std::osyncstream(std::cout) << "\n  [Start]\n"; });

            auto mb = flow.emplace([](tfl::MultiBranch& mb) {
                std::osyncstream(std::cout) << "  [MB] select_if: select successors whose name starts with 'ok'\n";
                mb.select_if([](tfl::TaskView tv) {
                    return tv.name().starts_with("ok");
                });
            });

            auto e0 = flow.emplace([] { std::osyncstream(std::cout) << "    -> ok_alpha (should run)\n"; });
            auto e1 = flow.emplace([] { std::osyncstream(std::cout) << "    -> skip_beta (should NOT run)\n"; });
            auto e2 = flow.emplace([] { std::osyncstream(std::cout) << "    -> ok_gamma (should run)\n"; });
            auto end = flow.emplace([] { std::osyncstream(std::cout) << "  [End]\n"; });

            e0.name("ok_alpha");
            e1.name("skip_beta");
            e2.name("ok_gamma");

            start.precede(mb);
            mb.precede(e0, e1, e2);
            e0.precede(end);
            e1.precede(end);
            e2.precede(end);

            executor.async(flow).wait();
        }
    }

    return 0;
}
