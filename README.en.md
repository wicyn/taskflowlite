# TaskflowLite

[![Ubuntu](https://github.com/wicyn/taskflowlite/actions/workflows/ubuntu.yml/badge.svg?branch=main)](https://github.com/wicyn/taskflowlite/actions/workflows/ubuntu.yml)
[![Windows](https://github.com/wicyn/taskflowlite/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/wicyn/taskflowlite/actions/workflows/windows.yml)
[![macOS](https://github.com/wicyn/taskflowlite/actions/workflows/macos.yml/badge.svg?branch=main)](https://github.com/wicyn/taskflowlite/actions/workflows/macos.yml)
[![CodeQL](https://github.com/wicyn/taskflowlite/actions/workflows/codeql-analysis.yml/badge.svg?branch=main)](https://github.com/wicyn/taskflowlite/actions/workflows/codeql-analysis.yml)
[![CI Extras](https://github.com/wicyn/taskflowlite/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/wicyn/taskflowlite/actions/workflows/ci.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue?logo=cplusplus)](#requirements)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Header Only](https://img.shields.io/badge/Header--Only-Yes-success)](#installation-and-integration)

[简体中文](README.md) · **English**

TaskflowLite (tfl) is a lightweight, header-only C++20 task-parallel library inspired by [Taskflow](https://github.com/taskflow/taskflow). It provides task dependency graphs, asynchronous scheduling, and runtime control flow.

Describe the dependencies between tasks, and the executor schedules ready tasks on its worker pool. Use it for dependent computations, batch workflows, and tasks whose child work is only known at runtime.

[Quick start](#quick-start) · [Installation](#installation-and-integration) · [Core concepts](#core-concepts) · [Usage](#basic-usage) · [Execution semantics](#execution-semantics-and-lifetimes) · [Build and test](#building-and-testing) · [Documentation](#documentation-and-examples)

[![TaskflowLite task graph overview: branches, jumps, nested graphs, and a task-local TaskGroup](documentation/img/taskflowlite-overview.png)](documentation/img/taskflowlite-overview.png)

## Features

- **Task graphs**: DAG construction, dependencies, placeholders, and task rebinding.
- **Asynchronous tasks**: immediate submission, deferred execution, dependencies, and shared results.
- **Object tasks**: `TaskObject<T>` / `AsyncTaskObject<R, T>` construct business objects in place and expose their state through `object()`, including non-copyable, non-movable types.
- **Dynamic scheduling**: Runtime, dynamic SubFlow, and scoped TaskGroup.
- **Control flow**: branches, multi-branches, jumps, repeated execution, and nested modules.
- **Execution control**: cooperative waiting, exception propagation, cooperative cancellation, and semaphore limits.
- **Observability**: task observers, Worker lifecycle callbacks, and D2 graph export.
- **Integration**: header-only library with an exported CMake target.

## Requirements

- A C++20 compiler and standard library with support for `std::format`, atomic waiting, and related C++20 facilities.
- GCC 13 or newer with its matching libstdc++; Clang / MSVC require a standard library providing the same facilities.
- CMake 3.21 or newer when using CMake.

The core library has no third-party runtime dependencies. Tests use Catch2, comparisons use Taskflow, and API documentation requires Doxygen; these dependencies are used only when their build options are enabled. D2 is only needed to render exported graph text as an image.

The repository configures CI for Ubuntu, Windows, and macOS. Windows uses Visual Studio 2022; the macOS workflow uses Homebrew LLVM with its matching libc++. See the [CI workflows](.github/workflows) for the configurations and the badges for their run status.

## Quick Start

```cpp
#include <iostream>
#include <taskflowlite/taskflowlite.hpp>

int main() {
    tfl::Executor executor(4);
    tfl::Flow flow;

    const int input = 10;
    int left = 0;
    int right = 0;
    int result = 0;

    auto [A, B, C] = flow.emplace(
        [input, &left] { left = input * 2; },
        [input, &right] { right = input + 12; },
        [&left, &right, &result] { result = left + right; }
    );

    C.succeed(A, B);
    executor.async(flow).get();

    std::cout << result << '\n';  // 42
}
```

A and B may run in parallel; C runs after both finish, and the program prints `42`. `C.succeed(A, B)` adds dependencies, `executor.async(flow)` submits the graph, and `get()` waits for completion and propagates exceptions.

Save this as `main.cpp` and use the CMake configuration below to build it. See [01_basic_dag.cpp](examples/01_basic_dag.cpp) for a complete standalone example.

## Installation and Integration

### CMake Subproject

Place the repository at `external/taskflowlite` in your project and save the code above as `main.cpp`:

```cmake
cmake_minimum_required(VERSION 3.21)
project(my_app LANGUAGES CXX)

add_subdirectory(external/taskflowlite)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE TaskflowLite::taskflowlite)
```

`TaskflowLite::taskflowlite` supplies include paths, the C++20 requirement, and platform link dependencies.

Build from the consuming project's root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
```

With a single-configuration generator, run `build/my_app` (with `.exe` on Windows). Multi-configuration generators such as Visual Studio typically place it at `build/Release/my_app.exe`.

### Installed Package

Run from the TaskflowLite repository root:

```bash
cmake -S . -B build/install -DTFL_BUILD_EXAMPLES=OFF -DTFL_BUILD_TESTS=OFF -DTFL_BUILD_BENCHMARKS=OFF
cmake --install build/install --prefix ./install
```

In the consuming project's `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.21)
project(my_app LANGUAGES CXX)

find_package(TaskflowLite CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE TaskflowLite::taskflowlite)
```

Configure the consumer with `-DCMAKE_PREFIX_PATH=/absolute/path/to/taskflowlite/install`, replacing the path with the absolute installation directory from the previous step. Installing only the headers and CMake package does not require compiling the library first.

### Using the Headers Directly

Copy the repository's `taskflowlite/` directory into your project's include directory and include `<taskflowlite/taskflowlite.hpp>`. Enable C++20 and configure threading and platform link dependencies yourself; CMake integration handles these settings automatically.

## Core Concepts

| Type | Responsibility | When to use it |
| --- | --- | --- |
| `Executor` | Owns worker threads and schedules tasks | Submit graphs and asynchronous tasks; wait for all work |
| `Flow` / `Task` | Owns a graph / refers to a graph node | Build dependencies before execution; resubmit after completion |
| `AsyncFuture<R>` | Shares completion state and a result | Receive an `async()` result or supply an asynchronous predecessor |
| `AsyncTask<R>` | Configurable, deferred shared task handle | Create with `defer_async()`, configure, then call `start()` |
| `Runtime` | Scheduling context inside a running task | Dispatch children and wait cooperatively |
| `SubFlow` | Graph constructed inside a running task | Build a dynamic graph and explicitly call `run()` |
| `TaskGroup` | Owns a scope for child work | Wait for a group and handle errors locally |
| `TaskObject<T>` / `AsyncTaskObject<R, T>` | Constructs a callable object in place | Associate business state with a task, including non-copyable, non-movable objects |
| `Branch` / `MultiBranch` | Selects one or more successors | Conditional paths |
| `Jump` / `MultiJump` | Explicit jump control flow | Retries, state machines, and loops |
| `Semaphore` | Manages resource quotas around task execution | Limit concurrency and coordinate scarce resources |

Start with `Flow` when you know the dependency graph, `async()` when you need a computation's result, and `defer_async()` when you need to configure work before submitting it. Use `Runtime` or `SubFlow` when child work is only known during execution.

## Basic Usage

The following C++ snippets are independent and belong inside a function body. Use the headers from the quick start and create `tfl::Executor executor(4)` before each snippet. The exception example also needs `<stdexcept>`.

### Asynchronous Tasks and Dependencies

```cpp
auto left = executor.async([] { return 20; });
auto right = executor.async([] { return 22; });

auto sum = executor.async(
    [left, right] { return left.get() + right.get(); },
    left, right
);

int result = sum.get();  // 42
```

`async` submits a task immediately and returns an `AsyncFuture<R>`. Arguments after the callable specify predecessors. Futures can be copied to share a result, and `get()` may be called repeatedly.

An asynchronous dependency means that a predecessor has completed; it does not automatically pass along a result or exception. Calling the predecessors' `get()` in this example reads their results and propagates their exceptions into `sum`.

### Deferred Execution

```cpp
auto first = executor.defer_async([] { return 21; });
auto second = executor.defer_async([first] { return first.get() * 2; });

first.start();
second.start(first);

int result = second.get();  // 42
```

`defer_async()` creates an idle task bound to the executor. Configure it, then call `start(dependencies...)` once.
Predecessors must already be started or finished. Mixed `AsyncTask` / `AsyncFuture` dependencies are supported; empty handles are ignored.
Each duplicate dependency retains a strong reference until the successor task object is destroyed. The executor must outlive submission and execution.
Use `Runtime::async()` or `TaskGroup::async()` for child tasks; `defer_async().start()` creates an independent top-level task.

See the [dependency notes](documentation/async-task-dependency-design.md) for the full contract and allocation-failure limitations.

### Repeated Execution

```cpp
int count = 0;
tfl::Flow flow;
(void)flow.emplace([&count] { ++count; });

executor.async(flow, 5ULL).get();  // count == 5
executor.async(flow, [&count]() noexcept {
    return count >= 10;
}).get();                        // count == 10
```

The count specifies the number of iterations. A stop predicate is checked before each iteration; returning `true` ends execution.

### Runtime Task Groups

```cpp
auto future = executor.async([](tfl::Runtime& runtime) {
    tfl::TaskGroup group(runtime);

    auto left = group.async([] { return 20; });
    auto right = group.async([] { return 22; });

    group.wait();
    return left.get() + right.get();
});

int result = future.get();  // 42
```

`Runtime` dispatches tasks during execution. `TaskGroup` manages a set of child tasks, and `wait()` cooperatively waits for the group to complete.

The `TaskGroup` destructor waits but does not rethrow child exceptions. Call `group.wait()` explicitly to catch and handle group failures. See the [error handling example](examples/16_error_handling.cpp).

### Dynamic Subgraphs

```cpp
int result = 0;

auto future = executor.async([&result](tfl::SubFlow& subflow) {
    auto A = subflow.emplace([&result] { result = 21; });
    auto B = subflow.emplace([&result] { result *= 2; });

    A.precede(B);
    subflow.run();
    subflow.wait();
});

future.get();  // result == 42
```

`SubFlow` builds a graph during task execution. Call `run()` to submit it and `wait()` to wait cooperatively.

Calling `emplace()` alone does not execute the subgraph. Do not retain the framework-provided `Runtime&` or `SubFlow&` beyond the task callback.

### Conditional Branches

```cpp
bool enabled = true;
int result = 0;
tfl::Flow flow;

auto condition = flow.emplace([enabled](tfl::Branch& branch) {
    branch.select(enabled ? 0 : 1);
});
auto yes = flow.emplace([&result] { result = 1; });
auto no = flow.emplace([&result] { result = -1; });

condition.precede(yes, no);
executor.async(flow).get();  // result == 1
```

Branch indices are zero-based and follow the successor order in `precede`. `MultiBranch` selects multiple successors; `Jump` / `MultiJump` provide jump-based control flow.

### Stateful Object Tasks

```cpp
struct Accumulator {
    explicit Accumulator(int initial) : value(initial) {}
    Accumulator(const Accumulator&) = delete;
    Accumulator& operator=(const Accumulator&) = delete;

    int operator()() { return value += 2; }
    int value;
};

auto task = executor.defer_async_object<Accumulator>(40);
task.start();

int result = task.get();              // 42: execution result
int state = task.object().value;      // 42: task object state
```

`object()` exposes business state; `get()` accesses the execution result. Configure objects before starting them and synchronize any concurrent access to their state. For graph nodes, use `flow.emplace_object<T>(constructor_args...)`. See [object tasks](examples/31_task_object.cpp) and [asynchronous object tasks](examples/32_async_task_object.cpp).

### Semaphore Limits

```cpp
tfl::Semaphore slots(2);
tfl::Flow flow;

for (int i = 0; i < 8; ++i) {
    auto task = flow.emplace([] { /* Access a limited resource. */ });
    task.acquire(slots).release(slots);
}

executor.async(flow).get();
```

At most two of these tasks can run concurrently, even with four workers. Use `acquire(sem, count)` / `release(sem, count)` for multiple units. Keep semaphores alive until the associated tasks complete; acquire traversal order is not guaranteed to match insertion order. See the [semaphore example](examples/07_semaphore.cpp).

### Error Handling

```cpp
auto future = executor.async([]() -> int {
    throw std::runtime_error("computation failed");
});

try {
    (void)future.get();
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
}
```

`wait()` only waits; `get()` also rethrows a stored exception. Static graph failures reach the Future returned by graph submission; dependent successors of a failed node do not continue normal execution. Top-level `silent_async()` has no result handle, so handle reportable errors inside the task. See [exceptions and dependency errors](examples/34_dependency_errors.cpp) for more cases.

## Execution Semantics and Lifetimes

| Topic | Contract |
| --- | --- |
| Graph lifetime | A graph submitted as an lvalue and any referenced data must outlive execution. `Task` is a non-owning handle and becomes invalid when its node is erased or its graph is destroyed. |
| Graph reuse | Resubmit after completion. Do not change graph structure or task configuration during execution, or submit the same graph concurrently. |
| Data synchronization | Edges express execution order. Independent tasks sharing writable data need their own atomics or locks. |
| Future results | Futures are copyable; `get()` does not consume the result. Value results are returned as `const R&`; keep the underlying task alive while retaining that reference. Reference results also depend on the original object's lifetime. |
| Deferred tasks | A task may be successfully started only once; predecessors must be started or completed. Converting an idle task to a Future does not start it. |
| Waiting on workers | Use cooperative waiting through `Runtime`, `SubFlow`, or `TaskGroup`. Blocking on unfinished work can exhaust the worker pool, especially with one worker. |
| Cooperative cancellation | `request_stop()` requests cancellation; long tasks check `stop_requested()`. It neither interrupts a thread forcibly nor rolls back completed side effects. |
| Child cancellation | `Runtime` / `TaskGroup` submissions through `async()` and `silent_async()` do not inherit parent stop requests by default. Use `<true>` explicitly when needed and respect the context lifetime requirements. |

See the [asynchronous dependency notes](documentation/async-task-dependency-design.md) for dependency rules, retained references, and failure boundaries.

## Building and Testing

Run these commands from a complete repository checkout. Examples are built by default; tests and benchmarks are disabled by default.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
```

Enable tests and run the examples and unit tests:

```sh
cmake -S . -B build/tests -DCMAKE_BUILD_TYPE=Release -DTFL_BUILD_TESTS=ON
cmake --build build/tests --config Release --parallel 4
ctest --test-dir build/tests -C Release --output-on-failure -LE perfile --no-tests=error
```

`-LE perfile` excludes duplicate per-file registrations, matching the platform CI workflows. Configuration downloads and verifies a pinned Catch2 version when no local copy is available. For offline builds, add `-DTFL_CATCH2_LOCAL_PATH=/path/to/catch2/extras`.

Alternatively, use the repository presets:

```sh
cmake --preset release
cmake --build --preset release --parallel 4
ctest --preset release -LE perfile --no-tests=error
```

The `release` preset requires Ninja and enables tests. For Visual Studio on Windows, replace `release` with `windows-release` in all three commands. See the [build guide](cmake/README.md) for sanitizer presets and Windows ASan runtime setup.

| Option | Default | Description |
| --- | --- | --- |
| `TFL_BUILD_EXAMPLES` | ON at top level, OFF as subproject | Build complete examples |
| `TFL_BUILD_TESTS` | OFF | Build and register unit tests |
| `TFL_TEST_HEADERS` | ON | Check standalone header compilation when tests are enabled |
| `TFL_BUILD_BENCHMARKS` | OFF | Build TaskflowLite and Taskflow comparison programs |
| `TFL_BUILD_DOCS` | OFF | Enable the Doxygen `GenerateDocs` target |
| `TFL_SANITIZER` | OFF | `OFF`, `ASAN`, or `TSAN`; MSVC does not support TSAN |
| `TFL_NATIVE_ARCH` | OFF | Enable native CPU optimization for internal GCC/Clang Release targets |

Using only the exported library target does not require downloading Catch2 or Taskflow. See the [build guide](cmake/README.md) for dependency paths, proxy settings, and the full option list.

## Performance Comparison

The repository contains TaskflowLite and Taskflow comparison programs using the same scenarios. Run `--smoke` to check correctness, then compare full workloads on the same machine with matching build settings. See the [benchmark guide](benchmarks/README.md) for commands and timing boundaries.

The results below are historical and have not been remeasured against the current working tree. The original table does not record exact revisions or distributions across repeated runs. Treat it as reference data, not a performance guarantee for the current version or every workload.

<details>
<summary>Historical environment and full results</summary>

Historical benchmark environment: Intel Core i7-9750H @ 2.60 GHz (6 cores / 12 threads), Windows 11, MSVC 2022, `/O2`.

Times are in milliseconds. Speedup = Taskflow time / TaskflowLite time.

| ID | Scenario | Threads × count | TaskflowLite (ms) | Taskflow (ms) | Speedup |
|----|----------|-----------------|------------------:|--------------:|---------------------:|
| 01 | 32 parallel tasks | 8 × 500k | 721.124 | 1231.84 | 1.71× |
| 02 | 32 serial tasks | 1 × 1M | 616.242 | 1367.07 | 2.22× |
| 03 | Diamond DAG | 2 × 1M | 196.331 | 362.007 | 1.84× |
| 04a | 4×2 fully connected layers | 2 × 1M | 422.024 | 613.798 | 1.45× |
| 04b | 6×4 fully connected layers | 4 × 500k | 1158.74 | 1710.31 | 1.48× |
| 04c | 8×8 fully connected layers | 8 × 100k | 808.12 | 1284.82 | 1.59× |
| 04d | 8×16 fully connected layers | 8 × 50k | 977.669 | 1727.62 | 1.77× |
| 04e | 8×32 fully connected layers | 8 × 20k | 1062.28 | 1998.19 | 1.88× |
| 04f | 6×100 fully connected layers | 8 × 2k | 522.272 | 885.16 | 1.69× |
| 05 | Binary tree | 8 × 500k | 1185.87 | 3209.39 | 2.71× |
| 06 | 1→256→1 fan-out / fan-in | 8 × 100k | 2355.74 | 4114 | 1.75× |
| 07 | 16 pipelines | 8 × 200k | 509.024 | 2511.21 | 4.93× |
| 08 | 16×16 grid | 8 × 100k | 962.875 | 2750.96 | 2.86× |
| 09 | Sparse DAG | 8 × 500k | 1780.81 | 3551.17 | 1.99× |
| 10 | Jump retry / conditional loop | 1 × 1M | 37.4647 | 49.0981 | 1.31× |
| 11 | MultiJump / multi-condition loop | 4 × 200k | 44.4267 | 75.4509 | 1.70× |
| 12 | Subgraph execution | 4 × 200k | 103.065 | 182.91 | 1.77× |
| 13 | Subgraph loop | 2 × 500k | 70.1495 | 158.994 | 2.27× |
| 14 | Empty task | 1 × 10M | 188.495 | 624.18 | 3.31× |
| 15 | Parallel for (1024 tasks) | 8 × 10k | 505.264 | 1126.37 | 2.23× |
| 16 | Reduction tree (127 nodes) | 8 × 50k | 312.939 | 680.67 | 2.18× |
| 17 | Scan chain (128 nodes) | 1 × 100k | 223.374 | 498.685 | 2.23× |
| 18 | Wavefront (210 nodes) | 8 × 10k | 97.2584 | 229.078 | 2.36× |
| 19 | Mixed tasks (18 nodes) | 8 × 100k | 721.13 | 903.111 | 1.25× |
| 20 | Memory stress (2000 nodes) | 8 × 500 | 773.992 | 1110.13 | 1.43× |
| | Geometric mean | | | | 1.97× |

`k` = 1,000 and `M` = 1,000,000. Counts for 10, 11, and 13 are internal loop iterations; the others are graph executions.

</details>

## Documentation and Examples

| Topic | Entry point |
| --- | --- |
| Complete guide (Chinese) | [PDF manual](documentation/TaskflowLite-Guide.zh-CN.pdf) · [Manual source](documentation/TaskflowLite-Guide.zh-CN.md) |
| Implementation and scheduling (Chinese) | [Architecture PDF](documentation/TaskflowLite-Architecture.zh-CN.pdf) · [Source](documentation/TaskflowLite-Architecture.zh-CN.md): bounded deque, shared work stacks, wakeups, dependency counters, resource waits, and reclamation, with 48 vector diagrams |
| Example index and run instructions | [examples/README.md](examples/README.md) |
| Build options, dependencies, and installation | [cmake/README.md](cmake/README.md) |
| Test commands | [Building and testing](#building-and-testing) |
| Asynchronous dependencies and lifetimes | [Dependency notes](documentation/async-task-dependency-design.md) |
| Branches, jumps, and modules | [Branches](examples/05_branch.cpp), [jumps](examples/06_jump.cpp), [nested modules](examples/08_subflow.cpp) |
| Dynamic work | [Runtime](examples/04_runtime.cpp), [TaskGroup](examples/26_task_group.cpp), [dynamic subgraphs](examples/27_dynamic_subflow.cpp) |
| Execution control | [Exceptions](examples/16_error_handling.cpp), [cancellation](examples/17_cancellation.cpp), [semaphores](examples/07_semaphore.cpp) |
| Observability | [Observer](examples/15_observer.cpp), [tracing](examples/21_observer_tracing.cpp), [worker callbacks](examples/30_worker_handler.cpp) |
| Benchmark methodology | [benchmarks/README.md](benchmarks/README.md) |

The pipeline examples compose a `Flow`. `core/pipeline.hpp` is a draft and does not provide a usable standalone Pipeline API. The supporting guides linked above are currently written in Chinese.

With Doxygen installed, generate API documentation with:

```sh
cmake -S . -B build/docs -DTFL_BUILD_EXAMPLES=OFF -DTFL_BUILD_DOCS=ON
cmake --build build/docs --target GenerateDocs
```

### Task Graph Visualization

Export D2 text with `flow.dump()`, then render it to SVG with D2. [View the full task graph](documentation/img/d2.svg).

<details>
<summary>Expand the D2 task graph preview</summary>

[![TaskflowLite D2 task graph: dependencies, branches, jumps, nested graphs, and semaphore annotations](documentation/img/d2.svg)](documentation/img/d2.svg)

</details>

## Feedback and Contributing

Report bugs and suggest features through [Issues](https://github.com/wicyn/taskflowlite/issues). Include a minimal reproduction, expected and actual behavior, compiler and standard library versions, operating system, build options, and relevant logs.

Include tests with code changes. Update examples and both READMEs when public interfaces change. Run relevant tests before submitting and use the repository's `.clang-format` for consistent formatting.

## License

This project is licensed under the [MIT License](LICENSE).
