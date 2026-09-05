/// @file test_submission.cpp
/// @brief Executor / Runtime 提交重载矩阵 —— 图、次数、谓词、回调与 Future 前驱。

#include "test_common.hpp"

using tfl_test::TestEnv;

namespace {

template <typename Submitter>
void submit_graph(Submitter& submitter, tfl::Flow& flow, int mode,
                  int& count, int& callbacks) {
    auto callback = [&] { ++callbacks; };
    auto predicate = [&] { return count == 3; };
    auto predecessor = submitter.async([] {});
    switch (mode) {
    case 0: (void)submitter.async(flow, predecessor); break;
    case 1: (void)submitter.async(flow, callback, predecessor); break;
    case 2: (void)submitter.async(flow, 3ULL, predecessor); break;
    case 3: (void)submitter.async(flow, 3ULL, callback, predecessor); break;
    case 4: (void)submitter.async(flow, predicate, predecessor); break;
    case 5: (void)submitter.async(flow, predicate, callback, predecessor); break;
    case 6: submitter.silent_async(flow); break;
    case 7: submitter.silent_async(flow, callback); break;
    case 8: submitter.silent_async(flow, 3ULL); break;
    case 9: submitter.silent_async(flow, 3ULL, callback); break;
    case 10: submitter.silent_async(flow, predicate); break;
    case 11: submitter.silent_async(flow, predicate, callback); break;
    }
}

}  // namespace

/// @test [submission][graph] 三种提交上下文覆盖相同的图重载。
TEST_CASE("Submission: graph overloads with callbacks and future dependencies", "[submission][graph]") {
    TestEnv env(1);
    const int context = GENERATE(0, 1, 2);
    const int mode = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
    int count = 0, callbacks = 0;
    tfl::Flow flow;
    (void)flow.emplace([&] { ++count; });
    if (context == 0) {
        submit_graph(env.executor, flow, mode, count, callbacks);
        env.executor.wait_for_all();
    } else {
        env.executor.async([&](tfl::Runtime& rt) {
            if (context == 1) {
                submit_graph(rt, flow, mode, count, callbacks);
                rt.wait();
            } else {
                tfl::TaskGroup group(rt);
                submit_graph(group, flow, mode, count, callbacks);
                group.wait();
            }
        }).get();
    }
    REQUIRE(count == (mode % 6 < 2 ? 1 : 3));
    REQUIRE(callbacks == (mode % 2));
}
