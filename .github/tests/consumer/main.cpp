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
    return count.load() == 2 ? 0 : 1;
}
