# TaskflowLite

[![Ubuntu](https://github.com/wicyn/taskflowlite/actions/workflows/ubuntu.yml/badge.svg?branch=main)](https://github.com/wicyn/taskflowlite/actions/workflows/ubuntu.yml)
[![Windows](https://github.com/wicyn/taskflowlite/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/wicyn/taskflowlite/actions/workflows/windows.yml)
[![macOS](https://github.com/wicyn/taskflowlite/actions/workflows/macos.yml/badge.svg?branch=main)](https://github.com/wicyn/taskflowlite/actions/workflows/macos.yml)
[![CodeQL](https://github.com/wicyn/taskflowlite/actions/workflows/codeql-analysis.yml/badge.svg?branch=main)](https://github.com/wicyn/taskflowlite/actions/workflows/codeql-analysis.yml)
[![CI Extras](https://github.com/wicyn/taskflowlite/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/wicyn/taskflowlite/actions/workflows/ci.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue?logo=cplusplus)](#build--integration)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Header Only](https://img.shields.io/badge/Header--Only-Yes-success)](#direct-header-inclusion)

[简体中文](README.md) · **English**

TaskflowLite (tfl) is a modern C++20, header-only task scheduling library inspired by [Taskflow](https://github.com/taskflow/taskflow), with dependency graphs, dynamic tasks, control flow, cooperative waiting, and shared asynchronous results.

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
12. [Migration and Caveats](#migration-and-caveats)
13. [License](#license)

## Quick Start

This guide describes source version **2.2.0** and uses **C++20**. Except for the complete minimal example, the C++ snippets are independent fragments for `main()`. They assume an existing `tfl::Executor executor(4)` and the standard headers noted alongside each example.

### Minimal Example

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

A runs first, B and C may run concurrently, and D runs after both finish. `get()` waits and propagates exceptions; a Future's `wait()` only waits. Each concurrent log message uses a separate `std::osyncstream` to prevent interleaving.

The current `utility.hpp` uses `std::memcpy` without directly including `<cstring>`. The example includes `<cstring>` first to work with this source snapshot; the library header still needs its own include.

### Tasks with Arguments

Bind application arguments with lambda captures or `std::bind_front` from `<functional>`. Arguments after the callable in `async(callable, ...)` are asynchronous dependencies, not application arguments.

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

### Repeated Execution and Completion Callbacks

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

The stop predicate is checked **before each iteration**: `true` stops execution, including zero iterations if the first check is true. A count of zero also skips the graph; exceptions or stop requests may reduce the number of iterations. The completion callback belongs to the entire submission, not each iteration. Submit the same `Flow` again only after its previous execution finishes.

---

## Building DAGs

### Creating and Editing Nodes

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

`Task::work(...)` can replace a callable or module graph; `placeholder()` lets you connect a node before assigning its work. Function pointers, function objects, and member functions can be adapted with `std::bind_front`. Edit structure, replace work, and register observers only while tasks are not executing.

### Dependencies and Graph Operations

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

`precede` returns its caller, so `a.precede(b).precede(c)` means **a → b and a → c**, not a → b → c. `linearize` also accepts a range of `Task` handles or an initializer list.

`Task` and `TaskView` are non-owning handles. They do not extend node lifetimes and become invalid after the corresponding `erase`, `clear`, or graph destruction. Erasure also disconnects dependency edges, so the entire operation is not generally O(1). Do not insert or erase nodes inside a traversal callback.

### Batch Argument Packs

`tfl::pack` is still supported: each pack expands into one valid `emplace` call, and the batch overload requires at least two packs. Bind application arguments before packing an ordinary callable; module packs may contain a count or predicate. This example uses `<utility>`.

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

## Task Types

| Type | Construction | Meaning |
|------|--------------|---------|
| Placeholder | `flow.placeholder()` | Dependency propagation only |
| Basic | `flow.emplace([] { ... })` | Ordinary callable; graph nodes do not expose Future results |
| Runtime | `flow.emplace([](tfl::Runtime& rt) { ... })` | Dynamic task submission and cooperative waiting |
| Branch / MultiBranch | Accept `Branch&` / `MultiBranch&` | Select one / multiple successors |
| Jump / MultiJump | Accept `Jump&` / `MultiJump&` | Force activation of one / multiple targets |
| Module | `flow.emplace(inner)` | Execute an existing graph |
| SubFlow | Accept `tfl::SubFlow&` | Build a dynamic graph during execution |

### Branch and MultiBranch

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

Indices start at zero and follow successor order established by `precede`. `branch(0)` is equivalent to `select(0)`; `select_if` accepts a predicate taking `TaskView`. Selecting nothing schedules no successor through that branch. A later single-target selection replaces the previous selection.

```cpp
tfl::Flow flow;
auto split = flow.emplace([](tfl::MultiBranch& branch) {
    branch.select(0, 2);
});
auto [a, b, c] = flow.emplace([] {}, [] {}, [] {});
split.precede(a, b, c);
executor.async(flow).get();
```

Multi-target controls also provide `select_all()`, `select_if(...)`, `unselect(...)`, and `reset()`. Branches may leave paths unexecuted; do not give a join node predecessor conditions that can never all be satisfied.

### Jump and MultiJump

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

Jump supports controlled loops and retries; selecting nothing activates no jump target. `MultiJump` is the multi-target version. A jump bypasses ordinary dependency activation conditions, so the target must be in a state that permits another execution. Do not re-enter a node while it is already running. Ordinary cyclic dependencies do not automatically become a valid retry graph.

### Module: Nesting an Existing Graph

```cpp
int count = 0;
tfl::Flow inner;
(void)inner.emplace([&count] { ++count; });

tfl::Flow outer;
(void)outer.emplace(inner, 3ULL);
executor.async(outer).get();
// count == 3
```

Lvalue graphs are borrowed and must outlive the module's last execution. An rvalue such as `std::move(inner)` transfers ownership into the node. Do not move the same graph twice or share one mutable graph between concurrently executing modules. Counts and predicates follow the same rules as top-level repeated execution.

### SubFlow: Building a Graph at Runtime

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

Call `SubFlow::run()` explicitly: returning after construction alone does not submit the graph. `wait()` cooperatively waits for submitted children so the callable can read their results; parent completion also waits for attached children.

The dynamic graph is cleared and rebuilt before the same dynamic node executes again, invalidating previous child handles. `SubFlow`, `Runtime`, and branch contexts are valid only inside the current callback on its Worker thread. Do not retain them for cross-thread use. See [Migration and Caveats](#migration-and-caveats) for the captureless SubFlow regression on MSVC.

---

## Runtime Dynamic Scheduling

### AsyncFuture: Shared Results

`Executor::async` returns an already-submitted `AsyncFuture<R>`. Copies share the same task and result rather than copying the task. `get()` does not consume the handle and may be called repeatedly. This example uses `<string>`.

```cpp
auto future = executor.async([] { return std::string("ready"); });
auto shared = future;
std::string value = shared.get();
const std::string& view = future.get();
// Keep a handle alive while using view.
(void)value;
(void)view;
```

| Operation | Semantics |
|-----------|-----------|
| `valid()` / `operator bool()` | Whether the handle refers to a task |
| `done()` / `running()` | Execution-state snapshots |
| `wait()` | Blocking wait; does not rethrow task exceptions |
| `get()` | Waits and propagates exceptions; value results return `const R&`, lvalue-reference results return the original reference, and `void` returns nothing |
| `has_exception()` | Whether an exception has been archived |
| `request_stop()` / `stop_requested()` | Request / query cooperative stopping |
| `reset()` | Releases this handle's strong reference; neither cancels nor waits |

A reference to a value result cannot outlive its result storage; copy the value when independent ownership is needed. Reference results require the original object to remain alive. Do not read a handle object while another thread resets, moves, or destroys that same object. Calling `get()` on an empty Future throws `tfl::Exception`.

### Future Dependencies

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

`sum` is scheduled only after both predecessors finish, so reading those results inside its callable does not occupy a Worker waiting for unfinished work. Dependencies do not automatically pass their results to the callable: capture handles and call `get()`. Any deferred tasks used as dependencies must be started separately.

### AsyncTask: Deferred Start

```cpp
tfl::AsyncTask first([] { return 21; });
tfl::AsyncTask second([first] { return first.get() * 2; });

executor.run(second, first);
executor.run(first);
int result = second.get();
// result == 42
```

`AsyncTask<R>` derives from `AsyncFuture<R>`, supports class template argument deduction, and can be configured with a name, observers, and semaphores before `run()`. Copies still share one task; each underlying task can be started successfully only once, including through copied handles.

The current implementation of `run(task, deps...)` only accepts `AsyncTask` predecessors internally. For existing `AsyncFuture` predecessors, use `async(callable, futures...)` as above. Do not wait for an unstarted deferred task unless another thread will start it.

### Runtime and Cooperative Waiting

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

This example needs `<functional>`. `Runtime::wait()` waits for children attached to the current parent; `wait_until(pred)` returns when the predicate is true. The Worker runs other ready work while waiting, but this does not eliminate circular dependencies, invalid lock ordering, or conditions that never become true.

Inside a Worker, do not replace cooperative waiting with blocking `get()` / `wait()` on unfinished Futures, and do not call executor-wide `wait_for_all()` to wait for yourself. If children borrow local data or local graphs, explicitly wait before those objects are destroyed. Implicit parent completion does not extend the lifetime of callable-local variables.

```cpp
executor.async([](tfl::Runtime& runtime) {
    tfl::Flow local;
    (void)local.emplace([] {});
    runtime.corun(local);
}).get();
```

`Runtime::run(graph)` submits and returns immediately, requiring a subsequent wait; `corun(graph)` submits and cooperatively waits. Runtime-spawned tasks always participate in parent completion. `async<false>` / `silent_async<false>` disconnect only the Topology parent chain for stopping and exceptions, not child-completion accounting.

### TaskGroup: Scoped Task Groups

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

`TaskGroup` is constructed from the current `Context` and provides `async`, `silent_async`, `run`, `wait`, `request_stop`, `stop_requested`, and `size`. Use and destroy it inside its creating callback on the same Worker.

Its destructor cooperatively waits and may propagate a group exception on normal scope exit; it does not throw a second exception during stack unwinding. Declare borrowed data before the group so the group is destroyed and finishes waiting first. Keep child handles that borrow the group's stop domain inside that scope; return result copies instead.

---

## Control Flow

### Exception Handling

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

This example needs `<stdexcept>`. The framework catches ordinary task exceptions and propagates them through the execution domain; the Future's `get()` rethrows them. `silent_async` has no result handle. Use `async` when failures must be observed, or handle exceptions explicitly inside the callable. `Executor::wait_for_all()` waits but does not provide exception results.

For local recovery, use a separate `TaskGroup` and catch outside its scope. To intercept child exceptions at `Runtime::wait()` or `SubFlow::wait()`, include `taskflowlite/core/scoped_exception_anchor.hpp`, construct `tfl::ScopedExceptionAnchor anchor(context)` **before** submitting children, and keep it alive until waiting finishes. Calling `wait()` alone does not establish a local exception anchor.

### Cooperative Cancellation

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

A stop request does not forcibly terminate a thread. Work that has not started may be skipped, while an already-running long task must check the stop state itself. Blocking I/O is not interrupted automatically. A cancelled task that produced no value cannot be treated as a successful value result; handle its exception or cancellation path. This example uses a `void` result and keeps the task handle alive until execution finishes.

### TaskObserver and WorkerHandler

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

This example needs `<atomic>`. Observer callbacks take `WorkerView` and must be `noexcept`; synchronize shared state yourself. Exceptions from logging or container allocations must not escape these callbacks.

`WorkerHandler` observes thread lifecycle through `on_start(Worker&) noexcept` and `on_stop(Worker&, const std::exception_ptr&) noexcept`. Register it with `Executor(handler, workers)` and keep the handler alive until Executor destruction finishes. It is not a task-exception recovery policy. See [30_worker_handler.cpp](examples/30_worker_handler.cpp) for a complete example.

---

## Resource Control

### Semaphore: Task-Level Concurrency Limits

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

When permits are unavailable, the task registers as a waiter and yields the Worker; a release reschedules waiting work. Use `.acquire(sem, count)` for multiple permits and chain calls for multiple semaphores. Keep semaphores alive until all tasks referring to them finish.

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

Call `reset` only when there are no waiters, no outstanding permits, and no concurrent acquire or release operations. It is not a live resizing API. Requests larger than capacity, or release tasks depending on tasks that cannot acquire permits, can still cause logical deadlock.

---

## Visualization

This example needs `<fstream>`. `Flow::dump` produces D2 text, and asynchronous handles also provide `dump`. Export while the graph structure is stable.

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

Render the output with the [D2 Playground](https://play.d2lang.com). Node and edge styles distinguish task types and dependency relationships.

![TaskflowLite DAG visualization](documentation/img/d2.svg)

---

## Build & Integration

### Requirements

- **Language standard**: C++20; C++23 is not required.
- **CMake**: 3.21 or newer.
- **Runtime dependencies**: the C++ standard library and threading support; the current CMake configuration also links `libatomic` on non-Windows, non-macOS platforms.
- **Library features**: Concepts, Ranges, `std::format`, atomic waiting, and related facilities; current headers also include `<stop_token>`. Concurrent-printing examples on this page use `<syncstream>`.
- The library is header-only. Building only the library or examples does not download Catch2 or Taskflow; test and benchmark dependencies are opt-in.

| Toolchain | Guidance |
|-----------|----------|
| GCC + libstdc++ | Use GCC / libstdc++ 13 or newer; libstdc++ added `std::format` in 13.1 |
| Clang | Check the selected standard library too; pair it with libstdc++ or libc++ providing the features above |
| MSVC | Use a Visual Studio 2022 toolset providing those C++20 features; see below for the MSVC 19.44 SubFlow regression |
| macOS | Repository CI uses Homebrew LLVM; match the compiler, libc++ headers, and runtime library |

These are toolchain selection requirements, not a claim that every version passes the full suite. See the [GCC status table](https://gcc.gnu.org/onlinedocs/libstdc++/manual/status.html) and [libc++ C++20 status table](https://libcxx.llvm.org/Status/Cxx20.html). A compiler name or `-std=c++20` alone does not establish standard-library completeness.

### CMake Build and Test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 4

cmake -S . -B build/test -DCMAKE_BUILD_TYPE=Release -DTFL_BUILD_TESTS=ON
cmake --build build/test --config Release --parallel 4
ctest --test-dir build/test -C Release --output-on-failure
```

Ninja and Unix Makefiles use `CMAKE_BUILD_TYPE`; multi-configuration generators such as Visual Studio use `--config` for builds and `-C` for CTest. Keep those configurations consistent. Top-level builds enable examples by default and disable tests and benchmarks; subproject builds disable examples by default.

With `TFL_BUILD_TESTS=ON`, the default setup registers 30 examples and one combined unit-test executable. Disabling examples leaves the combined test. Enabling benchmarks adds two smoke tests. Per-file unit tests are neither built nor registered by default, avoiding duplicate execution of the suite.

| CMake option | Default | Purpose |
|--------------|---------|---------|
| `TFL_BUILD_EXAMPLES` | ON at top level / OFF as subproject | Build examples |
| `TFL_BUILD_TESTS` | OFF | Build Catch2 tests and enable CTest |
| `TFL_BUILD_BENCHMARKS` | OFF | Build both benchmarks and obtain Taskflow |
| `TFL_SANITIZER` | OFF | OFF / ASAN / TSAN |
| `TFL_EXAMPLES_RUN_TARGETS` | OFF | Generate `run_example_<name>` targets |
| `TFL_EXAMPLES_EXCLUDE_FROM_ALL` | OFF | Exclude examples from the default build and automatic CTest registration |
| `TFL_TEST_PER_FILE_DEFAULT` | OFF | Build and register per-file tests by default |
| `TFL_TEST_RUN_TARGETS` | OFF | Generate per-file build-and-run targets |
| `TFL_CATCH2_LOCAL_PATH` | Empty | Directory containing both Catch2 amalgamated files |
| `TFL_CATCH2_REF` | devel | Ref used when downloading Catch2 |
| `TASKFLOW_LOCAL_PATH` | Empty | Directory containing `taskflow/taskflow.hpp` |

### Sanitizers

```bash
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug -DTFL_BUILD_TESTS=ON -DTFL_SANITIZER=ASAN
cmake --build build/asan --config Debug --parallel 4
ctest --test-dir build/asan -C Debug --output-on-failure

cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DTFL_BUILD_TESTS=ON -DTFL_SANITIZER=TSAN
cmake --build build/tsan --config Debug --parallel 4
ctest --test-dir build/tsan -C Debug --output-on-failure
```

Use separate build directories for ASAN and TSAN. The current GCC / Clang settings also enable UBSan. MSVC supports ASan but not this project's TSAN configuration. On Windows, make the matching toolset's `clang_rt.asan*.dll` available through `PATH` or beside every executable; tests and examples use different directories.

These options primarily apply to repository-internal targets; downstream projects must configure sanitizers themselves. Skipping failing cases or disabling detection is not a sanitizer pass.

### macOS

```bash
brew install llvm
LLVM_PREFIX="$(brew --prefix llvm)"
cmake -S . -B build/macos -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="$LLVM_PREFIX/bin/clang++" \
  -DCMAKE_EXE_LINKER_FLAGS="-L$LLVM_PREFIX/lib/c++ -Wl,-rpath,$LLVM_PREFIX/lib/c++"
cmake --build build/macos --parallel 4
```

Use a new build directory when changing toolchains. For Apple's bundled toolchain, check the standard-library features provided by that specific Xcode and SDK; upstream Clang versions do not establish Apple libc++ feature availability. Do not add `-latomic` on macOS.

### Offline Dependencies and Network Settings

```bash
cmake -S . -B build/offline -DCMAKE_BUILD_TYPE=Release \
  -DTFL_BUILD_TESTS=ON -DTFL_BUILD_BENCHMARKS=ON \
  -DTFL_CATCH2_LOCAL_PATH=/path/to/Catch2/extras \
  -DTASKFLOW_LOCAL_PATH=/path/to/taskflow
```

Replace the paths above. The Catch2 directory must contain both `catch_amalgamated.cpp` and `catch_amalgamated.hpp`. Without a valid explicit path, CMake checks `test/catch2/`, then `test/`, then downloads into the build directory. The default download ref is `devel`; use `TFL_CATCH2_REF` to pin a version. Benchmarks use local Taskflow or clone it automatically; reproducible experiments should pin the local dependency commit.

`TFL_GITHUB_MIRROR` defaults to `direct`; available choices are listed in the root CMake configuration. `TFL_GITHUB_PREFIX` sets a custom prefix, `TFL_GIT_PROXY` configures a Git proxy, `TFL_PARTIAL_CLONE` defaults to ON, and `TFL_UPDATE_DEPS` defaults to OFF. Mirrors are external services whose availability is controlled by their providers.

### Using the CMake Target

Choose one of the following integration methods. Replace `main.cpp` with your application's source; the subdirectory path must point to the TaskflowLite repository root.

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

Alternatively, use FetchContent. The `main` ref below follows the development branch; production builds should use a verified full commit hash or an existing release tag. A source version macro does not establish that a release tag exists.

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

### Installation and find_package

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

Configure the consumer with `-DCMAKE_PREFIX_PATH=/path/to/tfl-install`. The exported target carries include paths, the C++20 requirement, and threading/platform link dependencies. No separate TaskflowLite static library needs to be built.

### Direct Header Inclusion

Save the minimal example as `main.cpp` in the repository root. On Linux with GCC:

```bash
g++ -std=c++20 -O2 -pthread -I. main.cpp -o my_app -latomic
```

Header-only code still needs its platform link dependencies. On x86-64, configurations requiring 128-bit CAS may additionally need `-mcx16`; repository-internal targets set it on applicable platforms. Prefer the CMake target to reduce platform-specific setup.

---

## Project Structure

```text
taskflowlite/
├── taskflowlite/
│   ├── taskflowlite.hpp                 # Unified include and version
│   └── core/
│       ├── executor.hpp                # Scheduler
│       ├── flow.hpp / flow_builder.hpp # Graph ownership and construction
│       ├── task.hpp                    # Task / TaskView
│       ├── async_task.hpp              # Deferred tasks
│       ├── async_future.hpp            # Shared result handles
│       ├── runtime.hpp / subflow.hpp   # Dynamic scheduling and subgraphs
│       ├── task_group.hpp              # Scoped task groups
│       ├── scoped_exception_anchor.hpp # Local exception anchors
│       ├── branch.hpp / jump.hpp       # Branching and jumps
│       ├── semaphore.hpp / observer.hpp
│       ├── worker.hpp / context.hpp    # Workers and execution contexts
│       ├── work.hpp / work_factory.hpp / work_invokers.hpp
│       └── object_pool.hpp / bounded_queue.hpp / unbounded_queue.hpp
├── examples/                           # 30 standalone examples
├── test/                               # 31 test_*.cpp files, Catch2 v3
├── benchmarks/                         # Two benchmarks and shared verification
├── documentation/                      # Additional documentation and images
├── cmake/                              # Installed package configuration
├── .github/workflows/                  # Platform tests, CodeQL, CI Extras
├── CMakeLists.txt
├── README.md / README.en.md
└── LICENSE
```

---

## Benchmark Results

### Historical Measurements (Reference Only)

The following 25 measurements are retained from the previous README. The original record describes an empty-task comparison using the same hardware, thread counts, topology, and iterations. **These numbers were not remeasured after the current 2.2.0 API migration.** The original table does not pin both source commits and does not establish current-version performance across workloads.

**Originally reported environment:** Intel Core i7-9750H @ 2.60GHz (6C/12T), Windows 11, MSVC 2022 /O2.

| # | Scenario | Config | TaskflowLite | Taskflow | Speedup |
|--:|---------|------|--------:|------------:|------:|
| 01 | 32 parallel | 8 thr · 500k | 893 ms | 1321 ms | **1.48×** |
| 02 | 32 serial | 1 thr · 1M | 483 ms | 1223 ms | **2.53×** |
| 03 | Diamond DAG | 2 thr · 1M | 219 ms | 357 ms | **1.63×** |
| 04a | 4×2 full | 2 thr · 1M | 429 ms | 611 ms | **1.42×** |
| 04b | 6×4 full | 4 thr · 500k | 1435 ms | 1779 ms | **1.24×** |
| 04c | 8×8 full | 8 thr · 100k | 933 ms | 1258 ms | **1.35×** |
| 04d | 8×16 full | 8 thr · 50k | 1080 ms | 1496 ms | **1.39×** |
| 04e | 8×32 full | 8 thr · 20k | 1115 ms | 1627 ms | **1.46×** |
| 04f | 6×100 full | 8 thr · 2k | 548 ms | 715 ms | **1.30×** |
| 05 | Binary tree | 8 thr · 500k | 1349 ms | 2980 ms | **2.21×** |
| 06 | 1→256→1 fan | 8 thr · 100k | 3181 ms | 4096 ms | **1.29×** |
| 07 | 16 pipelines | 8 thr · 200k | 389 ms | 2452 ms | **6.30×** |
| 08 | 16×16 grid | 8 thr · 100k | 653 ms | 2722 ms | **4.17×** |
| 09 | Sparse DAG | 8 thr · 500k | 1815 ms | 3799 ms | **2.09×** |
| 10 | Jump loop | 1 thr · 1M | 25 ms | 50 ms | **2.00×** |
| 11 | MultiJump loop | 4 thr · 200k | 49 ms | 75 ms | **1.53×** |
| 12 | Subflow once | 4 thr · 200k | 130 ms | 183 ms | **1.41×** |
| 13 | Subflow loop | 2 thr · 500k | 94 ms | 159 ms | **1.69×** |
| 14 | Empty task | 1 thr · 10M | 406 ms | 633 ms | **1.56×** |
| 15 | Parallel for | 8 thr · 1024×10k | 580 ms | 1159 ms | **2.00×** |
| 16 | Reduce tree | 8 thr · 127×50k | 346 ms | 693 ms | **2.00×** |
| 17 | Scan chain | 1 thr · 128×100k | 170 ms | 488 ms | **2.87×** |
| 18 | Wavefront | 8 thr · 210×10k | 68 ms | 236 ms | **3.47×** |
| 19 | Heterogeneous | 8 thr · 18×100k | 746 ms | 873 ms | **1.17×** |
| 20 | Memory stress | 8 thr · 2000×500 | 786 ms | 1115 ms | **1.42×** |
| | **Geometric mean** | | | | **≈ 1.85×** |

Current benchmarks enable atomic-counter verification by default, so default timings include verification work and are not pure scheduling overhead. Both programs support `--smoke`, `--no-verify`, and `--help`. Use matching compilers, optimization flags, thread counts, dependency revisions, and arguments for comparisons. Pass a verified smoke run before measuring a full run with verification disabled. See [benchmarks/README.md](benchmarks/README.md).

---

## Examples & Tests

### Example Index

| File | Topic |
|------|-------|
| [01_basic_dag.cpp](examples/01_basic_dag.cpp) | Basic DAG |
| [02_parallel.cpp](examples/02_parallel.cpp) | Parallel tasks |
| [03_loop.cpp](examples/03_loop.cpp) | Repeated execution |
| [04_runtime.cpp](examples/04_runtime.cpp) | Runtime dispatch |
| [05_branch.cpp](examples/05_branch.cpp) | Conditional branches |
| [06_jump.cpp](examples/06_jump.cpp) | Jumps and retries |
| [07_semaphore.cpp](examples/07_semaphore.cpp) | Semaphore limits |
| [08_subflow.cpp](examples/08_subflow.cpp) | Module graphs |
| [09_pipeline.cpp](examples/09_pipeline.cpp) | Map-reduce pipeline |
| [10_dump.cpp](examples/10_dump.cpp) | D2 export |
| [11_flow_emplace.cpp](examples/11_flow_emplace.cpp) | Node creation and argument binding |
| [12_loop_workflow.cpp](examples/12_loop_workflow.cpp) | Looping workflows |
| [13_parallel_reduce.cpp](examples/13_parallel_reduce.cpp) | Parallel reduction |
| [14_async_task_chain.cpp](examples/14_async_task_chain.cpp) | Deferred task chains |
| [15_observer.cpp](examples/15_observer.cpp) | Observer timing |
| [16_error_handling.cpp](examples/16_error_handling.cpp) | Exception handling |
| [17_cancellation.cpp](examples/17_cancellation.cpp) | Cooperative cancellation |
| [18_pipeline_producer_consumer.cpp](examples/18_pipeline_producer_consumer.cpp) | Producer-consumer pipeline |
| [19_dependent_async.cpp](examples/19_dependent_async.cpp) | Asynchronous dependencies |
| [20_parallel_for_index.cpp](examples/20_parallel_for_index.cpp) | Index-partitioned parallelism |
| [21_observer_tracing.cpp](examples/21_observer_tracing.cpp) | Observer tracing |
| [22_state_machine.cpp](examples/22_state_machine.cpp) | State machine |
| [23_parallel_reduce.cpp](examples/23_parallel_reduce.cpp) | Static-partition reduction |
| [24_recursive_runtime.cpp](examples/24_recursive_runtime.cpp) | Recursive Runtime |
| [25_retry_backoff.cpp](examples/25_retry_backoff.cpp) | Retry backoff |
| [26_task_group.cpp](examples/26_task_group.cpp) | Scoped task groups |
| [27_dynamic_subflow.cpp](examples/27_dynamic_subflow.cpp) | Dynamic SubFlow |
| [28_task_editing.cpp](examples/28_task_editing.cpp) | Task editing |
| [29_async_future_results.cpp](examples/29_async_future_results.cpp) | Value, reference, and void results |
| [30_worker_handler.cpp](examples/30_worker_handler.cpp) | Worker lifecycle |

```bash
cmake --build build --config Release --target tfl_ex_01_basic_dag
cmake --build build --config Release --target run_all_examples
```

`run_all_examples` builds and runs all examples in sequence. With tests enabled, `ctest --test-dir build/test -C Release -L example --output-on-failure` runs the built examples.

### Unit Tests

```bash
ctest --test-dir build/test -C Release -L unit --output-on-failure
cmake --build build/test --config Release --target tfl_test_task
```

The 31 test files cover graph construction, tasks and Futures, the three submission contexts, branches and jumps, subgraphs, cancellation and exceptions, observers, queues, allocators, and stress scenarios. Select cases with Catch2 tags, for example by passing `"[subflow]"` to the appropriate executable. `tfl_test_task` builds only its corresponding test file; run it directly or enable `TFL_TEST_PER_FILE_DEFAULT=ON` to add per-file tests to CTest.

See [test/README.md](test/README.md) for coverage conventions and known core regressions. Full validation must not filter out regression tests.

### Executable Locations

| Program | Single-configuration generator | Visual Studio Release |
|---------|--------------------------------|-----------------------|
| Example | `<build>/bin/examples/01_basic_dag` | `<build>/bin/examples/Release/01_basic_dag.exe` |
| Combined tests | `<build>/bin/TaskflowLiteTest` | `<build>/bin/Release/TaskflowLiteTest.exe` |
| Per-file tests | `<build>/bin/tfl_test_task` | `<build>/bin/Release/tfl_test_task.exe` |
| Benchmark | `<build>/benchmarks/bench_taskflowlite` | `<build>/benchmarks/Release/bench_taskflowlite.exe` |

Replace `<build>` with your build directory. The Taskflow baseline executable is named `bench_taskflow` in the same benchmark directory.

### Running Benchmarks

```bash
cmake -S . -B build/bench -DCMAKE_BUILD_TYPE=Release -DTFL_BUILD_BENCHMARKS=ON
cmake --build build/bench --config Release --target bench_taskflowlite bench_taskflow --parallel 4

./build/bench/benchmarks/bench_taskflowlite --smoke
./build/bench/benchmarks/bench_taskflow --smoke

./build/bench/benchmarks/bench_taskflowlite --no-verify
./build/bench/benchmarks/bench_taskflow --no-verify
```

The run paths above are for single-configuration generators; adapt Windows paths using the table. `--smoke` preserves all scenario structures and caps repetitions at 3. It is for correctness checks, not performance ranking. With both tests and benchmarks enabled, use `ctest -L benchmark` to run the two smoke tests.

---

## Migration and Caveats

| Previous usage / pitfall | Current usage |
|--------------------------|---------------|
| `NonrepeatAsyncTask` | `AsyncTask<R>` or CTAD |
| `executor.submit(task)` | `executor.run(task)` |
| `detach(callable)` | `silent_async(callable)` |
| `cowait()` / `cowait_until(...)` | `Runtime::wait()` / `wait_until(...)` |
| `emplace(callable, business_args...)` | Captures or `std::bind_front`; module count/predicate overloads still exist |
| `tfl::pack{callable, business_args...}` | Bind the callable first; packs only expand valid `emplace` arguments |
| `Future` / `wait_for` / `share` | `AsyncFuture`, `done()`, and direct handle copying |
| `ResumeNever` / `ResumeAlways` | Future `get()` and explicit exception scopes |
| `on_before(TaskView)` | `on_before(WorkerView) noexcept`; likewise for `on_after` |
| Editing / clearing a running graph | Wait for completion before editing |
| Using `Semaphore::reset` for live resizing | Reset only without waiters, outstanding permits, or concurrent operations |

Current source and CI checks also require attention to the following:

- **MSVC SubFlow regression**: captureless SubFlow callables have reproduced an access violation in `Graph::clear()` on MSVC 19.44 / Windows x64, associated with `TFL_NO_UNIQUE_ADDRESS` layout. Two `[core-regression]` cases remain enabled by default; a filtered pass is not a full-suite pass. See the [test notes](test/README.md).
- **Self-contained headers on GCC**: `std::memcpy` needs `<cstring>` and `std::condition_variable` needs `<condition_variable>`. Fix the files using those facilities rather than relying on transitive standard-library includes.
- **Concurrent output and captures**: consistently use separate `std::osyncstream` objects or the same mutex for concurrent logging. Do not mix in unsynchronized concurrent output. Explicitly capture enclosing local constants when printing them, for example `[N] { std::osyncstream(std::cout) << N; }`.
- **Overflow in timing workloads**: use sufficiently wide integer types for loop accumulation. `volatile` does not prevent signed overflow. Avoid deprecated compound assignment on volatile objects in C++20.
- **Interpreting results**: use complete build, CTest, and sanitizer logs for the relevant commit. One passing example run does not establish freedom from races, and compiler warnings are not automatically CodeQL or sanitizer errors.

---

## License

[MIT License](LICENSE)
