/// @file 06_jump.cpp
/// @brief 演示 Jump（单目标无条件跳转/重试）与 MultiJump（多目标强制调度）。
///   覆盖 select、operator()、select_if、select_all、unselect 及复合场景。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>

int main() {
    std::cout << "=== Example 06: Jump & MultiJump ===\n\n";

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);

    // ================================================================
    // Part 1: Jump -- 重试循环（select 方式）
    // ================================================================
    {
        std::cout << "--- Part 1: Jump Retry Loop (select) ---\n";

        tfl::Flow flow;
        int attempts = 0;
        constexpr int max_attempts = 3;

        auto init = flow.emplace([] {
            std::cout << "  [Init] System init\n";
        });

        auto process = flow.emplace([&attempts] {
            std::cout << "  [Process] Attempt #" << ++attempts << "...\n";
        });

        auto check = flow.emplace([&attempts, max_attempts](tfl::Jump& jmp) {
            if (attempts < max_attempts) {
                std::cout << "  [Check] Failed, jump back to Process (select(0))\n";
                jmp.select(0);
            } else {
                std::cout << "  [Check] Passed!\n";
            }
        });

        auto success = flow.emplace([] {
            std::cout << "  [Success] Operation succeeded!\n";
        });

        init.precede(process);
        process.precede(check);
        check.precede(process, success);

        executor.async(flow).wait();
    }

    // ================================================================
    // Part 2: Jump -- operator() 语法
    // ================================================================
    {
        std::cout << "\n--- Part 2: Jump operator() ---\n";

        tfl::Flow flow;
        int counter = 0;

        auto entry = flow.emplace([] {
            std::cout << "  [Entry]\n";
        });

        auto work = flow.emplace([&counter] {
            ++counter;
            std::cout << "  [Work] counter = " << counter << "\n";
        });

        auto gate = flow.emplace([&counter](tfl::Jump& jmp) {
            if (counter < 2) {
                std::cout << "  [Gate] jmp(0) -- jump back to Work\n";
                jmp(0);
            } else {
                std::cout << "  [Gate] No jump, follow normal path\n";
            }
        });

        auto done = flow.emplace([] {
            std::cout << "  [Done]\n";
        });

        entry.precede(work);
        work.precede(gate);
        gate.precede(work, done);

        executor.async(flow).wait();
    }

    // ================================================================
    // Part 3: Jump -- select_if 谓词跳转
    // ================================================================
    {
        std::cout << "\n--- Part 3: Jump select_if ---\n";

        tfl::Flow flow;
        int state = 0;

        auto start = flow.emplace([] {
            std::cout << "  [Start]\n";
        });

        auto step = flow.emplace([&state] {
            ++state;
            std::cout << "  [Step] state = " << state << "\n";
        });

        auto decide = flow.emplace([&state](tfl::Jump& jmp) {
            if (state < 3) {
                std::cout << "  [Decide] select_if: jump to 'retry'\n";
                jmp.select_if([](tfl::TaskView tv) {
                    return tv.name() == "retry";
                });
            } else {
                std::cout << "  [Decide] Condition met, no jump\n";
            }
        });

        auto finish = flow.emplace([] {
            std::cout << "  [Finish] Done!\n";
        });

        step.name("retry");

        start.precede(step);
        step.precede(decide);
        decide.precede(step, finish);

        executor.async(flow).wait();
    }

    // ================================================================
    // Part 4: MultiJump -- 多目标强制调度
    // ================================================================
    {
        std::cout << "\n--- Part 4: MultiJump ---\n";

        tfl::Flow flow;

        auto start = flow.emplace([] {
            std::cout << "  [Start]\n";
        });

        // 方式 1: select(indices...)
        auto mj1 = flow.emplace([](tfl::MultiJump& mj) {
            std::cout << "  [MJ] select(0, 2) -- force jump to 0 and 2\n";
            mj.select(0, 2);
        });

        auto target0 = flow.emplace([] { std::cout << "    -> Target0 (should run)\n"; });
        auto target1 = flow.emplace([] { std::cout << "    -> Target1 (should NOT run)\n"; });
        auto target2 = flow.emplace([] { std::cout << "    -> Target2 (should run)\n"; });
        auto mid = flow.emplace([] { std::cout << "  [Mid]\n"; });

        start.precede(mj1);
        mj1.precede(target0, target1, target2);
        target0.precede(mid);
        target1.precede(mid);
        target2.precede(mid);

        // 方式 2: operator() 多索引
        auto mj2 = flow.emplace([](tfl::MultiJump& mj) {
            std::cout << "  [MJ] mj(0, 2) -- jump to 0 and 2\n";
            mj(0, 2);
        });

        auto u0 = flow.emplace([] { std::cout << "    -> U0 (should run)\n"; });
        auto u1 = flow.emplace([] { std::cout << "    -> U1 (should NOT run)\n"; });
        auto u2 = flow.emplace([] { std::cout << "    -> U2 (should run)\n"; });
        auto mid2 = flow.emplace([] { std::cout << "  [Mid2]\n"; });

        mid.precede(mj2);
        mj2.precede(u0, u1, u2);
        u0.precede(mid2);
        u1.precede(mid2);
        u2.precede(mid2);

        // 方式 3: operator() 选择 0 和 2
        auto mj3 = flow.emplace([](tfl::MultiJump& mj) {
            std::cout << "  [MJ] mj(0, 2) -- select 0 and 2\n";
            mj(0, 2);
        });

        auto v0 = flow.emplace([] { std::cout << "    -> V0 (should run)\n"; });
        auto v1 = flow.emplace([] { std::cout << "    -> V1 (should NOT run)\n"; });
        auto v2 = flow.emplace([] { std::cout << "    -> V2 (should run)\n"; });
        auto v3 = flow.emplace([] { std::cout << "    -> V3 (should NOT run)\n"; });
        auto end = flow.emplace([] { std::cout << "  [End]\n"; });

        mid2.precede(mj3);
        mj3.precede(v0, v1, v2, v3);
        v0.precede(end);
        v1.precede(end);
        v2.precede(end);
        v3.precede(end);

        executor.async(flow).wait();
    }

    // ================================================================
    // Part 5: MultiJump -- select_all / select_if
    // ================================================================
    {
        std::cout << "\n--- Part 5: MultiJump select_all / select_if ---\n";

        // select_all 广播
        {
            tfl::Flow flow;
            auto start = flow.emplace([] { std::cout << "  [Start]\n"; });

            auto mj = flow.emplace([](tfl::MultiJump& mj) {
                std::cout << "  [MJ] select_all() -- force jump to all successors\n";
                mj.select_all();
            });

            auto w0 = flow.emplace([] { std::cout << "    -> W0 (should run)\n"; });
            auto w1 = flow.emplace([] { std::cout << "    -> W1 (should run)\n"; });
            auto w2 = flow.emplace([] { std::cout << "    -> W2 (should run)\n"; });
            auto end = flow.emplace([] { std::cout << "  [End]\n"; });

            start.precede(mj);
            mj.precede(w0, w1, w2);
            w0.precede(end);
            w1.precede(end);
            w2.precede(end);

            executor.async(flow).wait();
        }

        // select_if 谓词筛选
        {
            tfl::Flow flow;
            auto start = flow.emplace([] { std::cout << "\n  [Start]\n"; });

            auto mj = flow.emplace([](tfl::MultiJump& mj) {
                std::cout << "  [MJ] select_if: force jump to successors containing 'hot'\n";
                mj.select_if([](tfl::TaskView tv) {
                    return tv.name().find("hot") != std::string_view::npos;
                });
            });

            auto x0 = flow.emplace([] { std::cout << "    -> hot_alpha (should run)\n"; });
            auto x1 = flow.emplace([] { std::cout << "    -> cold_beta (should NOT run)\n"; });
            auto x2 = flow.emplace([] { std::cout << "    -> hot_gamma (should run)\n"; });
            auto end = flow.emplace([] { std::cout << "  [End]\n"; });

            x0.name("hot_alpha");
            x1.name("cold_beta");
            x2.name("hot_gamma");

            start.precede(mj);
            mj.precede(x0, x1, x2);
            x0.precede(end);
            x1.precede(end);
            x2.precede(end);

            executor.async(flow).wait();
        }
    }

    // ================================================================
    // Part 6: MultiJump -- 复合场景：先选后撤
    // ================================================================
    {
        std::cout << "\n--- Part 6: MultiJump mixed operations ---\n";

        tfl::Flow flow;

        auto start = flow.emplace([] { std::cout << "  [Start]\n"; });

        auto mj = flow.emplace([](tfl::MultiJump& mj) {
            mj.select_all();
            std::cout << "  [MJ] select_all() then unselect(1) to cancel index 1\n";
            mj.unselect(1);
        });

        auto y0 = flow.emplace([] { std::cout << "    -> Y0 (should run)\n"; });
        auto y1 = flow.emplace([] { std::cout << "    -> Y1 (should NOT run)\n"; });
        auto y2 = flow.emplace([] { std::cout << "    -> Y2 (should run)\n"; });
        auto end = flow.emplace([] { std::cout << "  [End]\n"; });

        start.precede(mj);
        mj.precede(y0, y1, y2);
        y0.precede(end);
        y1.precede(end);
        y2.precede(end);

        executor.async(flow).wait();
    }

    return 0;
}
