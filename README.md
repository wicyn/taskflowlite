# 🚀 TaskflowLite (tfl)
**TaskflowLite** 是一个专为现代 C++23 打造的**高性能、无锁、支持有向无环图 (DAG) 与工作窃取 (Work-Stealing) 的任务调度引擎**。

灵感来源于优秀的 [Taskflow](https://github.com/taskflow/taskflow) 库，TaskflowLite 专注于**极致性能与零开销抽象**。通过深度运用 C++23 的 `Concepts`、无锁环形队列以及内存序屏障，它能帮助开发者以极低的开销轻松应对复杂的并发编排、动态路由控制以及异步任务调度。

---

## 📑 目录

* [✨ 核心特性](https://www.google.com/search?q=%23-%E6%A0%B8%E5%BF%83%E7%89%B9%E6%80%A7)
* [🏗️ 架构概览](https://www.google.com/search?q=%23-%E6%9E%B6%E6%9E%84%E6%A6%82%E8%A7%88)
* [📦 快速开始](https://www.google.com/search?q=%23-%E5%BF%AB%E9%80%9F%E5%BC%80%E5%A7%8B)
* [🧠 核心概念与 API](https://www.google.com/search?q=%23-%E6%A0%B8%E5%BF%83%E6%A6%82%E5%BF%B5%E4%B8%8E-api)
* [Flow & Task (任务图与句柄)](https://www.google.com/search?q=%231-flow--task-%E4%BB%BB%E5%8A%A1%E5%9B%BE%E4%B8%8E%E5%8F%A5%E6%9F%84)
* [Executor & AsyncTask (执行器与异步任务)](https://www.google.com/search?q=%232-executor--asynctask-%E6%89%A7%E8%A1%8C%E5%99%A8%E4%B8%8E%E5%BC%82%E6%AD%A5%E4%BB%BB%E5%8A%A1)
* [Runtime (运行时动态派发)](https://www.google.com/search?q=%233-runtime-%E8%BF%90%E8%A1%8C%E6%97%B6%E5%8A%A8%E6%80%81%E6%B4%BE%E5%8F%91)
* [Branch & Jump (静态路由控制)](https://www.google.com/search?q=%234-branch--jump-%E9%9D%99%E6%80%81%E8%B7%AF%E7%94%B1%E6%8E%A7%E5%88%B6)
* [Semaphore (任务级信号量)](https://www.google.com/search?q=%235-semaphore-%E4%BB%BB%E5%8A%A1%E7%BA%A7%E4%BF%A1%E5%8F%B7%E9%87%8F)


* [🗂️ 任务类型速查表](https://www.google.com/search?q=%23-%E4%BB%BB%E5%8A%A1%E7%B1%BB%E5%9E%8B%E9%80%9F%E6%9F%A5%E8%A1%A8)
* [🎨 D2 图可视化导出](https://www.google.com/search?q=%23-d2-%E5%9B%BE%E5%8F%AF%E8%A7%86%E5%8C%96%E5%AF%BC%E5%87%BA)
* [⚙️ 底层性能设计](https://www.google.com/search?q=%23%EF%B8%8F-%E5%BA%95%E5%B1%82%E6%80%A7%E8%83%BD%E8%AE%BE%E8%AE%A1)
* [🛠️ 编译要求与集成](https://www.google.com/search?q=%23-%E7%BC%96%E8%AF%91%E8%A6%81%E6%B1%82%E4%B8%8E%E9%9B%86%E6%88%90)
* [🚀 性能基准测试](https://www.google.com/search?q=%23-%E6%80%A7%E8%83%BD%E5%9F%BA%E5%87%86%E6%B5%8B%E8%AF%95)

---

## ✨ 核心特性

* **⚡ 极致的 Work-Stealing 调度**：每个 Worker 维护本地有界无锁队列，空闲时通过 `Xoshiro256**` 极速随机数从共享无界队列或邻居窃取任务，最大化 CPU 利用率。
* **🕸️ 强大的 DAG 拓扑编排**：直观的 `precede` / `succeed` 链式 API，支持任意复杂的依赖关系与图中图（Subflow）嵌套。
* **🔀 运行期动态流控**：内置 `Branch` (条件分支) 与 `Jump` (强制跳转/循环) 机制，完美复用底层依赖计数器，零额外开销。
* **⏳ 协作式等待 (Cooperative Wait)**：在 `Runtime` 中等待 Future 或子图时，线程绝不阻塞休眠，而是主动窃取其他任务执行，彻底告别系统级死锁。
* **🚦 任务级信号量**：`Semaphore` 精确限制任务并发数，获取失败时将任务“停车”挂起，不占用底层 Worker 线程。
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

---

## 📦 快速开始

### Hello World & DAG 基础

```cpp
#include "taskflowlite/taskflowlite.hpp"
#include <iostream>

int main() {
    tfl::ResumeNever handler;            // 遇到未捕获异常时直接终止
    tfl::Executor executor(handler, 4);  // 启动 4 个工作线程
    tfl::Flow flow;

    auto [A, B, C, D] = flow.emplace(
        [] { std::cout << "Task A (Init)\n"; },
        [] { std::cout << "Task B (Process 1)\n"; },
        [] { std::cout << "Task C (Process 2)\n"; },
        [] { std::cout << "Task D (Merge)\n"; }
    );

    // 编排 DAG: A 执行完后 B 和 C 并行，B 和 C 都执行完后执行 D
    A.precede(B, C);
    D.succeed(B, C);

    // 提交任务图，启动并阻塞等待完成
    executor.submit(flow).start().wait();
    
    return 0;
}

```

---

## 🧠 核心概念与 API

### 1. Flow & Task (任务图与句柄)

`Flow` 是装载任务的容器。使用 `emplace` 压入任务，返回一个轻量级的 `Task` 句柄用于连线。

```cpp
tfl::Flow flow;
auto t1 = flow.emplace([] { /* ... */ }).name("Task1");
auto t2 = flow.emplace([] { /* ... */ }).name("Task2");

t1.precede(t2); // t1 先跑，t2 后跑

```

### 2. Executor & AsyncTask (执行器与异步任务)

`submit` 遵循“创建与执行分离”原则，返回 `AsyncTask`。

```cpp
// 提交一个 Flow 循环执行 100 次
tfl::AsyncTask task = executor.submit(flow, 100);

// 你可以自由传递句柄，并在需要的时候点火启动
task.start();   
task.wait();    // 同步等待。如果图内发生异常，会在这里重新抛出！

```

### 3. Runtime (运行时动态派发)

当任务签名包含 `tfl::Runtime&` 时，赋予该节点运行时特权：**动态派发**与**协作等待**。

```cpp
flow.emplace([](tfl::Runtime& rt) {
    // 动态派发一个带返回值的任务
    auto fut = rt.async([] { return 42; });

    // 协作式等待：等待结果期间，当前线程会去窃取别的任务干，绝不阻塞！
    rt.wait_until([&]() noexcept {
        return fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    });

    std::cout << "Dynamic Result: " << fut.get() << "\n";
});

```

### 4. Branch & Jump (静态路由控制)

控制流不仅可以按顺序跑，还能动态选择分支或循环重试。

```cpp
// Branch: 走哪条路？
auto validate = flow.emplace([](tfl::Branch br) {
    bool ok = do_validate();
    br.allow(ok ? 0 : 1); // 激活下标为 0 (success) 或 1 (failure) 的后继
});

auto success = flow.emplace([] { puts("OK"); });
auto failure = flow.emplace([] { puts("Fail"); });

validate.precede(success, failure); // 按照 0, 1 顺序绑定

// Jump: 失败重试循环
auto retry = flow.emplace([](tfl::Jump jmp) {
    jmp.to(0); // 强行拉回 normalize 节点，重置其依赖计数
});
retry.precede(normalize); 

```

### 5. Semaphore (任务级信号量)

限制某类任务（如 GPU 计算、I/O）的最大并发数。

```cpp
tfl::Semaphore db_connection_limit(2); // 最多 2 个并发

for (int i = 0; i < 10; ++i) {
    auto t = flow.emplace([i] { /* 操作数据库 */ });
    t.acquire(db_connection_limit).release(db_connection_limit);
}
// 若名额耗尽，任务会在队列外“停车”，不阻塞任何 Worker 线程。

```

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

## ⚙️ 底层性能设计

TaskflowLite 之所以快，是因为在底层死抠了每一个时钟周期：

1. **缓存行隔离 (Cache-Line Isolation)**：
在 `Executor` 和 `BoundedQueue` 中，严格使用 `alignas(std::hardware_destructive_interference_size)` 将多线程竞争的热点原子变量（如 `top` 和 `bottom`，`m_num_topologies`）物理隔开，彻底消灭**伪共享 (False Sharing)**。
2. **极速内存分配 (Edge Storage Optimization)**：
图节点 `Work` 内部将后继指针(Successors)和前驱指针(Predecessors)打包在一块连续的 `std::vector<Work*>` 中，通过游标偏移访问，省去了一次堆分配。
3. **零开销的异常处理 (Noexcept Elision)**：
如果你的任务闭包标记为 `noexcept`，编译器在实例化 `invoke()` 时将直接抹除包裹它的 `try-catch` 块，消除展开表开销。
4. **无除法哈希桶 (Divisionless Distribution)**：
随机窃取模块和 `UnboundedQueueBucket` 路由模块采用 Lemire 的无除法界限映射算法与位运算，比标准库的分配器快 20% 以上。

---

## 🛠️ 编译要求与集成

**系统要求：**

* **C++ Standard**: C++23 或更高。
* **Compiler**: GCC 12+, Clang 15+, MSVC 2022+ (需完全支持 Concepts, source_location)。
* **Dependencies**: 无。（内置轻量级的 `ankerl::unordered_dense` 头文件）。

**集成方式（Header-Only）：**
由于是纯头文件库，直接将 `taskflowlite/` 目录拖入你的项目中即可：

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

测试极高密度的任务连线（100 层，每层 100 个并行任务，互相全连接，循环执行 100 次）：

```cpp
#include "taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <atomic>
#include <chrono>

int main() {
    constexpr std::size_t LAYERS    = 100;
    constexpr std::size_t PER_LAYER = 100;
    constexpr std::size_t THREADS   = 8;
    constexpr std::size_t ITERS     = 100;

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, THREADS);
    tfl::Flow flow;
    std::atomic<int> counter{0};

    // 构建一个巨大的网状 DAG
    std::vector<std::vector<tfl::Task>> layers(LAYERS);
    for (std::size_t layer = 0; layer < LAYERS; ++layer) {
        layers[layer].reserve(PER_LAYER);
        for (std::size_t i = 0; i < PER_LAYER; ++i) {
            layers[layer].push_back(
                flow.emplace([&]{ counter.fetch_add(1, std::memory_order_relaxed); })
            );
        }
        if (layer > 0) {
            for (auto& prev : layers[layer - 1])
                for (auto& curr : layers[layer])
                    prev.precede(curr); // 全连接
        }
    }

    executor.submit(flow, 1).start().wait();   // 预热 (Warm-up)

    counter.store(0);
    auto t0 = std::chrono::high_resolution_clock::now();
    executor.submit(flow, ITERS).start().wait(); // 计时执行
    auto t1 = std::chrono::high_resolution_clock::now();

    auto ns    = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    auto total = LAYERS * PER_LAYER * ITERS;
    printf("tasks: %d / %zu | total: %.2f ms | per-task: %ld ns\n",
           counter.load(), total, ns / 1e6, ns / (long)total);
}

```

---

## 🧪 单元测试

TaskflowLite 使用 CMake 进行构建和测试。

### 构建测试

```bash
# 配置项目（开启测试）
cmake -S . -B build -DTASKFLOWLITE_BUILD_TESTS=ON

# 编译
cmake --build build -j4

# 运行测试
cd build && ctest --build-config Release --output-on-failure
```

### 测试覆盖

测试文件位于 `test/test_taskflowlite.cpp`，包含以下测试用例：

- DAG 构建与依赖关系
- 并行任务执行
- 条件分支 (Branch)
- 强制跳转 (Jump)
- 信号量 (Semaphore)
- 子图嵌套 (Subflow)
- 运行时动态派发
- 异常处理

---

## 📚 示例代码

项目提供了 10+ 个完整示例，覆盖所有核心功能。

### 运行示例

```bash
# 配置项目（开启示例）
cmake -S . -B build -DTASKFLOWLITE_BUILD_EXAMPLES=ON

# 编译所有示例
cmake --build build -j4

# 运行单个示例
./build/examples/01_basic_dag
```

### 示例列表

| 示例 | 文件 | 说明 |
|------|------|------|
| 基础 DAG | `01_basic_dag.cpp` | 最简单的有向无环图 |
| 并行执行 | `02_parallel.cpp` | 并行任务调度 |
| 循环执行 | `03_loop.cpp` | 使用 Jump 实现循环 |
| 运行时 | `04_runtime.cpp` | 动态任务派发 |
| 条件分支 | `05_branch.cpp` | 条件选择执行路径 |
| 强制跳转 | `06_jump.cpp` | 状态机回跳与重试 |
| 信号量 | `07_semaphore.cpp` | 限制任务并发数 |
| 子图 | `08_subflow.cpp` | 图中图嵌套 |
| 管道 | `09_pipeline.cpp` | 流水线任务编排 |
| 导出 | `10_dump.cpp` | D2 图形导出 |

---

## 📄 许可证

本项目采用 [MIT License](https://www.google.com/search?q=LICENSE) 开源。

*TaskflowLite — 为追求极致性能的 C++ 并行程序而生。*