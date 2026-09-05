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

[![TaskflowLite task graph overview: branches, jumps, nested graphs, and a task-local TaskGroup](documentation/img/taskflowlite-overview.png)](documentation/img/taskflowlite-overview.png)

TaskflowLite (tfl) is a lightweight, header-only C++20 task-parallel library inspired by [Taskflow](https://github.com/taskflow/taskflow). It provides task dependency graphs, asynchronous scheduling, and runtime control flow.

## Features

- **Task graphs**: DAG construction, dependencies, placeholders, and task rebinding.
- **Asynchronous tasks**: immediate submission, deferred execution, dependencies, and shared results.
- **Dynamic scheduling**: Runtime, dynamic SubFlow, and scoped TaskGroup.
- **Control flow**: branches, multi-branches, jumps, repeated execution, and nested modules.
- **Execution control**: cooperative waiting, exception propagation, cooperative cancellation, and semaphore limits.
- **Observability**: task observers, Worker lifecycle callbacks, and D2 graph export.
- **Integration**: header-only library with an exported CMake target.

## Requirements

- A C++20 compiler and standard library with support for `std::format`, atomic waiting, and related C++20 facilities.
- GCC 13 or newer with its matching libstdc++; Clang / MSVC require a standard library providing the same facilities.
- CMake 3.21 or newer when using CMake.

---

## Quick Start

```cpp
#include <cstring>
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

A and B may run in parallel; C runs after both finish. Pass task data through lambda captures. `get()` waits for completion and propagates exceptions.

---

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

### Installed Package

Run from the TaskflowLite repository root:

```bash
cmake -S . -B build/install -DTFL_BUILD_EXAMPLES=OFF -DTFL_BUILD_TESTS=OFF -DTFL_BUILD_BENCHMARKS=OFF
cmake --install build/install --prefix /path/to/taskflowlite-install
```

In the consuming project's `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.21)
project(my_app LANGUAGES CXX)

find_package(TaskflowLite CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE TaskflowLite::taskflowlite)
```

Configure the consumer with `-DCMAKE_PREFIX_PATH=/path/to/taskflowlite-install`, replacing the path with your installation directory.

### Building the Repository

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4
```

| Option | Default | Description |
|--------|---------|-------------|
| `TFL_BUILD_EXAMPLES` | ON at top level, OFF as subproject | Build examples |
| `TFL_BUILD_TESTS` | OFF | Build unit tests |
| `TFL_BUILD_BENCHMARKS` | OFF | Build benchmarks |
| `TFL_SANITIZER` | OFF | OFF, ASAN, or TSAN; MSVC does not support TSAN |

---

## Basic Usage

The following snippets are independent. Use the headers from the quick start and create `tfl::Executor executor(4)` before each snippet.

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

### Deferred Execution

```cpp
tfl::AsyncTask first([] { return 21; });
tfl::AsyncTask second([first] { return first.get() * 2; });

executor.run(second, first);
executor.run(first);

int result = second.get();  // 42
```

`AsyncTask` is submitted by `run()`. Each task can be started only once, and its dependencies must also be started explicitly.

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

### Usage Guidelines

- Keep graphs and referenced captures alive until execution completes. Do not modify or resubmit the same graph while it is running.
- A Future's `wait()` only waits; `get()` also propagates exceptions. Inside a Worker, prefer cooperative waiting through Runtime, SubFlow, or TaskGroup.
- `request_stop()` requests cooperative cancellation. Long-running tasks check `stop_requested()`; running threads are not forcibly interrupted.

---

## Performance Comparison

The table reports total elapsed time for 25 scenarios in milliseconds; lower is better. Speedup = Taskflow time / TaskflowLite time. A value above 1 means TaskflowLite has a lower recorded time.

> Every case in both original logs reports `actual=0 [FAIL]`. Speedups are calculated from the recorded times only; they are not correctness-validated performance results.

| ID | Scenario | Threads × count | TaskflowLite (ms) | Taskflow (ms) | Speedup |
|----|----------|-----------------|------------------:|--------------:|--------:|
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
| | Geometric mean (unvalidated) | | | | 1.97× |

`k` = 1,000 and `M` = 1,000,000. Counts for 10, 11, and 13 are internal loop iterations; the others are graph executions. Speedups are rounded to two decimal places; the geometric mean uses unrounded ratios.

The logs do not specify hardware, operating system, compiler and optimization flags, or either source revision. A formal comparison must record these details, pass correctness checks, and repeat measurements under the same environment and configuration.

See the [benchmark guide](benchmarks/README.md). First run `--smoke` with verification enabled. To measure without counter overhead, run both programs with `--no-verify`.

---

## Documentation

See [documentation](documentation/) for more information.

### Task Graph Visualization

Export D2 text with `flow.dump()`, then render it to SVG with D2. [View the full task graph](documentation/img/d2.svg).

<details>
<summary>Expand the D2 task graph preview</summary>

[![TaskflowLite D2 task graph: dependencies, branches, jumps, nested graphs, and semaphore annotations](documentation/img/d2.svg)](documentation/img/d2.svg)

</details>

## License

This project is licensed under the [MIT License](LICENSE).
