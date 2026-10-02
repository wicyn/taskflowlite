#include "test_common.hpp"

TEST_CASE("Context: names, types and executor are available in every callback", "[context]") {
    tfl::Executor executor(1);
    tfl::Flow flow;
    int inspected = 0;
    auto check = [&](tfl::Context& ctx, tfl::TaskType expected, std::string_view name) {
        const tfl::Context& view = ctx;
        if (&ctx.executor() != &executor || &view.executor() != &executor ||
            ctx.type() != expected || ctx.name() != name || ctx.stop_requested() ||
            ctx.worker().id() != 0) {
            throw std::runtime_error("invalid callback context");
        }
        ++inspected;
    };
    flow.emplace([&](tfl::Runtime& rt) { check(rt, tfl::TaskType::Runtime, "runtime"); }).name("runtime");
    flow.emplace([&](tfl::SubFlow& sf) { check(sf, tfl::TaskType::Graph, "subflow"); }).name("subflow");
    flow.emplace([&](tfl::Branch& br) { check(br, tfl::TaskType::Branch, "branch"); }).name("branch");
    flow.emplace([&](tfl::MultiBranch& br) { check(br, tfl::TaskType::MultiBranch, "multi-branch"); }).name("multi-branch");
    flow.emplace([&](tfl::Jump& j) { check(j, tfl::TaskType::Jump, "jump"); }).name("jump");
    flow.emplace([&](tfl::MultiJump& j) { check(j, tfl::TaskType::MultiJump, "multi-jump"); }).name("multi-jump");
    executor.corun(flow);
    REQUIRE(inspected == 6);
}

TEST_CASE("Context: Runtime children and groups inherit parent stop requests", "[context][runtime][stop]") {
    tfl::Executor executor(1);
    const bool silent = GENERATE(false, true);
    tfl::AsyncTask<bool> parent;
    parent = executor.defer_async([&](tfl::Runtime& rt) {
        bool observed = false;
        auto child = [&](tfl::Runtime& nested) {
            observed = nested.stop_requested();
            tfl::TaskGroup group(nested);
            auto grandchild = group.async([](tfl::Runtime& ctx) { return ctx.stop_requested(); });
            group.wait();
            observed = observed && grandchild.get();
        };
        if (silent) rt.silent_async(child);
        else (void)rt.async(child);
        const bool first = parent.request_stop();
        rt.wait();
        return first && rt.stop_requested() && observed;
    });
    REQUIRE(parent.start().get());
}

TEST_CASE("Context: running SubFlow sees cooperative stop", "[context][subflow][stop]") {
    tfl::Executor executor(1);
    tfl::AsyncTask<bool> task;
    task = executor.defer_async([&](tfl::SubFlow& sf) {
        const bool initially_running = !sf.stop_requested();
        (void)task.request_stop();
        return initially_running && sf.stop_requested() && sf.name() == "dynamic";
    }).name("dynamic");
    REQUIRE(task.start().get());
}
