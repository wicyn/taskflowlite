/// @file 30_worker_handler.cpp
/// @brief 演示 WorkerHandler 线程生命周期回调与 Runtime 上下文查询。

#include "../taskflowlite/taskflowlite.hpp"
#include <atomic>
#include <iostream>
#include <syncstream>
class LifecycleHandler : public tfl::WorkerHandler {
public:
    std::atomic<int> starts{0}, stops{0}, failures{0};
    void on_start(tfl::Worker&) noexcept override { ++starts; }
    void on_stop(tfl::Worker&, const std::exception_ptr& error) noexcept override {
        ++stops;
        if (error) ++failures;
    }
};

int main() {
    std::osyncstream(std::cout) << "=== Example 30: Worker Lifecycle ===\n";
    LifecycleHandler handler;  // handler 必须在 Executor 析构完成后才能销毁
    {
        tfl::Executor executor(handler, 2);
        auto worker_id = executor.async([](tfl::Runtime& rt) { return rt.worker().id(); });
        std::osyncstream(std::cout) << "Task ran on worker " << worker_id.get() << "\n";
    }
    std::osyncstream(std::cout) << "Started: " << handler.starts << ", stopped: " << handler.stops << "\n";
    return handler.starts == 2 && handler.stops == 2 && handler.failures == 0 ? 0 : 1;
}
