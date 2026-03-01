# taskflowlite

> 一个高性能、现代 C++23 任务并行编程库，灵感来源于 [Taskflow](https://github.com/taskflow/taskflow)，专注于极致性能与零开销抽象。

---

## 目录

- [特性](#特性)
- [架构概览](#架构概览)
- [快速开始](#快速开始)
- [核心概念](#核心概念)
  - [Flow — 任务图](#flow--任务图)
  - [Task — 任务句柄](#task--任务句柄)
  - [Executor — 执行器](#executor--执行器)
  - [AsyncTask — 异步任务](#asynctask--异步任务)
  - [Runtime — 运行时动态任务](#runtime--运行时动态任务)
  - [Branch — 静态分支控制](#branch--静态分支控制)
  - [MultiBranch — 多路并行分发](#multibranch--多路并行分发)
  - [Jump — 静态回跳/跳转](#jump--静态回跳跳转)
  - [MultiJump — 多路跳转散射](#multijump--多路跳转散射)
  - [Context — 完整上下文](#context--完整上下文)
  - [Subflow — 嵌套子图](#subflow--嵌套子图)
  - [Semaphore — 信号量并发控制](#semaphore--信号量并发控制)
  - [WorkerHandler — Worker 生命周期](#workerhandler--worker-生命周期)
- [任务类型速查表](#任务类型速查表)
- [图可视化 dump](#图可视化-dump)
- [Executor API 参考](#executor-api-参考)
- [性能设计](#性能设计)
- [编译要求](#编译要求)
- [使用方式](#使用方式)

---

## 特性

- **单头文件**：仅需 `#include "taskflowlite/taskflowlite.hpp"`
- **DAG 任务图**：通过 `precede` / `succeed` 声明任务依赖，支持任意有向无环图
- **Work-Stealing 调度器**：每个 Worker 维护本地有界队列，空闲时从邻居窃取，最大化 CPU 利用率
- **丰富的任务类型**：Basic、Runtime、Branch、MultiBranch、Jump、MultiJump、Context、Subflow
- **Subflow 图中图**：子图可嵌套子图，支持指定重复执行次数
- **异步提交**：`submit` 返回 `AsyncTask`，支持延迟启动、固定次数重复、谓词控制循环、完成回调
- **信号量**：`Semaphore` 精确控制同时执行任务数，阻塞任务停车等待，唤醒时精准重调度
- **观察者**：`TaskObserver` 监听单个任务生命周期，`WorkerHandler` 监听 Worker 线程事件
- **图可视化**：`flow.dump(os)` 输出 JSON 格式任务图，配合 dagre-d3 HTML 渲染依赖关系
- **异常安全**：任务抛出的异常通过 Topology 聚合，统一在 `wait()` 时重新抛出
- **缓存友好**：Executor 成员按热路径访问频率排列，`m_num_topologies` 独占 cache line 避免 false sharing
- **C++23**：大量使用 Concepts、`std::move_only_function`、`std::atomic::wait/notify`

---

## 架构概览

```
┌─────────────────────────────────────────────────────────┐
│                        用户代码                          │
│  Flow  →  emplace(task)  →  Task  →  precede/succeed    │
│  Executor::submit(flow, N)  →  AsyncTask::start().wait() │
└────────────────────┬────────────────────────────────────┘
                     │ submit
┌────────────────────▼────────────────────────────────────┐
│                     Executor                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐               │
│  │ Worker 0 │  │ Worker 1 │  │ Worker N │               │
│  │ BoundedQ │  │ BoundedQ │  │ BoundedQ │  ← 本地队列   │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘               │
│       │   work-steal │             │                     │
│  ┌────▼─────────────▼─────────────▼──────┐              │
│  │         UnboundedQueueBucket           │ ← 共享队列   │
│  └────────────────────────────────────────┘              │
│  Notifier (高效 park / unpark)                           │
└─────────────────────────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│                    Work (内部节点)                        │
│  m_edges: [successors... | predecessors...]              │
│  m_join_counter, m_topology, m_observers?, m_semaphores? │
└─────────────────────────────────────────────────────────┘
```

---

## 快速开始

### Hello World

```cpp
#include "taskflowlite/taskflowlite.hpp"
#include <iostream>

int main() {
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);  // 4 个工作线程
    tfl::Flow flow;

    auto A = flow.emplace([] { std::cout << "Task A\n"; });
    auto B = flow.emplace([] { std::cout << "Task B\n"; });
    auto C = flow.emplace([] { std::cout << "Task C\n"; });

    A.precede(B, C);  // A 完成后，B 和 C 并行执行

    executor.submit(flow).start().wait();
}
```

### DAG 任务图

```cpp
tfl::Flow flow;

auto [A, B, C, D] = flow.emplace(
    [] { /* 初始化 */ },
    [] { /* 处理分支 1 */ },
    [] { /* 处理分支 2 */ },
    [] { /* 汇聚结果 */ }
);

// A → B → D
// A → C → D
A.precede(B, C);
D.succeed(B, C);

executor.submit(flow).start().wait();
```

### 重复执行

```cpp
// 执行 100 次
executor.submit(flow, 100).start().wait();

// 执行直到谓词为 true
int count = 0;
executor.submit(flow, [&]() noexcept {
    return ++count >= 50;
}).start().wait();
```

---

## 核心概念

### Flow — 任务图

`Flow` 是一个有向无环图（DAG），用于描述任务及其依赖关系。任务通过 `emplace` 加入图，返回 `Task` 句柄。

```cpp
tfl::Flow flow;

// 命名（用于 dump 可视化，节点标签显示此名称）
flow.name("MyFlow");

// 普通任务
auto t1 = flow.emplace([] { /* ... */ });
t1.name("task_name");

// 批量创建，结构化绑定
auto [init, process, cleanup] = flow.emplace(
    [] { /* 初始化 */ },
    [] { /* 处理 */ },
    [] { /* 清理 */ }
);
init.precede(process);
process.precede(cleanup);

// 输出任务图（JSON 格式）
flow.dump(std::cout);         // 输出到终端
std::ofstream f("g.json");
flow.dump(f);                 // 输出到文件
```

**Flow 的重要特性：**
- Flow 可重复提交，每次执行都从干净状态开始
- 同一个 Flow 实例不可并发提交（提交期间修改 Flow 是未定义行为）
- Flow 支持嵌套（Subflow），嵌套深度不限

---

### Task — 任务句柄

`Task` 是指向图内部节点的轻量句柄（类似指针语义），可以复制。`TaskView` 是只读视图，用于观察者回调。

```cpp
tfl::Task t = flow.emplace([] {});

// 命名（影响 dump 输出的节点标签）
t.name("my_task");

// 依赖关系
t.precede(other);      // t 完成后执行 other
t.succeed(other);      // other 完成后执行 t
t.precede(b, c, d);    // t 完成后执行 b、c、d（下标 0、1、2，Branch/Jump 引用此顺序）

// 查询
std::string  name     = t.name();
std::size_t  num_succ = t.num_successors();
std::size_t  num_pred = t.num_predecessors();
bool         valid    = t.valid();

// 信号量绑定
tfl::Semaphore sem(2);
t.acquire(sem);   // 执行前获取信号量
t.release(sem);   // 执行后释放信号量

// 观察者
auto obs = t.register_observer<MyObserver>(/* 构造参数 */);
t.unregister_observer(obs);
```

---

### Executor — 执行器

`Executor` 管理一组工作线程，每个线程持有本地 work-stealing 队列（`BoundedQueue`）。线程空闲时先从共享队列（`UnboundedQueueBucket`）获取任务，再尝试从邻居窃取。

```cpp
tfl::ResumeNever handler;
tfl::Executor executor(handler);      // 线程数 = hardware_concurrency
tfl::Executor executor(handler, 8);   // 指定 8 个工作线程
```

提交 Flow：

```cpp
AsyncTask t = executor.submit(flow);                     // 执行 1 次
AsyncTask t = executor.submit(flow, 100);                // 执行 100 次
AsyncTask t = executor.submit(flow, pred);               // 执行直到 pred() == true
AsyncTask t = executor.submit(flow, 10, callback);       // 执行 10 次 + 完成回调
AsyncTask t = executor.submit(flow, pred, callback);     // 谓词 + 回调
```

提交独立异步任务：

```cpp
auto fut = executor.async([] { return 42; });                   // 带返回值
auto fut = executor.async([](tfl::Runtime& rt) { return 0; }); // Runtime 版
executor.silent_async([] { /* 后台 fire-and-forget */ });
```

---

### AsyncTask — 异步任务

`submit` 返回 `AsyncTask`，任务在 `start()` 调用后才真正开始调度，实现"创建与执行分离"。

```cpp
auto task = executor.submit(flow, 100);
task.start();   // 启动
task.wait();    // 等待完成（内部异常在此重新抛出）

// 链式写法
executor.submit(flow, 50).start().wait();
```

`AsyncTask` 不可复制，仅可移动。`wait()` 是幂等的。

---

### Runtime — 运行时动态任务

任务函数签名包含 `tfl::Runtime&` 时，可在执行期间动态提交新任务或子图。

```cpp
auto t = flow.emplace([](tfl::Runtime& rt) {
    // 动态 fire-and-forget
    rt.silent_async([] { puts("dynamic"); });

    // 动态提交带返回值
    auto fut = rt.async([] { return 99; });

    // 动态提交子图（异步）
    tfl::Flow subflow;
    subflow.emplace([] { /* ... */ });
    rt.submit(subflow);

    // 协作式等待（不阻塞底层 worker 线程）
    rt.wait_until([&]() noexcept {
        return fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    });

    // 同步运行子图（内部协作调度，不阻塞 worker）
    tfl::Flow sync_flow;
    sync_flow.emplace([] { /* ... */ });
    rt.run(sync_flow);
});
t.name("runtime_task");
```

---

### Branch — 静态分支控制

`Branch` 在任务执行时决定哪条后继分支被激活，未被激活的后继**不参与调度**。后继下标按 `precede` 调用顺序从 0 开始。

```cpp
auto validate = flow.emplace([](tfl::Branch br) {
    bool ok = do_validate();
    br.allow(ok ? 0 : 1);  // 激活成功路径(0) 或失败路径(1)
});
validate.name("validate");

auto success = flow.emplace([] { puts("ok"); });   // index 0
auto failure = flow.emplace([] { puts("fail"); }); // index 1
validate.precede(success);
validate.precede(failure);
```

`Branch` 方法：

| 方法 | 说明 |
|------|------|
| `br.allow(i)` | 激活第 i 个后继，其余后继跳过 |
| `br.allow(i, j, ...)` | 激活多个指定后继 |
| `br.deny(i)` | 拒绝第 i 个后继（其余照常） |
| `br.deny_all()` | 拒绝所有后继 |

> dump 输出中，Branch 节点以菱形显示，激活边以实线标注，跳过边以虚线标注。

---

### MultiBranch — 多路并行分发

`MultiBranch` 可以在一个节点同时激活多条后继路径，所有被激活的后继**并行执行**。

```cpp
auto dispatch = flow.emplace([](tfl::MultiBranch mbr) {
    mbr.allow(0);  // pipeline_A
    mbr.allow(1);  // pipeline_B
    mbr.allow(2);  // pipeline_C
});
dispatch.name("dispatch");

dispatch.precede(pipeline_A);  // index 0
dispatch.precede(pipeline_B);  // index 1
dispatch.precede(pipeline_C);  // index 2
```

适用场景：扇出（fan-out）、多路并行数据处理、条件性多路分发。

---

### Jump — 静态回跳/跳转

`Jump` 允许任务在执行后"跳转"到某个后继节点，实现**循环/重试**逻辑（DAG 中的有限回边）。

```cpp
// 错误重试：log_err → retry(Jump) → normalize（回跳）
auto retry = flow.emplace([](tfl::Jump jmp) {
    jmp.to(0);   // 跳转到后继 index 0
});
retry.name("retry");

log_err.precede(retry);
retry.precede(normalize);  // index 0，形成重试环
```

`Jump` 方法：

| 方法 | 说明 |
|------|------|
| `jmp.to(i)` | 跳转到第 i 个后继，其余后继跳过 |

> **注意**：`Jump` 构成图中的有向环，需确保业务逻辑最终会终止，否则将无限循环。dump 输出中，Jump 回边以红色虚线标注。

---

### MultiJump — 多路跳转散射

`MultiJump` 同时触发多个跳转目标，实现**扇出**到多条路径，所有目标并行执行。

```cpp
auto scatter = flow.emplace([](tfl::MultiJump mjmp) {
    mjmp.to(0);  // write_DB
    mjmp.to(1);  // write_cache
    mjmp.to(2);  // write_file
    mjmp.to(3);  // push_MQ
});
scatter.name("scatter_output");

scatter.precede(out_db);     // index 0
scatter.precede(out_cache);  // index 1
scatter.precede(out_file);   // index 2
scatter.precede(out_queue);  // index 3
```

---

### Context — 完整上下文

`Context` 同时暴露 `Condition`（分支控制）和 `Runtime`（动态任务）两套接口，适合需要在同一任务中同时控制分支和动态提交的场景。

```cpp
auto t = flow.emplace([](tfl::Context& ctx) {
    ctx.condition().skip(1);               // 分支控制：跳过 index 1
    ctx.runtime().async([] { /* ... */ }); // 动态任务
});
```

---

### Subflow — 嵌套子图

将一个 `Flow` 作为参数传递给 `emplace`，该 Flow 成为当前图中的一个子图节点（Subflow）。子图可以多层嵌套，且可以指定重复执行次数。

```cpp
// 构建子图
tfl::Flow pipeline;
auto read    = pipeline.emplace([] {});  read.name("read");
auto process = pipeline.emplace([] {});  process.name("process");
auto write   = pipeline.emplace([] {});  write.name("write");
read.precede(process);
process.precede(write);

// 嵌入主图，执行 1 次
auto sub = flow.emplace(std::move(pipeline), 1);
sub.name("pipeline");

// 嵌入主图，重复执行 3 次
auto sub3 = flow.emplace(std::move(another_flow), 3);
sub3.name("pipeline_x3");
```

**图中图（多层嵌套）示例：**

```cpp
// 最内层 ETL
tfl::Flow etl;
auto extract   = etl.emplace([] {});               extract.name("extract");
auto transform = etl.emplace([](tfl::Runtime&){}); transform.name("transform");
auto load      = etl.emplace([] {});               load.name("load");
extract.precede(transform);
transform.precede(load);

// 中间层，嵌入 etl（重复 2 次）
tfl::Flow pipeline;
auto init  = pipeline.emplace([] {});              init.name("init");
auto inner = pipeline.emplace(std::move(etl), 2);  inner.name("ETL_inner");
auto done  = pipeline.emplace([] {});              done.name("done");
init.precede(inner);
inner.precede(done);

// 最外层，嵌入 pipeline（执行 1 次）
auto sub = flow.emplace(std::move(pipeline), 1);
sub.name("pipeline_B");
```

---

### Semaphore — 信号量并发控制

`Semaphore` 限制同时运行某类任务的数量。获取失败时，任务被"停车"到信号量内部等待队列，不占用 CPU，直到信号量被释放后精准唤醒。

```cpp
tfl::Semaphore gpu_sem(2);  // 最多 2 个任务同时使用 GPU

for (int i = 0; i < 10; ++i) {
    auto t = flow.emplace([i] { /* GPU 计算 */ });
    t.acquire(gpu_sem);  // 执行前获取
    t.release(gpu_sem);  // 执行后释放
}

executor.submit(flow).start().wait();
// 任何时刻最多 2 个 GPU 任务同时运行
```

```cpp
tfl::Semaphore sem(max_value);                  // 初始值 = max_value
tfl::Semaphore sem(max_value, current_value);   // 指定初始值

sem.value();              // 当前可用数
sem.max_value();          // 最大值
sem.reset();              // 重置为 max_value，清空等待队列
sem.reset(new_value);     // 重置为 new_value（饱和截断）
```

---

### WorkerHandler — Worker 生命周期

`WorkerHandler` 是抽象基类，用于监听和干预 Worker 线程的生命周期事件。

```cpp
class MyHandler : public tfl::WorkerHandler {
public:
    void on_start(tfl::WorkerView worker) override {
        printf("Worker %zu started\n", worker.id());
        // 可在此绑定线程亲和性、设置线程名称等
    }

    void on_stop(tfl::WorkerView worker) noexcept override {
        printf("Worker %zu stopped\n", worker.id());
    }

    // 返回 true = 继续运行，false = 终止该 Worker
    bool on_exception(tfl::WorkerView worker,
                      std::exception_ptr eptr) noexcept override {
        try { std::rethrow_exception(eptr); }
        catch (std::exception& e) {
            fprintf(stderr, "Worker %zu: %s\n", worker.id(), e.what());
        }
        return true;
    }
};

MyHandler handler;
tfl::Executor executor(handler, 4);
```

内置 Handler：

```cpp
tfl::ResumeNever handler;  // 异常发生时终止对应 Worker，不重试
```

---

## 任务类型速查表

| 函数签名 | 类型 | 分支控制 | 动态任务 | 说明 |
|---------|------|:--------:|:--------:|------|
| `[]()` | Basic | — | — | 普通任务，零开销 |
| `[](tfl::Runtime& rt)` | Runtime | — | ✓ | 运行时动态提交子任务/子图 |
| `[](tfl::Branch br)` | Branch | ✓ 单选 | — | 激活一条后继路径 |
| `[](tfl::MultiBranch mbr)` | MultiBranch | ✓ 多选 | — | 并行激活多条后继路径 |
| `[](tfl::Jump jmp)` | Jump | ✓ 跳转 | — | 跳转到指定后继（支持回边/循环） |
| `[](tfl::MultiJump mjmp)` | MultiJump | ✓ 多跳 | — | 并行跳转到多个后继（扇出散射） |
| `[](tfl::Context& ctx)` | Context | ✓ | ✓ | Branch + Runtime 全部能力 |
| `tfl::Flow` (Subflow) | Subflow | — | — | 将整个 Flow 嵌入当前节点，可重复 N 次 |

> `emplace` 通过 C++23 Concepts 自动推导任务类型，无需显式指定。

---

## 图可视化 dump

`Flow::dump(ostream)` 将任务图以 **JSON** 格式输出，节点按类型着色，可配合 dagre-d3 等前端库渲染为交互式 HTML 可视化图。

### 基本用法

```cpp
tfl::Flow flow;
flow.name("MyGraph");  // 命名后 dump 输出更清晰

// ... 构建任务图 ...

// 输出到终端
flow.dump(std::cout);

// 输出到文件
std::ofstream f("graph.json");
flow.dump(f);
```

### 节点颜色约定（dagre-d3 渲染）

| 颜色 | 节点类型 |
|------|---------|
| 白色/浅灰 | Basic 普通任务 |
| 浅蓝色 | Runtime 动态任务 |
| 蓝色菱形 | Branch / MultiBranch 分支节点 |
| 粉红色 | Jump / MultiJump 跳转节点（含回边） |
| 绿色边框 | Subflow 子图容器 |

### 完整示例：复杂嵌套管线可视化

```cpp
#include "taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <fstream>

int main() {
    tfl::Flow flow;

    // ── Stage 1: 数据采集链 ─────────────────────────────────────
    auto source    = flow.emplace([]{});  source.name("source");
    auto fetch     = flow.emplace([]{});  fetch.name("fetch_data");
    auto normalize = flow.emplace([]{});  normalize.name("normalize");
    source.precede(fetch);
    fetch.precede(normalize);

    // ── Stage 2: Branch 验证 + Jump 错误回跳重试 ───────────────
    auto validate = flow.emplace([](tfl::Branch br) {
        br.allow(0);   // 成功 → pre_dispatch；失败 → log_error
    });
    validate.name("validate");
    normalize.precede(validate);

    auto pre_dispatch = flow.emplace([]{});  pre_dispatch.name("pre_dispatch");
    auto log_err      = flow.emplace([]{});  log_err.name("log_error");
    validate.precede(pre_dispatch);  // index 0 — 成功路径
    validate.precede(log_err);       // index 1 — 失败路径

    // Jump 回跳：失败后重试
    auto retry = flow.emplace([](tfl::Jump jmp) {
        jmp.to(0);  // 跳回 normalize
    });
    retry.name("retry");
    log_err.precede(retry);
    retry.precede(normalize);        // index 0，形成重试环

    // ── Stage 3: MultiBranch 3 路并行分发 ──────────────────────
    auto dispatch = flow.emplace([](tfl::MultiBranch mbr) {
        mbr.allow(0); mbr.allow(1); mbr.allow(2);
    });
    dispatch.name("dispatch");
    pre_dispatch.precede(dispatch);

    // ── Stage 4: 三条并行 Subflow（含图中图）──────────────────

    // 管线 A：read → process → write
    tfl::Flow pipeline_a;
    {
        auto r = pipeline_a.emplace([]{});  r.name("read_A");
        auto p = pipeline_a.emplace([]{});  p.name("process_A");
        auto w = pipeline_a.emplace([]{});  w.name("write_A");
        r.precede(p);  p.precede(w);
    }
    auto sub_a = flow.emplace(std::move(pipeline_a), 1);
    sub_a.name("pipeline_A");

    // 管线 B：init → ETL内层(extract→transform→load, ×2) → done
    tfl::Flow etl_inner;
    {
        auto e = etl_inner.emplace([]{});              e.name("extract");
        auto t = etl_inner.emplace([](tfl::Runtime&){}); t.name("transform");
        auto l = etl_inner.emplace([]{});              l.name("load");
        e.precede(t);  t.precede(l);
    }
    tfl::Flow pipeline_b;
    {
        auto init  = pipeline_b.emplace([]{});                    init.name("init_B");
        auto inner = pipeline_b.emplace(std::move(etl_inner), 2); inner.name("ETL_inner");
        auto done  = pipeline_b.emplace([]{});                    done.name("done_B");
        init.precede(inner);  inner.precede(done);
    }
    auto sub_b = flow.emplace(std::move(pipeline_b), 1);
    sub_b.name("pipeline_B");

    // 管线 C：Runtime → Branch(ok/fail)
    tfl::Flow pipeline_c;
    {
        auto rt    = pipeline_c.emplace([](tfl::Runtime&){});        rt.name("dynamic_C");
        auto check = pipeline_c.emplace([](tfl::Branch br){ br.allow(0); }); check.name("check_C");
        auto ok    = pipeline_c.emplace([]{});  ok.name("ok_C");
        auto fail  = pipeline_c.emplace([]{});  fail.name("fail_C");
        rt.precede(check);
        check.precede(ok);    // index 0
        check.precede(fail);  // index 1
    }
    auto sub_c = flow.emplace(std::move(pipeline_c), 1);
    sub_c.name("pipeline_C");

    // MultiBranch → 三条并行管线
    dispatch.precede(sub_a);  // index 0
    dispatch.precede(sub_b);  // index 1
    dispatch.precede(sub_c);  // index 2

    // ── Stage 5: 汇聚 + Runtime 聚合 ───────────────────────────
    auto merge = flow.emplace([]{});  merge.name("merge_results");
    sub_a.precede(merge);
    sub_b.precede(merge);
    sub_c.precede(merge);

    auto aggregate = flow.emplace([](tfl::Runtime&){});
    aggregate.name("aggregate");
    merge.precede(aggregate);

    // ── Stage 6: MultiJump 散射输出到 4 路 ─────────────────────
    auto scatter = flow.emplace([](tfl::MultiJump mjmp) {
        mjmp.to(0); mjmp.to(1); mjmp.to(2); mjmp.to(3);
    });
    scatter.name("scatter_output");
    aggregate.precede(scatter);

    auto out_db    = flow.emplace([]{});  out_db.name("write_DB");
    auto out_cache = flow.emplace([]{});  out_cache.name("write_cache");
    auto out_file  = flow.emplace([]{});  out_file.name("write_file");
    auto out_queue = flow.emplace([]{});  out_queue.name("push_MQ");
    scatter.precede(out_db);     // index 0
    scatter.precede(out_cache);  // index 1
    scatter.precede(out_file);   // index 2
    scatter.precede(out_queue);  // index 3

    // ── Stage 7: 收尾 ──────────────────────────────────────────
    auto finalize = flow.emplace([]{});  finalize.name("finalize");
    auto cleanup  = flow.emplace([]{});  cleanup.name("cleanup");
    out_db.precede(finalize);
    out_cache.precede(finalize);
    out_file.precede(finalize);
    out_queue.precede(finalize);
    finalize.precede(cleanup);

    // 输出可视化 JSON
    flow.name("NestedDemo").dump(std::cout);
}
```

上述代码运行后，`flow.dump()` 输出的任务图渲染效果如下：

![NestedDemo 任务图](flow_diagram.svg)

> 💡 **交互式查看**：将下方 D2 代码粘贴到 [https://play.d2lang.com](https://play.d2lang.com) 即可在线渲染、缩放、交互浏览完整任务图。

<details>
<summary>📐 点击展开 D2 图表源码（可粘贴到 play.d2lang.com）</summary>

```d2
vars: {
  d2-config: {
    layout-engine: elk
  }
}

classes: {
  basic:       { style: { fill: "#ffffff"; stroke: "#888888"; border-radius: 6 } }
  runtime:     { style: { fill: "#d6eaff"; stroke: "#5599cc"; border-radius: 6 } }
  branch:      { shape: diamond; style: { fill: "#d0e8ff"; stroke: "#3377cc"; font-color: "#003399" } }
  multibranch: { shape: diamond; style: { fill: "#cce0ff"; stroke: "#1155bb"; font-color: "#003399" } }
  jump:        { shape: hexagon; style: { fill: "#ffd6d6"; stroke: "#cc3333"; font-color: "#990000" } }
  multijump:   { shape: hexagon; style: { fill: "#ffbbbb"; stroke: "#aa1111"; font-color: "#880000" } }
  subflow:     { style: { fill: "#e8f5e9"; stroke: "#4caf50"; font-color: "#2e7d32"; border-radius: 8 } }
  subflow_inner: { style: { fill: "#fff8e1"; stroke: "#ff9800"; font-color: "#e65100"; border-radius: 8 } }
}

# Stage 1
source.class: basic
fetch_data.class: basic
normalize.class: basic
source -> fetch_data
fetch_data -> normalize

# Stage 2: Branch + Jump 重试
validate: "validate\n[Branch]" {class: branch}
pre_dispatch.class: basic
log_error.class: basic
retry: "retry\n[Jump]" {class: jump}

normalize -> validate
validate -> pre_dispatch: "0 (ok)"   {style.stroke: "#2e7d32"; style.font-color: "#2e7d32"}
validate -> log_error:    "1 (fail)" {style.stroke: "#c62828"; style.font-color: "#c62828"; style.stroke-dash: 4}
log_error -> retry
retry -> normalize: "Jump ↑" {style.stroke: "#cc3333"; style.font-color: "#cc3333"; style.stroke-dash: 4}

# Stage 3: MultiBranch 分发
dispatch: "dispatch\n[MultiBranch]" {class: multibranch}
pre_dispatch -> dispatch

# Stage 4: 三条并行 Subflow
pipeline_A: "pipeline_A  ×1" {
  class: subflow
  read_A.class: basic
  process_A.class: basic
  write_A.class: basic
  read_A -> process_A -> write_A
}

pipeline_B: "pipeline_B  ×1" {
  class: subflow
  init_B.class: basic
  done_B.class: basic
  ETL_inner: "ETL_inner  ×2" {
    class: subflow_inner
    extract.class: basic
    transform: "transform\n[Runtime]" {class: runtime}
    load.class: basic
    extract -> transform -> load
  }
  init_B -> ETL_inner -> done_B
}

pipeline_C: "pipeline_C  ×1" {
  class: subflow
  dynamic_C: "dynamic_C\n[Runtime]" {class: runtime}
  check_C: "check_C\n[Branch]" {class: branch}
  ok_C.class: basic
  fail_C.class: basic
  dynamic_C -> check_C
  check_C -> ok_C:   "0 (ok)"
  check_C -> fail_C: "1 (fail)" {style.stroke-dash: 4}
}

dispatch -> pipeline_A: "0" {style.stroke: "#3377cc"; style.font-color: "#3377cc"}
dispatch -> pipeline_B: "1" {style.stroke: "#3377cc"; style.font-color: "#3377cc"}
dispatch -> pipeline_C: "2" {style.stroke: "#3377cc"; style.font-color: "#3377cc"}

# Stage 5: 汇聚
merge_results.class: basic
aggregate: "aggregate\n[Runtime]" {class: runtime}
pipeline_A -> merge_results
pipeline_B -> merge_results
pipeline_C -> merge_results
merge_results -> aggregate

# Stage 6: MultiJump 散射
scatter_output: "scatter_output\n[MultiJump]" {class: multijump}
write_DB.class: basic
write_cache.class: basic
write_file.class: basic
push_MQ.class: basic
aggregate -> scatter_output
scatter_output -> write_DB:    "0" {style.stroke: "#cc3333"; style.font-color: "#cc3333"}
scatter_output -> write_cache: "1" {style.stroke: "#cc3333"; style.font-color: "#cc3333"}
scatter_output -> write_file:  "2" {style.stroke: "#cc3333"; style.font-color: "#cc3333"}
scatter_output -> push_MQ:     "3" {style.stroke: "#cc3333"; style.font-color: "#cc3333"}

# Stage 7: 收尾
finalize.class: basic
cleanup.class: basic
write_DB -> finalize
write_cache -> finalize
write_file -> finalize
push_MQ -> finalize
finalize -> cleanup
```

</details>

### dump JSON 字段说明

| 字段 | 类型 | 说明 |
|------|------|------|
| `name` | string | Flow / 节点名称 |
| `nodes` | array | 节点列表 |
| `nodes[].id` | string | 节点唯一标识 |
| `nodes[].name` | string | 节点显示名称（`.name()` 设置的值） |
| `nodes[].type` | string | `basic` / `runtime` / `branch` / `multibranch` / `jump` / `multijump` / `subflow` |
| `edges` | array | 有向边列表 |
| `edges[].from` | string | 源节点 id |
| `edges[].to` | string | 目标节点 id |
| `edges[].index` | int | Branch/Jump 的后继下标 |
| `edges[].active` | bool | 是否为激活路径（渲染时区分实线/虚线） |
| `subflows` | array | 嵌套子图，递归相同结构 |

---

## Executor API 参考

### submit — 提交 Flow

```cpp
AsyncTask submit(Flow& flow);
AsyncTask submit(Flow&&);
AsyncTask submit(Flow&, std::uint64_t N);
AsyncTask submit(Flow&, Pred&&);                     // pred: () noexcept -> bool
AsyncTask submit(Flow&, std::uint64_t N, Callback&&);
AsyncTask submit(Flow&, Pred&&, Callback&&);
```

### submit — 提交独立异步任务

```cpp
AsyncTask submit(BasicCallable&&);
AsyncTask submit(RuntimeCallable&&);
```

### async — 带返回值的异步任务

```cpp
std::future<R> async(BasicCallable&&);
std::future<R> async(RuntimeCallable&&);
```

### silent_async — 无返回值后台任务

```cpp
void silent_async(BasicCallable&&);
void silent_async(RuntimeCallable&&);
```

### 查询

```cpp
std::size_t num_workers()    const noexcept;
bool        is_worker()      const noexcept;   // 当前线程是否是 worker
int         this_worker_id() const noexcept;   // -1 表示非 worker
WorkerView  worker_view(std::size_t id) const noexcept;
```

---

## 性能设计

### Work-Stealing 调度器

每个 Worker 持有固定容量的本地有界队列（`BoundedQueue<Work*, NF_DEFAULT_QUEUE_SIZE>`，默认 4096）。执行时：

1. 先从本地队列头部弹出（cache 友好的 LIFO）
2. 本地队列空 → 尝试从共享无界队列（`UnboundedQueueBucket`）获取
3. 仍为空 → 随机选择邻居 Worker 进行窃取（Xoshiro256** 随机数，避免偏向）
4. 仍为空 → 通过 `Notifier` 停车等待（`atomic::wait`，无忙轮询）

### 缓存友好的内存布局

Executor 成员按热路径访问频率排列：

```cpp
// cache line 1：steal 循环每次迭代都访问
const std::size_t m_num_workers;
const std::size_t m_num_queues;
std::vector<Worker> m_workers;

// 热路径调度数据
UnboundedQueueBucket<Work*> m_shared_queues;
Notifier m_notifier;

// 独占 cache line：高竞争原子变量，避免 false sharing
alignas(64) std::atomic<std::size_t> m_num_topologies{0};

// 冷数据（仅启动/关闭时访问）
WorkerHandler& m_handler;
unordered_dense::map<std::thread::id, Worker*> m_thread_worker_map;
```

### 边存储优化

`Work` 节点将前继和后继合并在单个 `std::vector<Work*>` 中，节省一次堆分配：

```
m_edges: [successor_0, successor_1, ..., | predecessor_0, predecessor_1, ...]
          <------- m_num_successors -----> <-------------- predecessors ------->
```

Branch / Jump / MultiBranch / MultiJump 的下标直接对应 `m_edges` 中后继的位置偏移，零额外查找开销。

### Semaphore 零开销停车

信号量阻塞时，任务通过侵入式链表停车（Work 节点内嵌 next 指针，零堆分配）。Release 时精准 handoff 给恰好能获取的等待者数量，不产生无效唤醒和重试。

### 条件编译异常处理

`emplace` 根据任务函数是否 `noexcept` 自动决定是否包裹 `try-catch`：

```cpp
if constexpr (std::is_nothrow_invocable_v<F, Args...>) {
    std::invoke(task, args...);   // 零开销，无 try-catch
} else {
    try {
        std::invoke(task, args...);
    } catch (...) {
        exe._process_exception(w);
    }
}
```

---

## 编译要求

| 项目 | 要求 |
|------|------|
| C++ 标准 | C++23 或更高 |
| 编译器 | GCC 12+、Clang 15+、MSVC 2022+ |
| 依赖 | 仅标准库 + `ankerl::unordered_dense`（已内嵌） |
| 平台 | Linux、macOS、Windows（x86-64、ARM64） |

推荐编译选项：

```bash
-std=c++20 -O2 -march=native
```

---

## 使用方式

### 单头文件集成

将 `taskflowlite/` 目录复制到项目中：

```cpp
#include "taskflowlite/taskflowlite.hpp"
```

### CMake

```cmake
add_subdirectory(taskflowlite)
target_link_libraries(your_target PRIVATE taskflowlite::taskflowlite)
```

或通过 `find_package`（安装后）：

```cmake
find_package(taskflowlite REQUIRED)
target_link_libraries(your_target PRIVATE taskflowlite::taskflowlite)
```

### 性能基准示例

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
                    prev.precede(curr);
        }
    }

    executor.submit(flow, 1).start().wait();   // 预热

    counter.store(0);
    auto t0 = std::chrono::high_resolution_clock::now();
    executor.submit(flow, ITERS).start().wait();
    auto t1 = std::chrono::high_resolution_clock::now();

    auto ns    = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    auto total = LAYERS * PER_LAYER * ITERS;
    printf("tasks: %d / %zu | total: %.2f ms | per-task: %ld ns\n",
           counter.load(), total, ns / 1e6, ns / (long)total);
}
```

---

## 许可证

MIT License

---

*taskflowlite — 为追求极致性能的 C++ 并行程序而生。*