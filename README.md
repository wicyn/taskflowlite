# TaskflowLite

[![Ubuntu](https://github.com/wicyn/taskflowlite/actions/workflows/ubuntu.yml/badge.svg?branch=main)](https://github.com/wicyn/taskflowlite/actions/workflows/ubuntu.yml)
[![Windows](https://github.com/wicyn/taskflowlite/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/wicyn/taskflowlite/actions/workflows/windows.yml)
[![macOS](https://github.com/wicyn/taskflowlite/actions/workflows/macos.yml/badge.svg?branch=main)](https://github.com/wicyn/taskflowlite/actions/workflows/macos.yml)
[![CodeQL](https://github.com/wicyn/taskflowlite/actions/workflows/codeql-analysis.yml/badge.svg?branch=main)](https://github.com/wicyn/taskflowlite/actions/workflows/codeql-analysis.yml)
[![CI Extras](https://github.com/wicyn/taskflowlite/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/wicyn/taskflowlite/actions/workflows/ci.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue?logo=cplusplus)](#编译与集成)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Header Only](https://img.shields.io/badge/Header--Only-Yes-success)](#直接包含头文件)

**简体中文** · [English](README.en.md)

TaskflowLite（简称 tfl）是一个受 [Taskflow](https://github.com/taskflow/taskflow) 启发的现代 C++20、header-only 任务调度库，支持依赖图、动态任务、控制流、协作等待与共享异步结果。

---

## 目录

1. [快速开始](#快速开始)
2. [构建 DAG](#构建-dag)
3. [任务类型全解](#任务类型全解)
4. [运行时动态调度](#运行时动态调度)
5. [控制流](#控制流)
6. [资源控制](#资源控制)
7. [可视化](#可视化)
8. [编译与集成](#编译与集成)
9. [项目结构](#项目结构)
10. [性能数据](#性能数据)
11. [示例与测试](#示例与测试)
12. [迁移与注意事项](#迁移与注意事项)
13. [许可证](#许可证)

## 快速开始

本文对应当前源码版本 **2.2.0**，使用 **C++20**。除最简示例外，下面的 C++ 片段相互独立，可放在 `main()` 中；默认已创建 `tfl::Executor executor(4)`，并按说明包含所需标准头文件。

### 最简示例

```cpp
#include <cstring>
#include <iostream>
#include <syncstream>
#include "taskflowlite/taskflowlite.hpp"

int main() {
    tfl::Executor executor(4);
    tfl::Flow flow;

    auto [A, B, C, D] = flow.emplace(
        [] { std::osyncstream(std::cout) << "Task A\n"; },
        [] { std::osyncstream(std::cout) << "Task B\n"; },
        [] { std::osyncstream(std::cout) << "Task C\n"; },
        [] { std::osyncstream(std::cout) << "Task D\n"; }
    );

    // A -> {B, C} -> D
    A.precede(B, C);
    D.succeed(B, C);

    executor.async(flow).get();
}
```

A 先执行，B、C 可并行，D 等两者完成后执行。`get()` 等待并传播异常；Future 的 `wait()` 只等待。每条并行日志使用独立的 `std::osyncstream`，避免消息交错。

当前 `utility.hpp` 使用 `std::memcpy` 但缺少直接的 `<cstring>` 包含；上例先包含 `<cstring>` 以兼容该源码快照，库头文件自身仍应补齐包含。

### 带参数的任务

使用 Lambda 捕获或 `<functional>` 中的 `std::bind_front` 绑定业务参数。`async(callable, ...)` 后面的参数是异步依赖，不是传给 callable 的业务参数。

```cpp
tfl::Flow flow;
int counter = 0;

auto [first, second] = flow.emplace(
    std::bind_front([](int value) {
        std::osyncstream(std::cout) << "Value: " << value << "\n";
    }, 42),
    std::bind_front([](int& value) { value = 100; }, std::ref(counter))
);
first.precede(second);
executor.async(flow).get();
// counter == 100
```

### 重复执行与完成回调

```cpp
tfl::Flow flow;
int count = 0;
(void)flow.emplace([&count] { ++count; });

executor.async(flow, 5ULL).get();
// count == 5

executor.async(flow, [&count]() noexcept { return count >= 10; }).get();
// count == 10

bool finished = false;
executor.async(flow, 2ULL, [&finished] { finished = true; }).get();
// count == 12 && finished
```

终止谓词在**每轮执行前**检查：返回 `true` 就停止，第一次返回 `true` 时执行零轮。固定次数为零也不执行图；异常或停止请求可能使实际次数减少。完成回调是整个提交的回调，不是每轮的回调。同一个 `Flow` 必须在上次运行结束后才能再次提交。

---

## 构建 DAG

### 创建与修改节点

```cpp
tfl::Flow flow;
int value = 0;

auto first = flow.placeholder().name("first");
auto second = flow.emplace([&value] { value *= 2; }).name("second");
first.work([&value] { value = 21; });
first.precede(second);

executor.async(flow).get();
// value == 42

second.work([&value] { value += 1; });
executor.async(flow).get();
// value == 22
```

`Task::work(...)` 可重绑定 callable 或模块图，`placeholder()` 用于先连边后填充任务。函数指针、仿函数和成员函数均可通过 `std::bind_front` 适配。结构修改、重绑定和观察者注册必须在任务未执行时完成。

### 依赖与图操作

```cpp
tfl::Flow flow;
auto [a, b, c] = flow.emplace([] {}, [] {}, [] {});

flow.linearize(a, b, c);  // a -> b -> c
// Equivalent: a.precede(b); b.precede(c);

(void)flow.size();
(void)flow.empty();
flow.for_each([](tfl::Task task) { (void)task.name(); });

b.remove_successor(c);
flow.erase(c);
c.reset();
flow.clear();
```

`precede` 返回调用者，因此 `a.precede(b).precede(c)` 表示 **a → b、a → c**，不表示 a → b → c。`linearize` 也接受 `Task` 范围和初始化列表。

`Task` / `TaskView` 是非拥有句柄，不会延长节点生命；`erase`、`clear` 或图析构后，相关句柄失效。删除节点还需要断开依赖边，不能笼统宣称整个删除操作是 O(1)。遍历回调中不要增删节点。

### 批量参数包

`tfl::pack` 仍然可用：每个包展开成一次有效的 `emplace` 调用，批量重载要求至少两个包。普通 callable 的业务参数应先绑定；模块可以携带次数或谓词。以下用到 `<utility>`。

```cpp
int count = 0;
tfl::Flow inner;
(void)inner.emplace([&count] { ++count; });

tfl::Flow outer;
auto [module, tail] = outer.emplace(
    tfl::pack{std::move(inner), 2ULL},
    tfl::pack{[] {}}
);
module.precede(tail);
executor.async(outer).get();
// count == 2
```

---

## 任务类型全解

| 类型 | 创建方式 | 含义 |
|------|----------|------|
| Placeholder | `flow.placeholder()` | 仅参与依赖传播 |
| Basic | `flow.emplace([] { ... })` | 普通 callable；图节点不提供 Future 结果 |
| Runtime | `flow.emplace([](tfl::Runtime& rt) { ... })` | 动态派发任务与协作等待 |
| Branch / MultiBranch | 接收 `Branch&` / `MultiBranch&` | 选择一个 / 多个后继 |
| Jump / MultiJump | 接收 `Jump&` / `MultiJump&` | 强制激活一个 / 多个目标 |
| Module | `flow.emplace(inner)` | 执行已有图 |
| SubFlow | 接收 `tfl::SubFlow&` | 执行期间构建动态子图 |

### Branch 与 MultiBranch

```cpp
tfl::Flow flow;
bool success = true;

auto decide = flow.emplace([success](tfl::Branch& branch) {
    branch.select(success ? 0 : 1);
});
auto good = flow.emplace([] {}).name("good");
auto bad = flow.emplace([] {}).name("bad");
decide.precede(good, bad);
executor.async(flow).get();
```

索引按 `precede` 建立的后继顺序，从 0 开始。`branch(0)` 等价于 `select(0)`；`select_if` 接收 `TaskView` 谓词。不选择目标时不调度该分支的后继。单目标的后一次选择覆盖前一次选择。

```cpp
tfl::Flow flow;
auto split = flow.emplace([](tfl::MultiBranch& branch) {
    branch.select(0, 2);
});
auto [a, b, c] = flow.emplace([] {}, [] {}, [] {});
split.precede(a, b, c);
executor.async(flow).get();
```

多目标接口还支持 `select_all()`、`select_if(...)`、`unselect(...)` 和 `reset()`。分支可以让部分路径不执行；不要为某个汇聚节点设置永远无法满足的前驱条件。

### Jump 与 MultiJump

```cpp
tfl::Flow flow;
int attempts = 0;

auto entry = flow.emplace([] {});
auto work = flow.emplace([&attempts] { ++attempts; });
auto retry = flow.emplace([&attempts](tfl::Jump& jump) {
    if (attempts < 3) {
        jump.select(0);
    }
});
entry.precede(work);
work.precede(retry);
retry.precede(work);

executor.async(flow).get();
// attempts == 3
```

Jump 用于受控循环 / 重试；没有选择时，本次跳转不激活目标。`MultiJump` 提供多目标版本。跳转会绕过普通依赖激活条件，目标必须处于允许再次执行的状态；不要用它同时重入正在执行的节点。普通循环依赖不会自动变成合法重试图。

### Module：嵌套已有图

```cpp
int count = 0;
tfl::Flow inner;
(void)inner.emplace([&count] { ++count; });

tfl::Flow outer;
(void)outer.emplace(inner, 3ULL);
executor.async(outer).get();
// count == 3
```

左值图按引用借用，必须存活到模块最后一次执行结束；右值图如 `std::move(inner)` 将所有权交给节点。不要重复移动同一份图，也不要让两个同时执行的模块共用同一可变图。计数 / 谓词规则与顶层图重复执行一致。

### SubFlow：运行时构建子图

```cpp
int result = 0;
auto future = executor.async([&result](tfl::SubFlow& subflow) {
    auto first = subflow.emplace([&result] { result = 21; });
    auto second = subflow.emplace([&result] { result *= 2; });
    first.precede(second);
    subflow.run();
    subflow.wait();
});
future.get();
// result == 42
```

必须显式调用 `SubFlow::run()`；只构图后返回不会自动提交。`wait()` 协作等待已提交子任务，便于在当前 callable 中读取结果；父任务的完成仍会等待已挂接子任务。

同一个动态节点下次执行前会清空并重建子图，上一轮的子节点句柄失效。`SubFlow`、`Runtime` 和分支上下文只在当前回调及其 Worker 线程中有效，不能保存后跨线程使用。MSVC 的无捕获 SubFlow 回归见[迁移与注意事项](#迁移与注意事项)。

---

## 运行时动态调度

### AsyncFuture：共享结果

`Executor::async` 返回已经提交的 `AsyncFuture<R>`。复制 Future 共享同一任务和结果，不复制任务；`get()` 不消费句柄，可以重复调用。以下使用 `<string>`。

```cpp
auto future = executor.async([] { return std::string("ready"); });
auto shared = future;
std::string value = shared.get();
const std::string& view = future.get();
// Keep a handle alive while using view.
(void)value;
(void)view;
```

| 操作 | 语义 |
|------|------|
| `valid()` / `operator bool()` | 句柄是否关联任务 |
| `done()` / `running()` | 执行状态快照 |
| `wait()` | 阻塞等待；不重新抛出任务异常 |
| `get()` | 等待并传播异常；值结果为 `const R&`，左值引用结果为原引用，`void` 无返回值 |
| `has_exception()` | 查询是否已经归档异常 |
| `request_stop()` / `stop_requested()` | 发出 / 查询协作停止请求 |
| `reset()` | 释放此句柄的强引用，不等同于取消或等待 |

值结果的引用不能超过其结果存储生命周期；需要独立拥有时复制结果。引用结果要求原对象保持存活。同一个句柄对象不能一边被读取，一边被其他线程重置、移动或销毁。空 Future 的 `get()` 会抛出 `tfl::Exception`。

### Future 依赖

```cpp
auto left = executor.async([] { return 20; });
auto right = executor.async([] { return 22; });
auto sum = executor.async(
    [left, right] { return left.get() + right.get(); },
    left, right
);
int result = sum.get();
// result == 42
```

`sum` 在两个前驱完成后才会调度，因此该 callable 中读取前驱结果不会占用 Worker 等待未完成的任务。依赖列表不会自动把结果传入 callable，仍需捕获句柄并调用 `get()`。传入的延迟任务必须另外启动。

### AsyncTask：延迟启动

```cpp
tfl::AsyncTask first([] { return 21; });
tfl::AsyncTask second([first] { return first.get() * 2; });

executor.run(second, first);
executor.run(first);
int result = second.get();
// result == 42
```

`AsyncTask<R>` 继承 `AsyncFuture<R>`，可通过 CTAD 推导结果类型，并在 `run()` 前配置名称、观察者和信号量。复制句柄仍共享同一任务；一个底层任务只能成功启动一次，不能通过复制句柄重启。

当前 `run(task, deps...)` 内部只接受 `AsyncTask` 前驱；已有 `AsyncFuture` 前驱请使用上一节的 `async(callable, futures...)`。不要等待尚未启动且没有其他线程负责启动的延迟任务。

### Runtime 与协作等待

```cpp
auto parent = executor.async([](tfl::Runtime& runtime) {
    runtime.silent_async([] {});
    auto left = runtime.async([] { return 20; });
    auto right = runtime.async(std::bind_front([](int n) { return n * 2; }, 11));

    runtime.wait_until([&left, &right]() noexcept {
        return left.done() && right.done();
    });
    runtime.wait();
    return left.get() + right.get();
});
int result = parent.get();
// result == 42
```

需要 `<functional>`。`Runtime::wait()` 等待当前父任务挂接的子任务，`wait_until(pred)` 在谓词为 `true` 时返回。等待期间 Worker 会推进其他就绪任务；这不能消除循环依赖、错误锁顺序或永远不满足的等待条件。

在 Worker 内不要用未完成 Future 的阻塞 `get()` / `wait()` 代替协作等待，也不要调用全执行器的 `wait_for_all()` 等待自己。局部数据或局部图被子任务借用时，必须在这些对象析构前显式等待；父任务的隐式收尾等待不能延长 callable 局部变量的生命周期。

```cpp
executor.async([](tfl::Runtime& runtime) {
    tfl::Flow local;
    (void)local.emplace([] {});
    runtime.corun(local);
}).get();
```

`Runtime::run(graph)` 提交后立即返回，需随后 `wait()`；`corun(graph)` 则提交并协作等待。`Runtime` 派生的任务始终参与父任务完成计数；`async<false>` / `silent_async<false>` 只切断 Topology 的停止 / 异常父链，不会让子任务脱离父任务的完成等待。

### TaskGroup：作用域任务组

```cpp
auto result = executor.async([](tfl::Runtime& runtime) {
    tfl::TaskGroup group(runtime);
    auto left = group.async([] { return 20; });
    auto right = group.async([] { return 22; });
    group.wait();
    return left.get() + right.get();
});
int sum = result.get();
// sum == 42
```

`TaskGroup` 从当前 `Context` 构造，提供 `async`、`silent_async`、`run`、`wait`、`request_stop`、`stop_requested` 和 `size`。它必须在创建它的回调及 Worker 内使用和销毁。

析构会协作等待；正常退出作用域时可能传播组内异常，异常展开期间不会再抛第二个异常。捕获数据应先于组声明，让组先析构并等完任务。借用组停止域的子任务句柄不要逃逸组作用域；向外返回结果副本。

---

## 控制流

### 异常处理

```cpp
auto future = executor.async([]() -> int {
    throw std::runtime_error("task failed");
});
future.wait();
try {
    (void)future.get();
} catch (const std::runtime_error& error) {
    std::osyncstream(std::cout) << error.what() << "\n";
}
```

需要 `<stdexcept>`。普通任务异常由框架捕获并沿所属执行域传播，Future 的 `get()` 负责重新抛出。`silent_async` 没有结果句柄；需要检查任务失败时应使用 `async`，或在任务体内明确处理异常。`Executor::wait_for_all()` 负责等待，不提供异常结果。

局部恢复可以使用独立 `TaskGroup` 并在组作用域外捕获异常。若要在 `Runtime::wait()` 或 `SubFlow::wait()` 处截获子任务异常，需包含 `taskflowlite/core/scoped_exception_anchor.hpp`，在提交子任务**之前**构造 `tfl::ScopedExceptionAnchor anchor(context)`，并保持到等待完成；仅调用 `wait()` 不会自动建立局部异常锚点。

### 协作取消

```cpp
tfl::AsyncTask<void> task;
task = tfl::AsyncTask([&task] {
    for (int i = 0; i < 1'000'000; ++i) {
        if (task.stop_requested()) {
            return;
        }
        // Perform one bounded unit of work.
    }
});
executor.run(task);
task.request_stop();
task.get();
```

停止请求不是强制终止线程。尚未执行的工作可能被跳过，已经进入的长任务需要主动检查停止状态；阻塞 I/O 不会被自动打断。被取消且未产生值的任务，读取其值结果不能视为成功；应按异常 / 取消路径处理。本例使用 `void` 结果，且任务句柄保持到执行结束。

### TaskObserver 与 WorkerHandler

```cpp
struct CounterObserver : tfl::TaskObserver {
    std::atomic<int> before{0};
    std::atomic<int> after{0};

    void on_before(tfl::WorkerView) noexcept override {
        before.fetch_add(1, std::memory_order_relaxed);
    }
    void on_after(tfl::WorkerView) noexcept override {
        after.fetch_add(1, std::memory_order_relaxed);
    }
};

tfl::Flow flow;
auto task = flow.emplace([] {});
auto observer = task.register_observer<CounterObserver>();
executor.async(flow).get();
// observer->before == 1 && observer->after == 1
```

需要 `<atomic>`。观察者回调接收 `WorkerView`，且必须 `noexcept`；共享状态需自行同步。可抛异常的日志、容器分配不能直接逃出这些回调。

`WorkerHandler` 则观察线程生命周期，签名为 `on_start(Worker&) noexcept` 和 `on_stop(Worker&, const std::exception_ptr&) noexcept`。用 `Executor(handler, workers)` 注册，handler 必须存活到 Executor 析构完成。它不是旧的任务异常恢复策略；完整示例见 [30_worker_handler.cpp](examples/30_worker_handler.cpp)。

---

## 资源控制

### Semaphore：任务级并发限流

```cpp
tfl::Semaphore slots(4);
tfl::Flow flow;

for (int i = 0; i < 20; ++i) {
    flow.emplace([i] {
        std::osyncstream(std::cout) << "Job " << i << "\n";
    }).acquire(slots).release(slots);
}
executor.async(flow).get();
```

配额不足时，任务登记等待并让出 Worker，之后由释放操作重新调度。一次获取多个配额使用 `.acquire(sem, count)`；多个信号量可链式配置。信号量必须存活到所有引用它的任务结束。

```cpp
tfl::Semaphore permits(3, 0);
tfl::Flow flow;
auto producer = flow.emplace([] {}).release(permits, 3);
auto consumer = flow.emplace([] {}).acquire(permits, 3).release(permits, 3);
producer.precede(consumer);
executor.async(flow).get();

permits.reset(10);
(void)permits.value();
(void)permits.max_value();
```

`reset` 只能在没有等待任务、没有未归还配额且没有并发获取 / 释放时调用，它不是运行中动态扩容接口。申请量超过容量，或让释放任务依赖于无法获得配额的任务，仍然可能导致逻辑死锁。

---

## 可视化

需要 `<fstream>`。`Flow::dump` 输出 D2 文本；异步句柄也提供 `dump`。导出应在图结构稳定时进行。

```cpp
tfl::Flow flow;
auto [first, second] = flow.emplace([] {}, [] {});
first.precede(second);

flow.name("MyPipeline");
std::ofstream file("pipeline.d2");
flow.dump(file);
std::string d2 = flow.dump();
(void)d2;
```

可使用 [D2 Playground](https://play.d2lang.com) 渲染输出；节点与边的样式区分任务类型和依赖关系。

![TaskflowLite DAG 可视化示例](documentation/img/d2.svg)

---

## 编译与集成

### 系统要求

- **语言标准**：C++20；不需要 C++23。
- **CMake**：3.21 或更高。
- **运行依赖**：C++ 标准库与线程支持；非 Windows、非 macOS 的当前 CMake 配置还链接 `libatomic`。
- **标准库功能**：需要 Concepts、Ranges、`std::format`、原子等待等；当前头文件也包含 `<stop_token>`。本页并行打印示例使用 `<syncstream>`。
- 库为 header-only；仅构建库 / 示例无需下载 Catch2 或 Taskflow。测试与基准依赖按开关启用。

| 工具链 | 使用说明 |
|--------|----------|
| GCC + libstdc++ | 使用 GCC / libstdc++ 13 或更高；`std::format` 从 libstdc++ 13.1 起提供 |
| Clang | 同时检查所选标准库；可以搭配满足上述要求的 libstdc++ 或 libc++ |
| MSVC | 使用支持上述 C++20 功能的 Visual Studio 2022 工具集；当前 MSVC 19.44 的 SubFlow 回归见下文 |
| macOS | 仓库 CI 使用 Homebrew LLVM；编译器、libc++ 头文件和运行库需要配套 |

以上是工具链选择依据，不是所有版本均已通过完整测试的承诺。标准库支持参见 [GCC 状态表](https://gcc.gnu.org/onlinedocs/libstdc++/manual/status.html)和 [libc++ C++20 状态表](https://libcxx.llvm.org/Status/Cxx20.html)。不要仅根据 `-std=c++20` 或编译器名称判断标准库是否完整。

### CMake 构建与测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4

cmake -S . -B build/test -DCMAKE_BUILD_TYPE=Release -DTFL_BUILD_TESTS=ON
cmake --build build/test --config Release --parallel 4
ctest --test-dir build/test -C Release --output-on-failure
```

Ninja / Unix Makefiles 使用 `CMAKE_BUILD_TYPE`；Visual Studio 等多配置生成器使用构建和 CTest 命令中的 `--config` / `-C`，两者要一致。默认顶层构建开启示例、关闭测试与基准；作为子项目时示例默认关闭。

`TFL_BUILD_TESTS=ON` 时，默认注册 30 个示例和一个单体测试；关闭示例则只注册单体测试。另开基准后增加两项 smoke。按文件测试默认不构建、不注册，避免重复运行整套用例。

| CMake 选项 | 默认值 | 作用 |
|------------|--------|------|
| `TFL_BUILD_EXAMPLES` | 顶层 ON / 子项目 OFF | 构建示例 |
| `TFL_BUILD_TESTS` | OFF | 构建 Catch2 测试并启用 CTest |
| `TFL_BUILD_BENCHMARKS` | OFF | 构建两套基准，获取 Taskflow |
| `TFL_SANITIZER` | OFF | OFF / ASAN / TSAN |
| `TFL_EXAMPLES_RUN_TARGETS` | OFF | 生成 `run_example_<name>` 目标 |
| `TFL_EXAMPLES_EXCLUDE_FROM_ALL` | OFF | 示例不参与默认构建，也不自动注册 CTest |
| `TFL_TEST_PER_FILE_DEFAULT` | OFF | 默认构建并注册按文件测试 |
| `TFL_TEST_RUN_TARGETS` | OFF | 生成按文件构建运行目标 |
| `TFL_CATCH2_LOCAL_PATH` | 空 | Catch2 amalgamated 两个文件所在目录 |
| `TFL_CATCH2_REF` | devel | 自动下载 Catch2 使用的 ref |
| `TASKFLOW_LOCAL_PATH` | 空 | 包含 `taskflow/taskflow.hpp` 的目录 |

### Sanitizer

```bash
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DTFL_BUILD_TESTS=ON -DTFL_SANITIZER=ASAN
cmake --build build/asan --config Debug --parallel 4
ctest --test-dir build/asan -C Debug --output-on-failure

cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DTFL_BUILD_TESTS=ON -DTFL_SANITIZER=TSAN
cmake --build build/tsan --config Debug --parallel 4
ctest --test-dir build/tsan -C Debug --output-on-failure
```

ASAN 与 TSAN 使用独立构建目录。当前 GCC / Clang 配置同时附加 UBSan；MSVC 支持 ASan，不支持此项目的 TSAN 配置。Windows 运行 ASan 程序时，确保匹配工具集的 `clang_rt.asan*.dll` 可通过 `PATH` 找到，或放在每个可执行文件目录中；测试与示例的目录不同。

这些开关主要应用于仓库内部目标；下游项目需要自行配置 sanitizer。不要把跳过失败用例或关闭检测后的结果当作 sanitizer 通过。

### macOS

```bash
brew install llvm
LLVM_PREFIX="$(brew --prefix llvm)"
cmake -S . -B build/macos -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="$LLVM_PREFIX/bin/clang++" \
  -DCMAKE_EXE_LINKER_FLAGS="-L$LLVM_PREFIX/lib/c++ -Wl,-rpath,$LLVM_PREFIX/lib/c++"
cmake --build build/macos --parallel 4
```

更换工具链时使用新构建目录。若使用 Apple 自带工具链，先核对该 Xcode / SDK 实际提供的标准库功能；不能由上游 Clang 版本推断 Apple libc++ 的功能。macOS 不要添加 `-latomic`。

### 离线依赖与网络设置

```bash
cmake -S . -B build/offline -DCMAKE_BUILD_TYPE=Release \
  -DTFL_BUILD_TESTS=ON -DTFL_BUILD_BENCHMARKS=ON \
  -DTFL_CATCH2_LOCAL_PATH=/path/to/Catch2/extras \
  -DTASKFLOW_LOCAL_PATH=/path/to/taskflow
```

替换上述路径。Catch2 目录须同时包含 `catch_amalgamated.cpp` 与 `catch_amalgamated.hpp`。未指定有效路径时，依次搜索 `test/catch2/`、`test/`，最后下载到构建目录；默认下载 ref 是 `devel`，可用 `TFL_CATCH2_REF` 固定版本。基准使用本地 Taskflow 或自动克隆；可复现实验应固定本地依赖提交。

`TFL_GITHUB_MIRROR` 默认 `direct`，可选值来自根 CMake 配置；`TFL_GITHUB_PREFIX` 提供自定义前缀，`TFL_GIT_PROXY` 用于 Git 代理，`TFL_PARTIAL_CLONE` 默认 ON，`TFL_UPDATE_DEPS` 默认 OFF。镜像是外部服务，其可用性由服务方决定。

### 作为 CMake 依赖

下面三个集成方式任选其一。将 `main.cpp` 换成应用自己的源文件；子目录方式的路径指向 TaskflowLite 仓库根目录。

```cmake
cmake_minimum_required(VERSION 3.21)
project(my_app LANGUAGES CXX)

set(TFL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TFL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TFL_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
add_subdirectory(path/to/taskflowlite)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE TaskflowLite::taskflowlite)
```

也可以使用 FetchContent。示例中的 `main` 跟随开发分支；正式项目应改成已验证的完整提交哈希或实际存在的发布标签，不要把源码版本宏当作已发布标签。

```cmake
cmake_minimum_required(VERSION 3.21)
project(my_app LANGUAGES CXX)

include(FetchContent)
set(TFL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TFL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TFL_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(taskflowlite
    GIT_REPOSITORY https://github.com/wicyn/taskflowlite.git
    GIT_TAG main
)
FetchContent_MakeAvailable(taskflowlite)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE TaskflowLite::taskflowlite)
```

### 安装与 find_package

```bash
cmake -S . -B build/install -DCMAKE_BUILD_TYPE=Release \
  -DTFL_BUILD_EXAMPLES=OFF -DTFL_BUILD_TESTS=OFF -DTFL_BUILD_BENCHMARKS=OFF
cmake --install build/install --prefix /path/to/tfl-install
```

```cmake
cmake_minimum_required(VERSION 3.21)
project(my_app LANGUAGES CXX)

find_package(TaskflowLite CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE TaskflowLite::taskflowlite)
```

配置使用方时添加 `-DCMAKE_PREFIX_PATH=/path/to/tfl-install`。导出的目标会传递包含路径、C++20 要求和线程 / 平台链接依赖；不需要另外构建一个 TaskflowLite 静态库。

### 直接包含头文件

把最简示例保存为仓库根目录的 `main.cpp`，Linux / GCC 可使用：

```bash
g++ -std=c++20 -O2 -pthread -I. main.cpp -o my_app -latomic
```

头文件需要的链接依赖仍需提供。x86-64 自行启用需要 128 位 CAS 的配置时，可能还需要 `-mcx16`；仓库内部目标在对应平台上设置该选项。优先使用 CMake 导出目标以减少平台差异。

---

## 项目结构

```text
taskflowlite/
├── taskflowlite/
│   ├── taskflowlite.hpp                 # 统一入口与版本
│   └── core/
│       ├── executor.hpp                # 调度器
│       ├── flow.hpp / flow_builder.hpp # 图所有权与构建
│       ├── task.hpp                    # Task / TaskView
│       ├── async_task.hpp              # 延迟启动任务
│       ├── async_future.hpp            # 共享结果句柄
│       ├── runtime.hpp / subflow.hpp   # 动态调度与子图
│       ├── task_group.hpp              # 作用域任务组
│       ├── scoped_exception_anchor.hpp # 局部异常锚点
│       ├── branch.hpp / jump.hpp       # 分支与跳转
│       ├── semaphore.hpp / observer.hpp
│       ├── worker.hpp / context.hpp    # Worker 与执行上下文
│       ├── work.hpp / work_factory.hpp / work_invokers.hpp
│       └── object_pool.hpp / bounded_queue.hpp / unbounded_queue.hpp
├── examples/                           # 30 个独立示例
├── test/                               # 31 个 test_*.cpp，Catch2 v3
├── benchmarks/                         # 两套基准与共享校验
├── documentation/                      # 补充文档与图片
├── cmake/                              # 安装包配置
├── .github/workflows/                  # 平台测试、CodeQL、CI Extras
├── CMakeLists.txt
├── README.md / README.en.md
└── LICENSE
```

---

## 性能数据

### 历史测量（保留作参考）

以下保留旧 README 已发布的 25 项测量，原记录描述为相同硬件、线程数、拓扑和迭代次数下的空任务比较。**这些数值不是当前 2.2.0 接口迁移后重新测得的结果**；原表没有固定双方源码提交，不能据此承诺当前版本在所有负载下的性能。

**原记录环境：** Intel Core i7-9750H @ 2.60GHz（6C/12T）、Windows 11、MSVC 2022 /O2。

| # | 测试场景 | 参数 | TaskflowLite | Taskflow | 加速比 |
|--:|---------|------|--------:|------------:|------:|
| 01 | 32 并行 | 8 线 · 500k | 893 ms | 1321 ms | **1.48×** |
| 02 | 32 串行 | 1 线 · 1M | 483 ms | 1223 ms | **2.53×** |
| 03 | 菱形 DAG | 2 线 · 1M | 219 ms | 357 ms | **1.63×** |
| 04a | 4×2 全连接 | 2 线 · 1M | 429 ms | 611 ms | **1.42×** |
| 04b | 6×4 全连接 | 4 线 · 500k | 1435 ms | 1779 ms | **1.24×** |
| 04c | 8×8 全连接 | 8 线 · 100k | 933 ms | 1258 ms | **1.35×** |
| 04d | 8×16 全连接 | 8 线 · 50k | 1080 ms | 1496 ms | **1.39×** |
| 04e | 8×32 全连接 | 8 线 · 20k | 1115 ms | 1627 ms | **1.46×** |
| 04f | 6×100 全连接 | 8 线 · 2k | 548 ms | 715 ms | **1.30×** |
| 05 | 二叉归约树 | 8 线 · 500k | 1349 ms | 2980 ms | **2.21×** |
| 06 | 1→256→1 扇出 | 8 线 · 100k | 3181 ms | 4096 ms | **1.29×** |
| 07 | 16 条管线 | 8 线 · 200k | 389 ms | 2452 ms | **6.30×** |
| 08 | 16×16 网格 | 8 线 · 100k | 653 ms | 2722 ms | **4.17×** |
| 09 | 稀疏 DAG | 8 线 · 500k | 1815 ms | 3799 ms | **2.09×** |
| 10 | Jump 循环 | 1 线 · 1M | 25 ms | 50 ms | **2.00×** |
| 11 | MultiJump 循环 | 4 线 · 200k | 49 ms | 75 ms | **1.53×** |
| 12 | Subflow 单次 | 4 线 · 200k | 130 ms | 183 ms | **1.41×** |
| 13 | Subflow 循环 | 2 线 · 500k | 94 ms | 159 ms | **1.69×** |
| 14 | 空任务 | 1 线 · 10M | 406 ms | 633 ms | **1.56×** |
| 15 | 并行 for | 8 线 · 1024×10k | 580 ms | 1159 ms | **2.00×** |
| 16 | 归约树 | 8 线 · 127×50k | 346 ms | 693 ms | **2.00×** |
| 17 | 扫描链 | 1 线 · 128×100k | 170 ms | 488 ms | **2.87×** |
| 18 | 三角波前 | 8 线 · 210×10k | 68 ms | 236 ms | **3.47×** |
| 19 | 异构负载 | 8 线 · 18×100k | 746 ms | 873 ms | **1.17×** |
| 20 | 内存压力 | 8 线 · 2000×500 | 786 ms | 1115 ms | **1.42×** |
| | **几何平均** | | | | **≈ 1.85×** |

当前基准默认开启原子计数校验，计时包含校验操作，不能把默认输出称为纯调度开销。两套程序都有 `--smoke`、`--no-verify` 和 `--help`；公平比较时应使用相同编译器、优化参数、线程数、依赖版本和参数。先通过带校验的 smoke，再进行关闭校验的完整测量。详见 [benchmarks/README.md](benchmarks/README.md)。

---

## 示例与测试

### 示例索引

| 文件 | 内容 |
|------|------|
| [01_basic_dag.cpp](examples/01_basic_dag.cpp) | 基础 DAG |
| [02_parallel.cpp](examples/02_parallel.cpp) | 并行任务 |
| [03_loop.cpp](examples/03_loop.cpp) | 重复执行 |
| [04_runtime.cpp](examples/04_runtime.cpp) | 运行时派发 |
| [05_branch.cpp](examples/05_branch.cpp) | 条件分支 |
| [06_jump.cpp](examples/06_jump.cpp) | 跳转与重试 |
| [07_semaphore.cpp](examples/07_semaphore.cpp) | 信号量限流 |
| [08_subflow.cpp](examples/08_subflow.cpp) | 模块子图 |
| [09_pipeline.cpp](examples/09_pipeline.cpp) | Map-Reduce 管线 |
| [10_dump.cpp](examples/10_dump.cpp) | D2 导出 |
| [11_flow_emplace.cpp](examples/11_flow_emplace.cpp) | 节点创建与参数绑定 |
| [12_loop_workflow.cpp](examples/12_loop_workflow.cpp) | 循环工作流 |
| [13_parallel_reduce.cpp](examples/13_parallel_reduce.cpp) | 并行归约 |
| [14_async_task_chain.cpp](examples/14_async_task_chain.cpp) | 延迟任务链 |
| [15_observer.cpp](examples/15_observer.cpp) | 任务观察者计时 |
| [16_error_handling.cpp](examples/16_error_handling.cpp) | 异常处理 |
| [17_cancellation.cpp](examples/17_cancellation.cpp) | 协作取消 |
| [18_pipeline_producer_consumer.cpp](examples/18_pipeline_producer_consumer.cpp) | 生产者 / 消费者管线 |
| [19_dependent_async.cpp](examples/19_dependent_async.cpp) | 异步依赖 |
| [20_parallel_for_index.cpp](examples/20_parallel_for_index.cpp) | 索引分区并行 |
| [21_observer_tracing.cpp](examples/21_observer_tracing.cpp) | Observer tracing |
| [22_state_machine.cpp](examples/22_state_machine.cpp) | 状态机 |
| [23_parallel_reduce.cpp](examples/23_parallel_reduce.cpp) | 静态分区归约 |
| [24_recursive_runtime.cpp](examples/24_recursive_runtime.cpp) | 递归 Runtime |
| [25_retry_backoff.cpp](examples/25_retry_backoff.cpp) | 重试退避 |
| [26_task_group.cpp](examples/26_task_group.cpp) | 作用域任务组 |
| [27_dynamic_subflow.cpp](examples/27_dynamic_subflow.cpp) | 动态构建子图 |
| [28_task_editing.cpp](examples/28_task_editing.cpp) | 任务编辑 |
| [29_async_future_results.cpp](examples/29_async_future_results.cpp) | 值 / 引用 / void 结果 |
| [30_worker_handler.cpp](examples/30_worker_handler.cpp) | Worker 生命周期 |

```bash
cmake --build build --config Release --target tfl_ex_01_basic_dag
cmake --build build --config Release --target run_all_examples
```

`run_all_examples` 会构建并逐个运行全部示例。开启测试后，也可用 `ctest --test-dir build/test -C Release -L example --output-on-failure` 运行已构建的示例。

### 单元测试

```bash
ctest --test-dir build/test -C Release -L unit --output-on-failure
cmake --build build/test --config Release --target tfl_test_task
```

31 个测试文件覆盖图构建、任务与 Future、三种提交上下文、分支 / 跳转、子图、取消 / 异常、观察者、队列、分配器和压力场景。可用 Catch2 标签选择用例，例如在对应可执行文件路径后加 `"[subflow]"`。按文件目标 `tfl_test_task` 只编译对应文件；直接运行它，或用 `TFL_TEST_PER_FILE_DEFAULT=ON` 将按文件测试加入 CTest。

覆盖约定和已知核心回归见 [test/README.md](test/README.md)。完整测试应不带排除回归的过滤器。

### 可执行文件位置

| 程序 | 单配置生成器 | Visual Studio Release |
|------|--------------|-----------------------|
| 示例 | `<build>/bin/examples/01_basic_dag` | `<build>/bin/examples/Release/01_basic_dag.exe` |
| 单体测试 | `<build>/bin/TaskflowLiteTest` | `<build>/bin/Release/TaskflowLiteTest.exe` |
| 单文件测试 | `<build>/bin/tfl_test_task` | `<build>/bin/Release/tfl_test_task.exe` |
| 基准 | `<build>/benchmarks/bench_taskflowlite` | `<build>/benchmarks/Release/bench_taskflowlite.exe` |

`<build>` 替换成实际构建目录；Taskflow 对照基准同目录，名称为 `bench_taskflow`。

### 基准运行

```bash
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release -DTFL_BUILD_BENCHMARKS=ON
cmake --build build/bench --config Release --target bench_taskflowlite bench_taskflow --parallel 4

./build/bench/benchmarks/bench_taskflowlite --smoke
./build/bench/benchmarks/bench_taskflow --smoke

./build/bench/benchmarks/bench_taskflowlite --no-verify
./build/bench/benchmarks/bench_taskflow --no-verify
```

以上运行路径适用于单配置生成器；Windows 路径按上表调整。`--smoke` 保留全部场景结构，将重复次数限制为最多 3，适合正确性检查，不用于性能排名。启用测试和基准时，可用 `ctest -L benchmark` 运行两项 smoke。

---

## 迁移与注意事项

| 旧写法 / 误用 | 当前写法 |
|---------------|----------|
| `NonrepeatAsyncTask` | `AsyncTask<R>` 或 CTAD |
| `executor.submit(task)` | `executor.run(task)` |
| `detach(callable)` | `silent_async(callable)` |
| `cowait()` / `cowait_until(...)` | `Runtime::wait()` / `wait_until(...)` |
| `emplace(callable, business_args...)` | 捕获或 `std::bind_front`；图模块的次数 / 谓词重载仍保留 |
| `tfl::pack{callable, business_args...}` | 先绑定 callable；pack 只展开有效的 `emplace` 参数 |
| `Future` / `wait_for` / `share` | `AsyncFuture`、`done()`、直接复制句柄 |
| `ResumeNever` / `ResumeAlways` | Future `get()` 与显式异常作用域 |
| `on_before(TaskView)` | `on_before(WorkerView) noexcept`，`on_after` 同理 |
| 执行中修改 / 清空图 | 等当前执行完成后再编辑 |
| 把 `Semaphore::reset` 当作运行中扩容 | 无等待者、无占用、无并发操作时再重置 |

当前源码与 CI 检查还需注意：

- **MSVC SubFlow 回归**：MSVC 19.44 / Windows x64 下，无捕获 SubFlow callable 曾复现 `Graph::clear()` 访问异常，关联 `TFL_NO_UNIQUE_ADDRESS` 布局；两项 `[core-regression]` 默认启用，不能把排除后的通过当作全套通过。详见[测试说明](test/README.md)。
- **GCC 头文件自包含**：`std::memcpy` 应由 `<cstring>` 声明，`std::condition_variable` 应由 `<condition_variable>` 声明；缺少这些包含时应修正使用它们的文件，不能依赖别的标准库间接包含。
- **并行输出与捕获**：并行日志统一使用独立 `std::osyncstream` 或同一互斥锁，不能混用未同步的并行输出。打印外层局部常量时显式捕获，例如 `[N] { std::osyncstream(std::cout) << N; }`。
- **计时负载溢出**：循环累加应使用足够宽的整数类型；`volatile` 不会防止有符号溢出。C++20 下避免对 volatile 使用已弃用的复合赋值。
- **结果判定**：以对应提交的完整构建 / CTest / sanitizer 日志为准。示例通过一次不能证明不存在竞争，编译警告也不等同于 CodeQL 或 sanitizer 错误。

---

## 许可证

[MIT License](LICENSE)
