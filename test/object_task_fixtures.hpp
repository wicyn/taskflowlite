/// @file object_task_fixtures.hpp
/// @brief 对象任务共用夹具；删除复制/移动以验证真正的原地构造。
#pragma once

#include "../taskflowlite/taskflowlite.hpp"
#include <atomic>
#include <memory>
#include <stdexcept>

namespace tfl_test::objects {

struct Immovable {
    Immovable() = default;
    Immovable(const Immovable&) = delete;
    Immovable(Immovable&&) = delete;
    Immovable& operator=(const Immovable&) = delete;
    Immovable& operator=(Immovable&&) = delete;
};

struct Counter : Immovable {
    int value;
    int calls{0};
    explicit Counter(int initial = 0) : value(initial) {}
    void operator()() { ++value; ++calls; }
};

template <typename Control, bool Multiple = false>
struct Router : Immovable {
    int calls{0};
    int selected{1};
    void operator()(Control& control) {
        ++calls;
        if constexpr (Multiple) {
            control.select(0, 2);
        } else {
            control.select(selected);
        }
    }
};

struct RuntimeJob : Immovable {
    int value{0};
    int operator()(tfl::Runtime& runtime) {
        runtime.silent_async([this] { value += 7; });
        runtime.wait();
        return value;
    }
};

struct SubflowJob : Immovable {
    int value{0};
    int operator()(tfl::SubFlow& subflow) {
        subflow.emplace([this] { value += 11; });
        subflow.run();
        subflow.wait();  // run() 只提交；读取子任务写入的结果前必须等待。
        return value;
    }
};

// 捕获 this 的子图还验证 Module 对象在构造后没有改变地址。
struct Module : Immovable {
    tfl::Flow flow;
    std::unique_ptr<int> step;
    int value{0};
    int calls{0};
    Module() : Module(std::make_unique<int>(1)) {}
    explicit Module(std::unique_ptr<int> increment) : step(std::move(increment)) {
        flow.emplace([this] { value += *step; ++calls; });
    }
    tfl::Graph& graph() noexcept { return flow.graph(); }
    const tfl::Graph& graph() const noexcept { return flow.graph(); }
};

struct Lifetime : Immovable {
    std::atomic<int>& destroyed;
    explicit Lifetime(std::atomic<int>& count) : destroyed(count) {}
    ~Lifetime() { ++destroyed; }
    int operator()() { return 42; }
};

struct ThrowingConstructor : Immovable {
    std::unique_ptr<int> resource;
    explicit ThrowingConstructor(std::unique_ptr<int> input) : resource(std::move(input)) {
        throw std::runtime_error("object construction failed");
    }
    void operator()() {}
};

}  // namespace tfl_test::objects
