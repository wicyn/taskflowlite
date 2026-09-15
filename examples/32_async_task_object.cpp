/// @file 32_async_task_object.cpp
/// @brief 异步业务对象：延迟启动、共享/移动句柄、结果、依赖与 Module 重载。
#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <memory>
#include <tuple>

namespace {
struct Multiply {
    int input;
    int factor{2};
    int calls{0};
    explicit Multiply(int value) : input(value) {}
    Multiply(const Multiply&) = delete;
    Multiply(Multiply&&) = delete;
    int operator()() { ++calls; return input * factor; }
};

struct RuntimeValue {
    int value{0};
    int operator()(tfl::Runtime& runtime) {
        runtime.silent_async([this] { value = 7; });
        runtime.wait();
        return value;
    }
};

struct DynamicValue {
    int value{0};
    int operator()(tfl::SubFlow& subflow) {
        subflow.emplace([this] { value = 11; });
        subflow.run();
        subflow.wait();  // 返回依赖子图计算的值前，需要显式等待。
        return value;
    }
};

struct Batch {
    tfl::Flow flow;
    std::unique_ptr<int> step;
    int total{0};
    Batch() : Batch(std::make_unique<int>(1)) {}
    explicit Batch(std::unique_ptr<int> increment) : step(std::move(increment)) {
        flow.emplace([this] { total += *step; });
    }
    Batch(const Batch&) = delete;
    Batch(Batch&&) = delete;
    tfl::Graph& graph() noexcept { return flow.graph(); }
    const tfl::Graph& graph() const noexcept { return flow.graph(); }
};
}  // namespace

int main() {
    tfl::Executor executor(2);
    auto task = executor.defer_async_object<Multiply>(14);
    task.name("multiply");  // 先保存 AsyncTaskObject，再配置/启动。
    task.object().factor = 3;  // Idle 状态，尚未执行。
    auto shared = task;
    auto moved = std::move(task);  // task 置空，业务对象留在原地址。
    moved.start();

    auto runtime = executor.defer_async_object<RuntimeValue>();
    auto dynamic = executor.defer_async_object<DynamicValue>();
    runtime.start(shared);  // 对象句柄也能作为异步前驱。
    dynamic.start(runtime);
    const int dynamic_result = dynamic.get();
    std::cout << "Result: " << shared.get() << ", calls: " << shared.object().calls << '\n'
              << "Runtime: " << runtime.get() << ", SubFlow: " << dynamic_result << '\n';
    bool ok = shared.get() == 42 && shared.object().calls == 1
        && runtime.get() == 7 && dynamic_result == 11 && !task.valid();
    moved.reset();  // shared 继续持有任务和业务对象。

    auto once = executor.defer_async_object<Batch>();
    auto counted = executor.defer_async_object<Batch>(3);
    auto until = executor.defer_async_object<Batch>([n = 0]() mutable { return n++ == 2; });
    int callbacks = 0;
    auto tuple_batch = executor.defer_async_object<Batch>(
        std::make_tuple(std::make_unique<int>(5)), 2, [&] { ++callbacks; });
    once.start();
    counted.start();
    until.start();
    tuple_batch.start();
    once.get();
    counted.get();
    until.get();
    tuple_batch.get();
    std::cout << "Modules: " << once.object().total << ", " << counted.object().total
              << ", " << until.object().total << ", " << tuple_batch.object().total << '\n';
    ok = ok && once.object().total == 1 && counted.object().total == 3
        && until.object().total == 2 && tuple_batch.object().total == 10 && callbacks == 1;
    // object() 不同步执行；这里所有访问均发生在 get() 完成后。
    return ok ? 0 : 1;
}
