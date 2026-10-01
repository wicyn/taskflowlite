/// @file test_corun.cpp
/// @brief 同步图执行的线程、生命周期、异常隔离和停止域回归。
#include "test_common.hpp"

namespace {
struct GraphHolder {
    tfl::Flow flow;
    tfl::Graph& graph() noexcept { return flow.graph(); }
    const tfl::Graph& graph() const noexcept { return flow.graph(); }
};

tfl::Flow make_flow(int& count) {
    tfl::Flow flow;
    auto first = flow.emplace([&] { ++count; });
    auto last = flow.emplace([&] { ++count; });
    first.precede(last);
    return flow;
}
} // namespace

TEST_CASE("Executor: corun waits for external and nested graphs", "[executor][corun]") {
    const int workers = GENERATE(1, 4);
    tfl::Executor executor(workers);
    int count = 0;
    tfl::Flow empty;
    REQUIRE_NOTHROW(executor.corun(empty));
    REQUIRE_NOTHROW(executor.corun(tfl::Flow{}));
    for (int i = 0; i < 100; ++i) {
        executor.corun(make_flow(count)); // 临时图在同步调用返回后才销毁。
        REQUIRE(count == 2 * (i + 1));
    }
    auto parent = executor.async([&] {
        GraphHolder holder{make_flow(count)};
        executor.corun(holder);
        executor.corun(std::move(holder)); // corun 借用图，不移动图的所有权。
        executor.corun(empty);
        return holder.flow.size();
    });
    REQUIRE(parent.get() == 2);
    REQUIRE(count == 204);
    executor.wait_for_all();
    REQUIRE(executor.num_topologies() == 0);
}

TEST_CASE("Executor: corun from another executor uses the target workers", "[executor][corun]") {
    tfl::Executor caller(1), target(1);
    const auto target_thread = target.async([] { return std::this_thread::get_id(); }).get();
    std::thread::id actual;
    auto done = caller.async([&] {
        tfl::Flow flow;
        (void)flow.emplace([&] { actual = std::this_thread::get_id(); });
        target.corun(flow);
    });
    REQUIRE_NOTHROW(done.get());
    REQUIRE(actual == target_thread);
}

TEST_CASE("Executor: concurrent external corun publishes every source exactly once", "[executor][corun][stress]") {
    const int workers = GENERATE(1, 3, 4);
    tfl::Executor executor(workers);
    std::atomic<bool> correct{true};
    std::vector<std::thread> callers;
    for (int caller = 0; caller < 4; ++caller) {
        callers.emplace_back([&] {
            for (int sources : {1, workers, workers + 1, workers * 2, workers * 2 + 1, 1031}) {
                for (int repeat = 0; repeat < 3; ++repeat) {
                    std::vector<std::atomic<int>> visits(static_cast<std::size_t>(sources));
                    tfl::Flow graph;
                    for (int i = 0; i < sources; ++i) {
                        (void)graph.emplace([&, i] { ++visits[i]; });
                    }
                    executor.corun(graph);
                    for (const auto& visit : visits) {
                        if (visit.load() != 1) correct = false;
                    }
                } // Destroy completed graph and captures immediately.
            }
        });
    }
    for (auto& caller : callers) caller.join();
    REQUIRE(correct.load());
    executor.wait_for_all();
    REQUIRE(executor.num_topologies() == 0);
}

TEST_CASE("corun: exceptions are local and the graph can be reused", "[corun][exception]") {
    tfl::Executor executor(1);
    const int scope = GENERATE(0, 1, 2, 3); // external / Executor worker / Runtime / TaskGroup
    bool fail = true;
    int count = 0;
    tfl::Flow graph;
    auto source = graph.emplace([&] { if (fail) throw std::runtime_error("corun child"); });
    auto sink = graph.emplace([&] { ++count; });
    source.precede(sink);
    auto recover = [&](auto& runner) {
        bool caught = false;
        try { runner.corun(graph); }
        catch (const std::runtime_error& e) { caught = std::string(e.what()) == "corun child"; }
        const bool skipped = count == 0;
        fail = false;
        runner.corun(graph);
        return caught && skipped && count == 1;
    };
    if (scope == 0) {
        REQUIRE(recover(executor));
    } else {
        auto parent = executor.async([&](tfl::Runtime& rt) {
            if (scope == 1) return recover(executor);
            if (scope == 2) return recover(rt);
            tfl::TaskGroup group(rt);
            const bool recovered = recover(group);
            group.wait();
            return recovered;
        });
        REQUIRE(parent.get());
    }
}

TEST_CASE("Runtime and TaskGroup: forwarding graph holders and local waits", "[corun][runtime][task-group]") {
    tfl::Executor executor(1);
    const bool use_group = GENERATE(false, true);
    int count = 0;
    auto parent = executor.async([&](tfl::Runtime& rt) {
        auto run = [&](auto& runner) {
            GraphHolder holder{make_flow(count)};
            runner.run(std::move(holder)); // run 借用，holder 必须存活到 wait 返回。
            runner.wait();
            runner.corun(GraphHolder{make_flow(count)});
            runner.corun(tfl::Flow{});
            return holder.flow.size();
        };
        if (use_group) {
            tfl::TaskGroup group(rt);
            return run(group);
        }
        return run(rt);
    });
    REQUIRE(parent.get() == 2);
    REQUIRE(count == 4);
}

TEST_CASE("corun: local wait does not require unrelated blocked children", "[corun][runtime][task-group]") {
    tfl::Executor executor(1);
    const bool use_group = GENERATE(false, true);
    tfl::Semaphore gate(1, 0);
    std::atomic<int> count{0};
    tfl::Flow blocked;
    blocked.emplace([&] { ++count; }).acquire(gate).release(gate);
    auto parent = executor.async([&](tfl::Runtime& rt) {
        auto run = [&](auto& runner) {
            runner.run(blocked);
            tfl::Flow local;
            (void)local.emplace([&] { ++count; });
            runner.corun(local);
            tfl::Flow release;
            release.emplace([] {}).release(gate);
            runner.corun(release);
            runner.wait();
        };
        if (use_group) {
            tfl::TaskGroup group(rt);
            run(group);
        } else {
            run(rt);
        }
    });
    REQUIRE_NOTHROW(parent.get());
    REQUIRE(count.load() == 2);
    REQUIRE(gate.value() == 1);
}

TEST_CASE("corun: Runtime and TaskGroup inherit stop, Executor is independent", "[corun][stop]") {
    tfl::Executor executor(1);
    int count = 0;
    tfl::AsyncTask<bool> parent;
    parent = executor.defer_async([&](tfl::Runtime& rt) {
        (void)parent.request_stop();
        auto graph = make_flow(count);
        rt.corun(graph);
        tfl::TaskGroup group(rt);
        group.corun(graph);
        const bool skipped = count == 0;
        executor.corun(graph);
        return skipped;
    });
    REQUIRE(parent.start().get());
    REQUIRE(count == 2);
}
