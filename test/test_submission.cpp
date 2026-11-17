/// @file test_submission.cpp
/// @brief Executor / Runtime 提交重载矩阵 —— 图、次数、谓词、回调与 Future 前驱。

#include "test_common.hpp"

using tfl_test::TestEnv;

namespace {

template <typename Submitter, std::integral I>
void submit_graph(Submitter& submitter, tfl::Flow& flow, int mode,
                  I num, int& count, int& callbacks) {
    auto callback = [&] { ++callbacks; };
    auto predicate = [&, num] { return count == static_cast<int>(num); };
    auto predecessor = submitter.async([] {});
    switch (mode) {
    case 0: (void)submitter.async(flow, predecessor); break;
    case 1: (void)submitter.async(flow, callback, predecessor); break;
    case 2: (void)submitter.async(flow, num, predecessor); break;
    case 3: (void)submitter.async(flow, num, callback, predecessor); break;
    case 4: (void)submitter.async(flow, predicate, predecessor); break;
    case 5: (void)submitter.async(flow, predicate, callback, predecessor); break;
    case 6: submitter.silent_async(flow); break;
    case 7: submitter.silent_async(flow, callback); break;
    case 8: submitter.silent_async(flow, num); break;
    case 9: submitter.silent_async(flow, num, callback); break;
    case 10: submitter.silent_async(flow, predicate); break;
    case 11: submitter.silent_async(flow, predicate, callback); break;
    }
}

}  // namespace

/// @test [submission][graph] 三种提交上下文覆盖相同的图重载。
TEMPLATE_TEST_CASE("Submission: graph overloads with integral counts callbacks and future dependencies",
                   "[submission][graph][count]", bool, std::uint8_t, int, std::uint64_t) {
    TestEnv env(1);
    const auto num = static_cast<TestType>(GENERATE(0, 3));
    const int context = GENERATE(0, 1, 2);
    const int mode = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
    int count = 0, callbacks = 0;
    tfl::Flow flow;
    (void)flow.emplace([&] { ++count; });
    if (context == 0) {
        submit_graph(env.executor, flow, mode, num, count, callbacks);
        env.executor.wait_for_all();
    } else {
        env.executor.async([&](tfl::Runtime& rt) {
            if (context == 1) {
                submit_graph(rt, flow, mode, num, count, callbacks);
                rt.cowait();
            } else {
                tfl::TaskGroup group(rt);
                submit_graph(group, flow, mode, num, count, callbacks);
                group.cowait();
            }
        }).get();
    }
    REQUIRE(count == (mode % 6 < 2 ? 1 : static_cast<int>(num)));
    REQUIRE(callbacks == (mode % 2));
}
