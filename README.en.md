# TaskflowLite

[![CI](https://github.com/wicyn/taskflowlite/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/wicyn/taskflowlite/actions/workflows/ci.yml)
[![Linux](https://img.shields.io/badge/Linux-passing-success?logo=linux&logoColor=white)](https://github.com/wicyn/taskflowlite/actions/workflows/ci.yml)
[![macOS](https://img.shields.io/badge/macOS-passing-success?logo=apple&logoColor=white)](https://github.com/wicyn/taskflowlite/actions/workflows/ci.yml)
[![Windows](https://img.shields.io/badge/Windows-passing-success?logo=windows&logoColor=white)](https://github.com/wicyn/taskflowlite/actions/workflows/ci.yml)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue?logo=cplusplus)](https://en.cppreference.com/w/cpp/23)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Header Only](https://img.shields.io/badge/Header--Only-Yes-success)](#)

[简体中文](README.md) · **English**

TaskflowLite (tfl) is a modern C++23 concurrency scheduling library inspired by [Taskflow](https://github.com/taskflow/taskflow).

---

## Table of Contents

1. [Quick Start](#quick-start)
2. [Building DAGs](#building-dags)
3. [Task Types](#task-types)
4. [Runtime Dynamic Scheduling](#runtime-dynamic-scheduling)
5. [Control Flow](#control-flow)
6. [Resource Control](#resource-control)
7. [Visualization](#visualization)
8. [Build & Integration](#build--integration)
9. [Project Structure](#project-structure)
10. [Benchmark Results](#benchmark-results)
11. [Examples & Tests](#examples--tests)
12. [License](#license)

## Quick Start

### Minimal Example

```cpp
#include "taskflowlite/taskflowlite.hpp"
#include <iostream>

int main() {
    tfl::Executor executor(4);
    tfl::Flow flow;

    auto [A, B, C, D] = flow.emplace(
        [] { std::cout << "Task A\n"; },
        [] { std::cout << "Task B\n"; },
        [] { std::cout << "Task C\n"; },
        [] { std::cout << "Task D\n"; }
    );

    // A -> {B, C} -> D
    A.precede(B, C);
    D.succeed(B, C);

    executor.async(flow).wait();
}
```

### Tasks with Arguments

```cpp
tfl::Flow flow;
int counter = 0;

auto [t1, t2] = flow.emplace(
    tfl::pack{ [](int a) { std::cout << "Val: " << a << "\n"; }, 42 },
    tfl::pack{ [](int& c) { c = 100; }, std::ref(counter) }
);

t1.precede(t2);
executor.async(flow).wait();
// counter == 100
```

### Loop Execution

```cpp
// Fixed count
executor.async(flow, 5ULL).wait();

// Predicate-driven
int round = 0;
executor.async(flow, [&]() noexcept { return ++round >= 10; }).wait();
```

---

## Building DAGs

### Creating Nodes

```cpp
tfl::Flow flow;

// 1. No-arg lambda
auto t1 = flow.emplace([] { /* work */ });

// 2. Lambda + value args (framework copies)
auto t2 = flow.emplace([](int x, double y) { /* ... */ }, 42, 3.14);

// 3. Lambda + std::ref (zero-copy)
int state = 0;
auto t3 = flow.emplace([](int& s) { s = 99; }, std::ref(state));

// 4. Function pointer
auto t4 = flow.emplace(&my_function, arg1, arg2);

// 5. Functor
auto t5 = flow.emplace(MyFunctor{multiplier}, std::ref(data));

// 6. Member function pointer
MyService svc;
auto t6 = flow.emplace(&MyService::process, &svc, 42);
auto t7 = flow.emplace(&MyService::process, std::ref(svc), 99);
```

### Weaving Dependencies

```cpp
// Fluent chaining — lvalue returns reference
t1.name("Step1")
  .precede(t2, t3)
  .acquire(io_sem)
  .release(io_sem);

// succeed = reverse precede
t4.succeed(t2, t3);

// Batch insert + structured bindings
auto [a, b, c] = flow.emplace(
    [] { load(); },
    [] { transform(); },
    [] { save(); }
);
a.precede(b).precede(c);
```

### Graph Operations

```cpp
flow.erase(task);        // O(1) removal (swap-with-last)
flow.clear();            // Remove all nodes
flow.empty();            // Check if empty
flow.size();             // Total node count
flow.for_each([](tfl::Task t) { /* iterate */ });
flow.name("MyPipeline"); // Name for debugging/visualization
```

---

## Task Types

`Flow::emplace` uses C++20 Concepts to dispatch to the correct node factory at compile time:

### Basic — Plain Task

```cpp
flow.emplace([] { /* no Runtime access */ });
flow.emplace([](int x) { /* with args */ }, 42);
```

### Runtime — Dynamic Scheduling

```cpp
flow.emplace([](tfl::Runtime& rt) {
    rt.detach([] { /* fire-and-forget */ });
    auto fut = rt.async([](int x) { return x * 2; }, 21);
    rt.cowait();
    int val = fut.get();  // 42
});
```

### Branch — Single-Target Conditional

```cpp
auto decide = flow.emplace([](tfl::Branch& br) {
    if (condition) br.select(0); else br.select(1);
});
auto good = flow.emplace([] { std::cout << "OK\n"; });
auto bad  = flow.emplace([] { std::cout << "Fail\n"; });
decide.precede(good, bad);
```

Also supports `operator()` and `select_if`:

```cpp
auto br = flow.emplace([](tfl::Branch& br) {
    br(2);   // equivalent to br.select(2)
    br.select_if([](tfl::TaskView tv) { return tv.name() == "target"; });
});
```

### MultiBranch — Multi-Target Broadcast

```cpp
auto mb = flow.emplace([](tfl::MultiBranch& mb) {
    mb.select(0, 2);       // activate successors 0 and 2
    // mb.select_all();    // activate all
    // mb.select_if(...);  // filter by name
});
mb.precede(t0, t1, t2, t3);
```

### Jump — Forced Jump (Loop / Retry)

```cpp
auto process = flow.emplace([] { /* work */ });

auto retry = flow.emplace([&](tfl::Jump& jmp) {
    if (++attempt < max)
        jmp.select(0);       // jump back to process (target[0])
    // No select → natural completion
});

process.precede(retry);
retry.precede(process);       // weight=0, excluded from cycle detection
```

### MultiJump — Multi-Target Forced Jump

```cpp
auto mj = flow.emplace([](tfl::MultiJump& mj) {
    mj.select(0, 1, 2);   // simultaneously reset three join_counters
});
```

### Subflow — Nested Flow

```cpp
tfl::Flow inner;
inner.emplace([]{ std::cout << "Inner task\n"; });

// Single execution
flow.emplace(std::move(inner));

// Predicate-driven loop (5 iterations)
int i = 0;
flow.emplace(std::move(inner), [&i]() mutable noexcept { return ++i >= 5; });
```

---

## Runtime Dynamic Scheduling

### Runtime API

```cpp
flow.emplace([](tfl::Runtime& rt) {
    // Fire-and-forget
    rt.detach([] { background_work(); });

    // Async with results
    auto f1 = rt.async([] { return compute_a(); });
    auto f2 = rt.async([](int n) { return compute_b(n); }, 100);

    // Cooperative wait (worker steals tasks, never blocks)
    rt.cowait_until([&] {
        return f1.wait_for(0s) == std::future_status::ready
            && f2.wait_for(0s) == std::future_status::ready;
    });

    int result = f1.get() + f2.get();
    std::cout << "Result: " << result << "\n";
});
```

### How Cowait Works

```
Normal wait:     Worker thread → OS blocks → CPU wasted
TFL cowait:      Worker thread → steals other tasks → CPU never idles
```

During `cowait` / `cowait_until`, the worker continuously steals tasks from its local queue or neighbors. It never performs a system-level block, completely eliminating deadlock risks in recursive scheduling and subflow nesting.

### AsyncTask

`AsyncTask` is a reference-counted handle (vs `Task` which is a weak reference):

```cpp
auto t1 = tfl::NonrepeatAsyncTask([] { step_a(); });
auto t2 = tfl::NonrepeatAsyncTask([] { step_b(); });
auto t3 = tfl::NonrepeatAsyncTask([] { step_c(); });

executor.submit(t2, t1);   // t2 depends on t1
executor.submit(t3, t2);   // t3 depends on t2

t3.wait();                 // Wait for the entire chain
```

---

## Control Flow

### Exception Handling

```cpp
// Terminate on uncaught exception (default)
tfl::ResumeNever handler;
tfl::Executor exec(handler, 4);

// Ignore exceptions, successors continue
tfl::ResumeAlways handler;
tfl::Executor exec(handler, 4);
```

### Cancellation

```cpp
auto task = tfl::NonrepeatAsyncTask([] { /* long work */ });
executor.submit(task);
// ...
task.request_stop();  // Set soft interrupt
task.wait();          // Node self-checks before next invoke and skips
```

### TaskObserver

```cpp
struct MyTracer : tfl::TaskObserver {
    void on_before(tfl::TaskView tv) override {
        std::cout << "Start: " << tv.name() << "\n";
    }
    void on_after(tfl::TaskView tv) override {
        std::cout << "End: " << tv.name() << "\n";
    }
};

auto t = flow.emplace([] { /* work */ });
t.register_observer<MyTracer>();
```

---

## Resource Control

### Semaphore — Task-Level Concurrency Limit

```cpp
tfl::Semaphore db_pool(4);   // max 4 concurrent

for (int i = 0; i < 20; ++i) {
    flow.emplace([i] { query_database(i); })
        .acquire(db_pool)
        .release(db_pool);
}
// Excess tasks suspend without occupying worker threads
```

### Advanced Usage

```cpp
tfl::Semaphore sem(3, 0);               // capacity 3, initial available 0
producer.release(sem, 3);               // batch release 3 permits
consumer.acquire(sem);                  // activate after producer releases

sem.reset(10);                          // dynamic resize
sem.value();                            // current available
sem.max_value();                        // maximum capacity
```

---

## Visualization

```cpp
std::ofstream file("pipeline.d2");
flow.name("MyPipeline").dump(file);

// Or get the string directly
std::string d2 = flow.dump();
std::cout << d2;
```

Paste the output into the [D2 Playground](https://play.d2lang.com) to render. Legend:

- **Gray solid line** — Normal dependency edge
- **Blue line** — Conditional branch
- **Red dashed line** — Jump back-edge

---

## Build & Integration

### Requirements

| Compiler | Minimum Version |
|----------|----------------|
| GCC | 12+ |
| Clang | 15+ |
| MSVC | 2022+ (17.0+) |
| Apple Clang | 15+ (Xcode 15+) |

- **C++ Standard**: C++23
- **CMake**: 3.21+
- **Dependencies**: C++ standard library + pthread (Unix)

### CMake

```bash
# Basic build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel

# With tests
cmake -S . -B build -DTFL_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build -C Release --output-on-failure

# Sanitizer
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTFL_SANITIZER=ASAN
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DTFL_SANITIZER=TSAN
```

### Network / Mirror (for restricted regions)

Enabling tests or benchmarks may fetch dependencies from GitHub (Catch2 for tests,
Taskflow for benchmarks). If direct GitHub access is unreliable, switch the mirror
at configure time (default is `direct`):

```bash
# choices: direct / ghfast.top / gh-proxy.com / ghproxy.net
cmake -S . -B build -DTFL_BUILD_TESTS=ON -DTFL_GITHUB_MIRROR=ghfast.top
```

Advanced (under cmake-gui "Advanced"): `TFL_GITHUB_PREFIX` (custom prefix overriding
the mirror), `TFL_GIT_PROXY` (route git through a local HTTP(S) proxy, e.g.
`http://127.0.0.1:7890`).

### As a Dependency

```cmake
# add_subdirectory
add_subdirectory(path/to/taskflowlite)
target_link_libraries(your_app PRIVATE TaskflowLite::taskflowlite)

# FetchContent
include(FetchContent)
FetchContent_Declare(taskflowlite
    GIT_REPOSITORY https://github.com/wicyn/taskflowlite.git
    GIT_TAG v1.2.0)   # pin a release tag rather than main for reproducibility
FetchContent_MakeAvailable(taskflowlite)
target_link_libraries(your_app PRIVATE TaskflowLite::taskflowlite)
```

### Header-Only

```cpp
#include "taskflowlite/taskflowlite.hpp"  // one line, compile with -std=c++23 -pthread
```

---

## Project Structure

```
taskflowlite/
├── taskflowlite/
│   ├── taskflowlite.hpp                   # Unified include
│   └── core/                              # 30+ core headers
│       ├── executor.hpp                   # Scheduling engine
│       ├── flow.hpp / task.hpp            # DAG builder & task handles
│       ├── async_task.hpp / runtime.hpp   # Dynamic tasks & runtime
│       ├── work.hpp / works.hpp           # Node base & factories
│       ├── branch.hpp / jump.hpp          # Control flow
│       ├── semaphore.hpp / observer.hpp   # Resources & observation
│       ├── bounded_queue.hpp etc.         # Concurrency primitives
│       └── traits.hpp / utility.hpp       # Concepts & utilities
├── test/                                  # 23 test files (Catch2 v3)
├── examples/                              # 25 examples
├── benchmarks/                            # Performance comparison (vs Taskflow)
├── .github/workflows/ci.yml               # CI matrix
├── CMakeLists.txt
├── LICENSE (MIT)
└── README.md
```

---

## Benchmark Results

TaskflowLite vs Taskflow — **same hardware, threads, topology, and total iterations**.

**Test Environment:** Intel Core i7-9750H @ 2.60GHz (6C/12T), Windows 11, MSVC 2022 /O2

| # | Scenario | Config | TaskflowLite | Taskflow | Speedup |
|--:|---------|------|--------:|------------:|------:|
| 01 | 32 parallel | 8 thr · 500k | 1009 ms | 1479 ms | **1.47×** |
| 02 | 32 serial | 1 thr · 1M | 662 ms | 1323 ms | **2.00×** |
| 03 | Diamond DAG | 2 thr · 1M | 255 ms | 400 ms | **1.57×** |
| 04a | 4×2 full | 2 thr · 1M | 504 ms | 663 ms | **1.32×** |
| 04b | 6×4 full | 4 thr · 500k | 1737 ms | 1964 ms | **1.13×** |
| 04c | 8×8 full | 8 thr · 100k | 1076 ms | 1309 ms | **1.22×** |
| 04d | 8×16 full | 8 thr · 50k | 1250 ms | 1531 ms | **1.22×** |
| 04e | 8×32 full | 8 thr · 20k | 1210 ms | 1795 ms | **1.48×** |
| 04f | 6×100 full | 8 thr · 2k | 516 ms | 778 ms | **1.51×** |
| 05 | Binary tree | 8 thr · 500k | 1969 ms | 3278 ms | **1.66×** |
| 06 | 1→256→1 fan | 8 thr · 100k | 3395 ms | 4167 ms | **1.23×** |
| 07 | 16 pipelines | 8 thr · 200k | 911 ms | 2591 ms | **2.84×** |
| 08 | 16×16 grid | 8 thr · 100k | 1228 ms | 2978 ms | **2.43×** |
| 09 | Sparse DAG | 8 thr · 500k | 2508 ms | 4042 ms | **1.61×** |
| 10 | Jump loop | 1 thr · 1M | 30 ms | 53 ms | **1.77×** |
| 11 | MultiJump loop | 4 thr · 200k | 58 ms | 82 ms | **1.41×** |
| 12 | Subflow once | 4 thr · 200k | 160 ms | 210 ms | **1.31×** |
| 13 | Subflow loop | 2 thr · 500k | 105 ms | 168 ms | **1.60×** |
| 14 | Empty task | 1 thr · 10M | 473 ms | 642 ms | **1.36×** |
| 15 | Parallel for | 8 thr · 1024×10k | 734 ms | 1221 ms | **1.66×** |
| 16 | Reduce tree | 8 thr · 127×50k | 465 ms | 828 ms | **1.78×** |
| 17 | Scan chain | 1 thr · 128×100k | 235 ms | 570 ms | **2.43×** |
| 18 | Wavefront | 8 thr · 210×10k | 115 ms | 262 ms | **2.28×** |
| 19 | Heterogeneous | 8 thr · 18×100k | 851 ms | 878 ms | **1.03×** |
| 20 | Memory stress | 8 thr · 2000×500 | 774 ms | 1144 ms | **1.48×** |
| | **Geometric mean** | | | | **≈ 1.58×** |

> Full benchmark source code in [benchmarks/](benchmarks/).

---

## Examples & Tests

### Examples

```bash
cmake -S . -B build -DTFL_BUILD_EXAMPLES=ON
cmake --build build --config Release

./build/bin/examples/01_basic_dag       # Basic DAG
./build/bin/examples/05_branch          # Conditional branching
./build/bin/examples/09_pipeline        # Map-Reduce pipeline
```

### Tests

```bash
cmake -S . -B build -DTFL_BUILD_TESTS=ON
cmake --build build --config Release

# All tests
./build/bin/TaskflowLiteTest

# Single file (build on demand)
cmake --build build --target tfl_test_queue
./build/bin/tfl_test_queue
```

> Tests depend on Catch2 v3 amalgamated: used directly if present under `test/`
> (offline, reproducible); otherwise auto-downloaded once `-DTFL_BUILD_TESTS=ON`
> is set (download reuses the `TFL_GITHUB_MIRROR` mirror above). `TFL_BUILD_TESTS`
> defaults to OFF — tests are built (and the download triggered) only when enabled.

### Benchmarks

```bash
cmake -S . -B build -DTFL_BUILD_BENCHMARKS=ON
cmake --build build --config Release

./build/bin/bench_taskflowlite
./build/bin/bench_taskflow
```

> Benchmarks only require Taskflow (header-only), auto-cloned during configure. Offline: use `-DTASKFLOW_LOCAL_PATH=<path>`.

---

## License

[MIT License](LICENSE)

*TaskflowLite — built for developers who demand extreme performance and modern C++ aesthetics.*
