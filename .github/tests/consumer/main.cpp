#include <taskflowlite/taskflowlite.hpp>

#include <atomic>

int main() {
    std::atomic<int> count{0};
    tfl::Flow flow;
    auto first = flow.emplace([&] { count.fetch_add(1); });
    auto second = flow.emplace([&] { count.fetch_add(1); });
    first.precede(second);
    tfl::Executor executor(2);
    executor.async(flow).get();
    executor.corun(flow);
    auto result = executor.async([&](tfl::Runtime& rt) {
        tfl::TaskGroup group(rt);
        group.corun(flow);
        auto first = group.async([] { return 20; });
        auto last = group.async([first] { return first.get() + 22; }, first);
        group.wait();
        return last.get();
    });
    if (result.get() != 42) return 1;
    tfl::Semaphore gate(1);
    return count.load() == 6 && gate.reset(2) && gate.value() == 2 ? 0 : 1;
}
