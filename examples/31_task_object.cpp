/// @file 31_task_object.cpp
/// @brief 原地构造不可移动业务对象，访问状态，并组合 Runtime / SubFlow / Module。
#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <memory>
#include <tuple>

namespace {
struct Counter {
    int value;
    explicit Counter(int initial) : value(initial) {}
    Counter(const Counter&) = delete;
    Counter(Counter&&) = delete;
    void operator()() { ++value; }
};

struct RuntimeSum {
    int value{0};
    void operator()(tfl::Runtime& runtime) {
        runtime.silent_async([this] { value = 10 + 20; });
        runtime.wait();
    }
};

struct DynamicSum {
    int value{0};
    void operator()(tfl::SubFlow& subflow) {
        subflow.emplace([this] { value += 5; });
        subflow.run();  // 动态子图必须显式运行。
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
    tfl::Flow flow;

    // 构造参数直接转发到 Work 内部，不产生临时 Counter。
    auto counter = flow.emplace_object<Counter>(40);
    counter.name("counter");  // 先保存类型句柄；链式配置返回的是 Task&。
    auto shared = counter;
    shared.object().value = 41;  // 复制句柄共享业务对象。
    auto runtime = flow.emplace_object<RuntimeSum>();
    auto dynamic = flow.emplace_object<DynamicSum>();
    auto batch = flow.emplace_object<Batch>(std::make_tuple(std::make_unique<int>(3)), 2);
    auto done = flow.emplace([] {});
    flow.linearize(counter, runtime, dynamic, batch, done);

    executor.async(flow).get();
    // object() 不会等待或加锁；只在运行前或等待完成后读写业务状态。
    std::cout << "Counter: " << counter.object().value << '\n'
              << "Runtime: " << runtime.object().value << '\n'
              << "SubFlow: " << dynamic.object().value << '\n'
              << "Module (3 x 2): " << batch.object().total << '\n';
    const bool ok = counter.object().value == 42 && runtime.object().value == 30
        && dynamic.object().value == 5 && batch.object().total == 6;

    flow.erase(counter, runtime, dynamic, batch, done);
    // TaskObject 为弱句柄：erase 后不得再使用 counter/shared 等句柄访问节点。
    return ok && flow.empty() ? 0 : 1;
}
