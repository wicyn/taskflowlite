/// @brief 查询执行上下文，并在动态任务中协作响应停止请求。
#include <taskflowlite/taskflowlite.hpp>
#include <iostream>

int main() {
    tfl::Executor executor(1);
    bool child_observed_stop = false;
    tfl::AsyncTask<void> parent;
    parent = executor.defer_async([&](tfl::Runtime& rt) {
        std::cout << "Task: " << rt.name() << ", worker: " << rt.worker().id() << '\n';
        rt.silent_async([&](tfl::Runtime& child) {
            // 动态 callable 仍会进入执行，可用继承的停止请求结束业务工作。
            child_observed_stop = child.stop_requested();
            if (child_observed_stop) return;
            std::cout << "Doing work\n";
        });
        (void)parent.request_stop();
        rt.wait();
    }).name("parent");
    parent.start().get();
    std::cout << "Child observed stop: " << std::boolalpha << child_observed_stop << '\n';
    return child_observed_stop ? 0 : 1;
}
