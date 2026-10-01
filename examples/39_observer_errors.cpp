/// @brief 观察者异常通过任务结果报告；同一节点的其他观察者仍会收到通知。
#include <taskflowlite/taskflowlite.hpp>
#include <iostream>
#include <stdexcept>

struct Observer final : tfl::TaskObserver {
    void on_before(tfl::WorkerView) override { throw std::runtime_error("trace unavailable"); }
    void on_after(tfl::WorkerView) override {}
};

int main() {
    tfl::Executor executor(1);
    tfl::Flow flow;
    int calls = 0;
    auto task = flow.emplace([&] { ++calls; });
    (void)task.register_observer<Observer>();
    try {
        executor.corun(flow);
    } catch (const std::runtime_error& error) {
        std::cout << "Observer error: " << error.what() << '\n';
        return calls == 1 ? 0 : 1;
    }
    return 1;
}
