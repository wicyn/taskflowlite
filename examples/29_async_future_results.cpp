/// @file 29_async_future_results.cpp
/// @brief 演示 AsyncFuture：共享结果、重复 get、引用结果和 Future 前驱。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <memory>
#include <syncstream>
int main() {
    std::osyncstream(std::cout) << "=== Example 29: AsyncFuture Results ===\n";
    tfl::Executor executor(2);
    auto owned = executor.async([] { return std::make_unique<int>(42); });
    auto shared = owned;  // 复制句柄，共享结果；不复制 unique_ptr
    const auto& value = owned.get();
    std::osyncstream(std::cout) << "Shared value: " << *value << ", again: " << *shared.get() << "\n";

    auto next = executor.async([shared] { return *shared.get() + 1; }, shared);
    std::osyncstream(std::cout) << "Dependent result: " << next.get() << "\n";
    owned.reset();  // 其他句柄继续保活结果

    int external = 7;
    auto reference = executor.async([&]() -> int& { return external; });
    reference.get() = 9;
    std::osyncstream(std::cout) << "Reference result updates external value: " << external << "\n";
    return next.get() == 43 && external == 9 ? 0 : 1;
}
