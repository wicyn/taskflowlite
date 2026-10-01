/// @file 37_no_exceptions.cpp
/// @brief 不抛异常的任务代码，可配合 TFL_ENABLE_EXCEPTIONS=0 或禁用编译器异常。
#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>

int main() {
    tfl::Executor executor(1);
    tfl::Semaphore resource(1);
    if (!resource.reset(2)) return 1;

    int result = 0;
    tfl::Flow flow;
    auto task = flow.emplace([&]() noexcept { result = 42; });
    task.acquire(resource).release(resource);
    executor.corun(flow);

    // 禁用异常时，原本由库抛出的错误会终止程序；请勿依靠 catch 处理非法调用。
    // 同一程序中所有使用 TaskflowLite 的翻译单元应采用相同宏配置。
    std::cout << "TFL_ENABLE_EXCEPTIONS=" << TFL_ENABLE_EXCEPTIONS
              << ", result=" << result << '\n';
    return result == 42 && resource.value() == 2 ? 0 : 1;
}
