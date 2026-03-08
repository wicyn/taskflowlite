# 🚀 TaskflowLite (tfl)

**为现代 C++23 打造的极速、无锁、零开销的任务调度与 DAG 编排引擎**

[![Standard](https://img.shields.io/badge/C%2B%2B-23-blue.svg?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/23)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Header Only](https://img.shields.io/badge/Header--Only-Yes-success)](#)

**[中文文档](README.zh.md) · [English](README.md)**

---

**TaskflowLite (tfl)** 是一个受 [Taskflow](https://github.com/taskflow/taskflow) 启发，但在底层全面拥抱 **C++23 现代化范式** 的轻量级并发调度库。它专注于**极致性能与类型安全**，通过无锁环形队列 (Lock-free Ring Buffer)、工作窃取 (Work-Stealing) 算法以及强悍的编译期 `Concepts` 约束，帮助开发者以极低的抽象开销，轻松应对复杂的并发拓扑、动态路由与异步调度。

## 📑 目录

* [✨ 为什么选择 TaskflowLite？](#-为什么选择-taskflowlite)
* [✨ 核心特性](#-核心特性)
* [🏗️ 架构概览](#-架构概览)
* [📦 快速开始](#-快速开始)
* [🧠 核心特性与 API](#-核心特性与-api)
* [🛡️ 现代 C++23 黑魔法设计](#-现代-c23-黑魔法设计)
* [🗂️ 任务类型速查表](#-任务类型速查表)
* [🎨 D2 图可视化导出](#-d2-图可视化导出)
* [⚙️ 极致的性能优化](#-极致的性能优化)
* [🛠️ 编译要求与集成](#-编译要求与集成)
* [🚀 性能基准测试](#-性能基准测试)
* [📄 许可证](#-许可证)

---

## ✨ 为什么选择 TaskflowLite？

* 🪶 **Header-Only & 零依赖**：只需将代码拖入项目即可使用（内部仅集成极速的 `ankerl::unordered_dense`）。
* ⚡ **极致的工作窃取 (Work-Stealing)**：基于 `Xoshiro256**` 伪随机数发生器与无锁有界队列，实现千万级任务的亚毫秒级调度。
* 🛡️ **安全的泛型编程**：首创 **Arity-Guard (参数数量守卫)**，完美支持 `[](auto x)` 泛型 Lambda，彻底消灭模板推导引发的 Hard Error。
* 🔀 **强大的控制流**：原生支持条件分支 (`Branch`)、强制跳转/重试 (`Jump`)，复用底层依赖计数器，真正做到**零额外分配开销**。
* 🛑 **防死锁协作等待**：等待 Future 或子图时，线程绝不系统级阻塞，而是主动窃取旁路任务，榨干 CPU 最后一丝算力。

---

## ✨ 核心特性

* **⚡ 极致的 Work-Stealing 调度**：每个 Worker 维护本地有界无锁队列，空闲时通过 `Xoshiro256**` 极速随机数从共享无界队列或邻居窃取任务，最大化 CPU 利用率。
* **🕸️ 强大的 DAG 拓扑编排**：直观的 `precede` / `succeed` 链式 API，支持任意复杂的依赖关系与图中图（Subflow）嵌套。
* **🔀 运行期动态流控**：内置 `Branch` (条件分支) 与 `Jump` (强制跳转/循环) 机制，完美复用底层依赖计数器，零额外开销。
* **⏳ 协作式等待 (Cooperative Wait)**：在 `Runtime` 中等待 Future 或子图时，线程绝不阻塞休眠，而是主动窃取其他任务执行，彻底告别系统级死锁。
* **🚦 任务级信号量**：`Semaphore` 精确限制任务并发数，获取失败时将任务"停车"挂起，不占用底层 Worker 线程。
* **🛡️ 现代 C++23 契约驱动**：深度使用 C++23 `Concepts` 进行编译期严格类型安检；利用 `noexcept` 智能擦除冗余的 `try-catch` 汇编块。
* **📊 D2 可视化一键导出**：原生支持将运行期的复杂任务图直接 `dump()` 为 D2 声明式图形代码，轻松生成架构图。

---

## 🏗️ 架构概览

```text
┌─────────────────────────────────────────────────────────┐
│                        用户代码                         │
│  Flow  →  emplace(task)  →  Task  →  precede/succeed    │
│  Executor::submit(flow, N)  →  AsyncTask::start().wait()│
└────────────────────┬────────────────────────────────────┘
                     │ submit
┌────────────────────▼────────────────────────────────────┐
│                    Executor                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐               │
│  │ Worker 0 │  │ Worker 1 │  │ Worker N │               │
│  │ BoundedQ │  │ BoundedQ │  │ BoundedQ │  ← 本地队列   │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘               │
│       │   work-steal│             │                     │
│  ┌────▼─────────────▼─────────────▼──────┐              │
│  │          UnboundedQueueBucket         │ ← 共享队列   │
│  └───────────────────────────────────────┘              │
│  Notifier (基于原子 wait 的防丢失唤醒中枢)              │
└─────────────────────────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│                    Work (内部物理节点)                  │
│  m_edges: [successors... | predecessors...]             │
│  m_join_counter, m_topology, m_observers?, m_semaphores?│
└─────────────────────────────────────────────────────────┘
```

## 📦 快速开始

一个简单却强大的有向无环图 (DAG) 示例：

```cpp
#include "taskflowlite/taskflowlite.hpp"
#include <iostream>

int main() {
    tfl::ResumeNever handler;            // 异常策略：遇到未捕获异常立刻终止
    tfl::Executor executor(handler, 4);  // 启动 4 个物理工作线程
    tfl::Flow flow;

    // 1. 创建任务 (完美支持 C++17 结构化绑定)
    auto [A, B, C, D] = flow.emplace(
        [] { std::cout << "Task A (Init)\n"; },
        [] { std::cout << "Task B (Process 1)\n"; },
        [] { std::cout << "Task C (Process 2)\n"; },
        [] { std::cout << "Task D (Merge)\n"; }
    );

    // 2. 编排拓扑: A 执行完后 B 和 C 并行，最后执行 D
    A.precede(B, C);
    D.succeed(B, C);

    // 3. 提交并同步等待
    executor.submit(flow).start().wait();

    return 0;
}
```

---

## 🧠 核心特性与 API

### 1. 批量插入与 DAG 编排

TaskflowLite 支持无缝解包 `std::tuple` 和完美转发，让你可以极度优雅地传递参数和 `std::ref`，告别冗长的 Lambda 捕获。

```cpp
tfl::Flow flow;
int counter = 0;

// 使用 Tuple 批量插入带参数的任务
auto [t1, t2] = flow.emplace(
    std::tuple{[](int a) { std::cout << "Val: " << a << "\n"; }, 42},
    std::tuple{[](int& c) { c = 100; }, std::ref(counter)} // 零拷贝引用传递
);
t1.precede(t2);
```

### 2. 运行时动态挂载 (Runtime)

当任务签名包含 `tfl::Runtime&` 时，任务将在执行期获得操控调度器的特权。

```cpp
flow.emplace([](tfl::Runtime& rt) {
    // 动态派发子任务并获取 Future
    auto fut = rt.async([](int x) { return x * 2; }, 21);

    // 协作式等待：线程不会挂起，而是去窃取执行其他队列的任务！
    rt.wait_until([&] {
        return fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    });

    std::cout << "Result: " << fut.get() << "\n";
});
```

### 3. 静态路由与状态机 (Branch & Jump)

复用 DAG 引擎实现 `if-else` 与 `while` 循环逻辑。

```cpp
// ==========================================
// Branch: 动态决定走哪条分支
// ==========================================
auto start = flow.emplace([] { puts("Start"); }); // 明确的无依赖起点

auto check = flow.emplace([](tfl::Branch& br) {
    br.allow(1); // 激活下标为 1 (failure) 的后继节点，跳过 0
});
auto success = flow.emplace([] { puts("OK"); });
auto failure = flow.emplace([] { puts("Fail"); });

start.precede(check);            // 连线起点
check.precede(success, failure); // 按照 0, 1 顺序绑定后继

// ==========================================
// Jump: 失败重试循环
// ==========================================
auto init = flow.emplace([] { puts("Init"); });   // 明确的无依赖起点

auto process = flow.emplace([]{ /* 业务逻辑 */ });
auto retry = flow.emplace([](tfl::Jump& jmp) {
    if (need_retry()) jmp.to(0); // 触发跳转，拉回 target 0 并重置其依赖
});

init.precede(process);   // 连线起点：系统从这里进入
process.precede(retry);
retry.precede(process);  // 闭环连线：将 process 设为 Jump 的 0 号 target
                         // (注：底层设计中 Jump 连线的初始权重为 0，不会引发静态图死锁)
```

### 4. 任务级并发限流 (Semaphore)

限制特定资源（如 GPU、数据库连接）的并发度，超额任务将被挂起，且**不阻塞 Worker 线程**。

```cpp
tfl::Semaphore db_limit(2); // 全局最多 2 个并发

for (int i = 0; i < 10; ++i) {
    auto t = flow.emplace([i] { /* 操作数据库 */ });
    t.acquire(db_limit).release(db_limit); // 声明配额消耗
}
```

### 5. 图中图嵌套 (Subflow)

支持将一整张 Flow 作为节点嵌套进主图中，甚至支持**基于谓词的动态循环执行**。

```cpp
tfl::Flow subflow;
subflow.emplace([]{ puts("Subflow tick"); });

int loops = 0;
// 将 subflow 挂载到主图，并基于 Lambda 谓词循环执行
flow.emplace(std::move(subflow), [&loops]() mutable noexcept {
    return ++loops >= 5;
});
```

---

## 🛡️ 现代 C++23 黑魔法设计

TaskflowLite 内部包含大量针对现代 C++ 的尖端防御性编程设计：

* **泛型 Lambda 保护 (Arity-Guard)**：传统 TMP 在遇到 `[](auto x)` 配合 `std::invocable` 探测时，极易引发函数体非法实例化的 Hard Error。TFL 底层基于 `requires` 表达式实现了参数数量嗅探，完美支持无约束泛型闭包。
* **引用折叠透明化 (`std::ref` Unwrap)**：框架底层存储闭包时使用 `unwrap_ref_decay_t`，允许用户像使用 `std::thread` 一样，通过 `std::ref` 零拷贝地传递状态，且 Concept 验证能精准识别其真实引用类型。
* **Tag Dispatching 优先级路由**：消灭了因 `std::bind` 类型擦除导致的重载决议二义性，使编译报错精准、清晰。

---

## 🗂️ 任务类型速查表

`emplace` 会利用 C++23 Concepts 自动推导你的闭包签名，无需显式指定类型：

| 函数签名 | 类型推导 | 功能说明 | 图形化标识 |
| --- | --- | --- | --- |
| `[]()` | **Basic** | 普通顺序任务，零抽象开销 | 灰色矩形 |
| `[](tfl::Runtime&)` | **Runtime** | 动态派发新任务、协作式阻塞等待 | 粉色矩形 |
| `[](tfl::Branch)` | **Branch** | 单路条件选择（激活 1 条路径） | 蓝色菱形 |
| `[](tfl::MultiBranch)` | **MultiBranch** | 多路并行分发（激活 N 条路径） | 蓝色六边形 |
| `[](tfl::Jump)` | **Jump** | 强制状态机回跳（支持循环重试） | 红色菱形(虚线) |
| `[](tfl::MultiJump)` | **MultiJump** | 并行散射强制跳转（扇出） | 红色六边形(虚线) |
| 传入 `Flow` 对象 | **Subflow** | 将一整张图作为一个节点嵌套执行 | 绿色分组框 |

---

## 🎨 D2 图可视化导出

极其复杂的嵌套拓扑如何调试？一行代码将其导出为 D2 描述语言，利用前端工具或 [D2 官网](https://play.d2lang.com) 极速渲染。

```cpp
std::ofstream file("pipeline.d2");
flow.name("MyPipeline").dump(file);
```

![D2 Visualization](documentation/img/d2.svg)

导出的逻辑极度清晰：**灰色实线**表示普通推演，**蓝色连线**表示条件分支抉择，**红色虚线**表示破坏拓扑的跳转回边。

---

## ⚙️ 极致的性能优化

TaskflowLite 在底层死抠了每一个时钟周期：

1. **缓存行隔离 (Cache-Line Isolation)**：严格使用 `alignas(std::hardware_destructive_interference_size)` 隔离热点原子变量（如队列的 `top/bottom`），彻底消灭多核**伪共享 (False Sharing)**。
2. **极速边存储 (Edge Storage Optimization)**：节点 `Work` 的后继指针与前驱指针打包在同一块连续的 `std::vector<Work*>` 中，通过游标偏移访问，省去一次堆分配并提高 L1 缓存命中率。
3. **零开销异常擦除 (Noexcept Elision)**：如果你的闭包标记为 `noexcept`，编译器在实例化 `invoke()` 时将直接抹除包裹它的 `try-catch` 汇编块。
4. **无除法映射 (Divisionless Distribution)**：随机窃取模块采用 Lemire 的无除法界限映射算法与位运算，大幅降低 CPU 周期消耗。

---

## 🛠️ 编译要求与集成

**系统要求：**

* **C++ Standard**: C++23 或更高。
* **Compiler**: GCC 12+, Clang 15+, MSVC 2022+ (需完全支持 Concepts 与结构化绑定)。

**集成方式（Header-Only）：**
无需编译动态库，直接将 `taskflowlite/` 目录放入项目的 `include` 路径即可：

```cpp
#include "taskflowlite/taskflowlite.hpp"
```

**CMake 推荐选项：**

```cmake
set(CMAKE_CXX_STANDARD 23)
target_compile_options(your_target PRIVATE -O3 -march=native)
```

---

## 🚀 性能基准测试

测试极高密度的全连接网状 DAG（100 层，每层 100 个任务，互相全连接，执行 100 次）：

```cpp
// 详见 benchmark/benchmark.cpp
// 构建 100x100 的全连接矩阵网络
for (std::size_t layer = 1; layer < 100; ++layer) {
    for (auto& prev : layers[layer - 1])
        for (auto& curr : layers[layer])
            prev.precede(curr); // 密集的依赖连线
}
executor.submit(flow, 100).start().wait();
```

---

## 📄 许可证

本项目采用 [MIT License](LICENSE) 开源。

*TaskflowLite — 为追求极致性能与现代 C++ 审美的开发者而生。*