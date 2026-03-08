# 🚀 TaskflowLite (tfl)

**An ultra-fast, lock-free, zero-overhead task scheduling and DAG orchestration engine for modern C++23**

[![Standard](https://img.shields.io/badge/C%2B%2B-23-blue.svg?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/23)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Header Only](https://img.shields.io/badge/Header--Only-Yes-success)](#)

**[中文文档](README.zh.md) · [English](README.md)**

---

**TaskflowLite (tfl)** is a lightweight concurrent scheduling library inspired by [Taskflow](https://github.com/taskflow/taskflow), rebuilt from the ground up with **C++23 modern paradigms**. It focuses on **peak performance and type safety** — featuring a lock-free ring buffer, work-stealing scheduler, and powerful compile-time `Concepts` constraints — enabling developers to handle complex concurrent topologies, dynamic routing, and async dispatch with near-zero abstraction overhead.

## 📑 Table of Contents

* [✨ Why TaskflowLite?](#-why-taskflowlite)
* [✨ Core Features](#-core-features)
* [🏗️ Architecture Overview](#-architecture-overview)
* [📦 Quick Start](#-quick-start)
* [🧠 Core Features & API](#-core-features--api)
* [🛡️ Modern C++23 Design Techniques](#-modern-c23-design-techniques)
* [🗂️ Task Type Cheat Sheet](#-task-type-cheat-sheet)
* [🎨 D2 Graph Visualization Export](#-d2-graph-visualization-export)
* [⚙️ Performance Optimizations](#-performance-optimizations)
* [🛠️ Build Requirements & Integration](#-build-requirements--integration)
* [🚀 Benchmarks](#-benchmarks)
* [📄 License](#-license)

---

## ✨ Why TaskflowLite?

* 🪶 **Header-Only & Zero Dependencies**: Drop the headers into your project and go. (Internally bundles only the blazing-fast `ankerl::unordered_dense`.)
* ⚡ **Extreme Work-Stealing**: Built on a `Xoshiro256**` PRNG and a lock-free bounded queue, delivering sub-millisecond scheduling for tens of millions of tasks.
* 🛡️ **Safe Generic Programming**: A first-of-its-kind **Arity-Guard** provides perfect support for `[](auto x)` generic lambdas, eliminating Hard Errors from template deduction failures.
* 🔀 **Powerful Control Flow**: Native conditional branching (`Branch`) and forced jumps/retries (`Jump`) reuse the underlying dependency counter — truly **zero extra allocation overhead**.
* 🛑 **Deadlock-Safe Cooperative Waiting**: Waiting on a future or subgraph never system-blocks the thread; it actively steals side-channel tasks and squeezes every last cycle out of the CPU.

---

## ✨ Core Features

* **⚡ Extreme Work-Stealing Scheduling**: Each worker maintains a local bounded lock-free queue. When idle, it uses `Xoshiro256**` random numbers to steal from a shared unbounded queue or from neighbors — maximizing CPU utilization.
* **🕸️ Powerful DAG Topology Orchestration**: Intuitive `precede` / `succeed` chaining API supporting arbitrary complex dependency graphs and nested subflows (graph-in-graph).
* **🔀 Runtime Dynamic Flow Control**: Built-in `Branch` (conditional routing) and `Jump` (forced jumps/loops) mechanisms that fully reuse the underlying dependency counter with zero additional overhead.
* **⏳ Cooperative Waiting**: When waiting on a future or subgraph inside a `Runtime`, the thread never sleeps — it actively steals other tasks, eliminating system-level deadlocks.
* **🚦 Task-Level Semaphore**: `Semaphore` precisely limits task concurrency. Tasks that fail to acquire are "parked" without occupying a worker thread.
* **🛡️ Modern C++23 Contract-Driven Design**: Deep use of C++23 `Concepts` for strict compile-time type checking; `noexcept` elides redundant `try-catch` assembly blocks.
* **📊 One-Click D2 Visualization Export**: Natively dump complex runtime task graphs as D2 declarative diagram code for instant architecture visualization.

---

## 🏗️ Architecture Overview

```text
┌─────────────────────────────────────────────────────────┐
│                       User Code                         │
│  Flow  →  emplace(task)  →  Task  →  precede/succeed    │
│  Executor::submit(flow, N)  →  AsyncTask::start().wait()│
└────────────────────┬────────────────────────────────────┘
                     │ submit
┌────────────────────▼────────────────────────────────────┐
│                    Executor                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐               │
│  │ Worker 0 │  │ Worker 1 │  │ Worker N │               │
│  │ BoundedQ │  │ BoundedQ │  │ BoundedQ │  ← Local Queue│
│  └────┬─────┘  └────┬─────┘  └────┬─────┘               │
│       │   work-steal│             │                     │
│  ┌────▼─────────────▼─────────────▼──────┐              │
│  │          UnboundedQueueBucket         │ ← Shared Q   │
│  └───────────────────────────────────────┘              │
│  Notifier (atomic wait-based, loss-free wakeup hub)     │
└─────────────────────────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│               Work (Internal Physical Node)             │
│  m_edges: [successors... | predecessors...]             │
│  m_join_counter, m_topology, m_observers?, m_semaphores?│
└─────────────────────────────────────────────────────────┘
```

---

## 📦 Quick Start

A simple yet powerful DAG example:

```cpp
#include "taskflowlite/taskflowlite.hpp"
#include <iostream>

int main() {
    tfl::ResumeNever handler;            // Exception policy: terminate on uncaught exception
    tfl::Executor executor(handler, 4);  // Launch 4 worker threads
    tfl::Flow flow;

    // 1. Create tasks (C++17 structured bindings natively supported)
    auto [A, B, C, D] = flow.emplace(
        [] { std::cout << "Task A (Init)\n"; },
        [] { std::cout << "Task B (Process 1)\n"; },
        [] { std::cout << "Task C (Process 2)\n"; },
        [] { std::cout << "Task D (Merge)\n"; }
    );

    // 2. Wire the topology: A runs first, then B and C in parallel, then D
    A.precede(B, C);
    D.succeed(B, C);

    // 3. Submit and wait for completion
    executor.submit(flow).start().wait();

    return 0;
}
```

---

## 🧠 Core Features & API

### 1. Bulk Insertion & DAG Orchestration

TaskflowLite supports seamless `std::tuple` unpacking and perfect forwarding, making it trivial to pass arguments and `std::ref` without verbose lambda captures.

```cpp
tfl::Flow flow;
int counter = 0;

// Bulk-insert tasks with arguments via tuple
auto [t1, t2] = flow.emplace(
    std::tuple{[](int a) { std::cout << "Val: " << a << "\n"; }, 42},
    std::tuple{[](int& c) { c = 100; }, std::ref(counter)} // Zero-copy reference passing
);
t1.precede(t2);
```

### 2. Runtime Dynamic Dispatch

When a task signature includes `tfl::Runtime&`, the task gains the ability to control the scheduler during execution.

```cpp
flow.emplace([](tfl::Runtime& rt) {
    // Dynamically dispatch a subtask and get a future
    auto fut = rt.async([](int x) { return x * 2; }, 21);

    // Cooperative wait: the thread does NOT block — it steals and runs other tasks!
    rt.wait_until([&] {
        return fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    });

    std::cout << "Result: " << fut.get() << "\n";
});
```

### 3. Static Routing & State Machines (Branch & Jump)

Reuse the DAG engine to express `if-else` and `while` loop logic natively.

```cpp
// ==========================================
// Branch: dynamically choose a path
// ==========================================
auto start = flow.emplace([] { puts("Start"); }); // Explicit entry point

auto check = flow.emplace([](tfl::Branch& br) {
    br.allow(1); // Activate successor at index 1 (failure), skip 0
});
auto success = flow.emplace([] { puts("OK"); });
auto failure = flow.emplace([] { puts("Fail"); });

start.precede(check);            // Wire the entry point
check.precede(success, failure); // Bind successors in order: 0, 1

// ==========================================
// Jump: retry loop on failure
// ==========================================
auto init = flow.emplace([] { puts("Init"); }); // Explicit entry point

auto process = flow.emplace([]{ /* business logic */ });
auto retry = flow.emplace([](tfl::Jump& jmp) {
    if (need_retry()) jmp.to(0); // Jump: pull back target 0 and reset its dependency count
});

init.precede(process);   // Entry point: execution begins here
process.precede(retry);
retry.precede(process);  // Close the loop: process becomes Jump's target 0
                         // (Note: Jump edges have initial weight 0, preventing static graph deadlock)
```

### 4. Task-Level Concurrency Throttling (Semaphore)

Precisely limit concurrency for specific resources (e.g., GPU, database connections). Tasks exceeding the limit are suspended without blocking worker threads.

```cpp
tfl::Semaphore db_limit(2); // At most 2 concurrent DB operations globally

for (int i = 0; i < 10; ++i) {
    auto t = flow.emplace([i] { /* access database */ });
    t.acquire(db_limit).release(db_limit); // Declare resource consumption
}
```

### 5. Graph-in-Graph Nesting (Subflow)

Embed an entire `Flow` as a single node inside a parent graph, with optional **predicate-driven loop execution**.

```cpp
tfl::Flow subflow;
subflow.emplace([]{ puts("Subflow tick"); });

int loops = 0;
// Mount subflow into the main graph with a loop predicate
flow.emplace(std::move(subflow), [&loops]() mutable noexcept {
    return ++loops >= 5; // Stop after 5 iterations
});
```

---

## 🛡️ Modern C++23 Design Techniques

TaskflowLite employs numerous cutting-edge defensive programming patterns:

* **Generic Lambda Guard (Arity-Guard)**: Traditional TMP easily triggers Hard Errors when probing `[](auto x)` with `std::invocable`. TFL uses `requires` expressions for arity sniffing, perfectly supporting unconstrained generic closures.
* **Reference Unwrapping Transparency (`std::ref` Unwrap)**: The framework stores closures using `unwrap_ref_decay_t`, allowing users to pass state via `std::ref` with zero copies — just like `std::thread` — while Concept validation sees the true underlying reference type.
* **Tag Dispatching Priority Routing**: Eliminates overload resolution ambiguity caused by `std::bind` type erasure, producing precise and readable compile errors.

---

## 🗂️ Task Type Cheat Sheet

`emplace` uses C++23 Concepts to automatically deduce your closure signature — no explicit type tagging required:

| Signature | Deduced Type | Description | Visual Style |
| --- | --- | --- | --- |
| `[]()` | **Basic** | Plain sequential task, zero abstraction overhead | Grey rectangle |
| `[](tfl::Runtime&)` | **Runtime** | Dynamically dispatch tasks, cooperative blocking wait | Pink rectangle |
| `[](tfl::Branch)` | **Branch** | Single-path conditional select (activates 1 path) | Blue diamond |
| `[](tfl::MultiBranch)` | **MultiBranch** | Multi-path parallel dispatch (activates N paths) | Blue hexagon |
| `[](tfl::Jump)` | **Jump** | Forced state-machine back-jump (supports retry loops) | Red dashed diamond |
| `[](tfl::MultiJump)` | **MultiJump** | Parallel fan-out forced jumps | Red dashed hexagon |
| Pass a `Flow` object | **Subflow** | Embed an entire graph as a single node | Green group box |

---

## 🎨 D2 Graph Visualization Export

How do you debug an extremely complex nested topology? Export it in one line as D2 declaration language and render it instantly via [D2 Playground](https://play.d2lang.com) or local tooling.

```cpp
std::ofstream file("pipeline.d2");
flow.name("MyPipeline").dump(file);
```

![D2 Visualization](documentation/img/d2.svg)

The exported graph is immediately readable: **grey solid lines** represent normal transitions, **blue lines** represent conditional branch choices, and **red dashed lines** represent topology-breaking back-edges from jumps.

---

## ⚙️ Performance Optimizations

TaskflowLite squeezes every clock cycle at the lowest level:

1. **Cache-Line Isolation**: Strict `alignas(std::hardware_destructive_interference_size)` on hot atomics (e.g., queue `top/bottom`) completely eliminates multicore **false sharing**.
2. **Edge Storage Optimization**: Successor and predecessor pointers for each `Work` node are packed into a single contiguous `std::vector<Work*>`, accessed via cursor offsets — eliminating one heap allocation and improving L1 cache hit rates.
3. **Zero-Overhead Exception Elision**: If your closure is marked `noexcept`, the compiler strips the surrounding `try-catch` assembly when instantiating `invoke()`.
4. **Divisionless Distribution**: The random steal module uses Lemire's divisionless bounded mapping algorithm with bitwise operations, significantly reducing CPU cycle cost.

---

## 🛠️ Build Requirements & Integration

**System Requirements:**

* **C++ Standard**: C++23 or later.
* **Compiler**: GCC 12+, Clang 15+, MSVC 2022+ (full Concepts and structured binding support required).

**Integration (Header-Only):**

No library compilation needed. Simply place the `taskflowlite/` directory on your include path:

```cpp
#include "taskflowlite/taskflowlite.hpp"
```

**Recommended CMake Options:**

```cmake
set(CMAKE_CXX_STANDARD 23)
target_compile_options(your_target PRIVATE -O3 -march=native)
```

---

## 🚀 Benchmarks

Testing an extremely dense fully-connected mesh DAG (100 layers × 100 tasks per layer, fully interconnected, executed 100 times):

```cpp
// See benchmark/benchmark.cpp for full code
// Build a 100×100 fully-connected matrix network
for (std::size_t layer = 1; layer < 100; ++layer) {
    for (auto& prev : layers[layer - 1])
        for (auto& curr : layers[layer])
            prev.precede(curr); // Dense dependency wiring
}
executor.submit(flow, 100).start().wait();
```

---

## 📄 License

This project is open-sourced under the [MIT License](LICENSE).

*TaskflowLite — Built for developers who demand peak performance and modern C++ aesthetics.*