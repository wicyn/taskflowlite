/// @brief Current APIs, separate from the Taskflow comparison suite.
#include <taskflowlite/taskflowlite.hpp>
#include "bench_common.hpp"

int main(int argc, char** argv) {
    const int args = parse_benchmark_args(argc, argv);
    if (args != 0) return args > 0 ? 0 : 2;
    const int runs = bench_runs(10'000);
    tfl::Executor executor(4);
    tfl::Flow graph;
    for (int i = 0; i < 33; ++i) (void)graph.emplace([] { add_one(); });
    g_counter = 0;
    {
        Timer timer("corun external | 33 sources | 4 workers");
        for (int i = 0; i < runs; ++i) executor.corun(graph);
    }
    verify(runs * 33);
    g_counter = 0;
    {
        Timer timer("corun nested | Executor / Runtime / TaskGroup");
        executor.async([&](tfl::Runtime& rt) {
            tfl::TaskGroup group(rt);
            for (int i = 0; i < runs; ++i) {
                executor.corun(graph);
                rt.corun(graph);
                group.corun(graph);
            }
        }).get();
    }
    verify(runs * 99);
    g_counter = 0;
    {
        Timer timer("Runtime / TaskGroup | duplicate and completed dependencies");
        executor.async([&](tfl::Runtime& rt) {
            auto run = [&](auto& scope) {
                for (int i = 0; i < runs; ++i) {
                    auto first = scope.async([] { add_one(); });
                    auto last = scope.async([] { add_one(); }, first, first);
                    scope.wait();
                    last.get();
                    auto completed = scope.async([] { add_one(); }, last);
                    scope.wait();
                    completed.get();
                }
            };
            run(rt);
            tfl::TaskGroup group(rt);
            run(group);
        }).get();
    }
    verify(runs * 6);
    executor.wait_for_all();
    return g_failures == 0 && executor.num_topologies() == 0 ? 0 : 1;
}
