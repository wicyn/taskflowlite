/// @file forward.hpp
/// @brief 框架架构总览 + 全类前向声明
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn
///
/// ============================================================================
///  taskflow-lite 架构总览
/// ============================================================================
///
/// 整套库由 **两条贯穿性线索** 组织 ——
///
/// **线索 1：动 vs 静的对偶（Static / Dynamic Duality）**
///   - 静态侧：`Flow` / `Task` —— 提交前编辑的 DAG 蓝图，可反复执行；
///   - 动态侧：`Executor` 动态提交接口 / `Runtime` / `AsyncTask` —— 提交期 / 执行期产生的任务；
///   - 两侧通过 `Executor` 这个唯一执行引擎落地。
///
/// **线索 2：抽象 vs 实现的纵向分层**
///   - 用户层：`Flow` / `Task` / `AsyncTask` / `DeferredAsyncTask` / `Runtime`（视图与句柄）
///   - 中间层：`Work` / `Topology` / `Graph`（核心数据结构）
///   - 实现层：`Worker` / `BoundedQueue` / `Notifier` / `SplitMix64`（并发原语）
///
/// 每层往下越靠近"操作系统语义"，往上越靠近"用户业务语义"。任意一层的实现
/// 替换（比如把 BoundedQueue 改成 ABP queue）都不会冲击上层 API —— 这是本库
/// 能在保持简洁外观的同时容纳这么多机制的原因。
///
/// ============================================================================
///  组件协作图
/// ============================================================================
///
/// @code
///                             ┌─────────────────┐
///                             │      Flow       │  ← 用户层蓝图（静态）
///                             └────────┬────────┘
///                                      │ 持有
///                                      ▼
///                             ┌─────────────────┐
///                             │      Graph      │  ← 节点容器（vec<Work*>）
///                             └────────┬────────┘
///                                      │ 持有
///                                      ▼
///    Task / TaskView ─────引用─────► Work（基类）
///    AsyncTask / DeferredAsyncTask              ▲
///                                               │ 派生
///                               ┌───────────────┼──────────────┐
///                               │  BasicWork    │   AnchorWork │
///                               │  RuntimeWork  │   GraphWork  │
///                               │  BranchWork   │   JumpWork   │  ...
///                               └───────────────┴──────────────┘
///                                               ▲
///                                               │ 派生
///                               ┌──────────── *Invoker（works.hpp）
///                               │ 持有 callable + args
///                               └───────────────┘
///
///    ┌─────────────────────────────┐  调度  ┌─────────────────────────────┐
///    │           Executor          │ ◄───► │       Worker × N            │
///    │  - 提交 API（4 套）          │        │  - BoundedQueue (LIFO)      │
///    │  - work-stealing 三阶段      │        │  - SplitMix64 RNG           │
///    │  - m_num_topologies         │        │  - cache-aligned terminate  │
///    │  - m_buffers (Unbounded)    │        └─────────────────────────────┘
///    │  - Notifier (wait/notify)   │
///    └──────────────┬──────────────┘
///                   │ 派生上下文
///         ┌─────────┴──────────┐
///         ▼                    ▼
///    ┌──────────────────┐   ┌──────────┐
///    │ Executor 动态 API │   │ Runtime  │  ← 动态调度上下文
///    │   (图外提交)      │   │  (图内派生) │
///    └────────┬─────────┘   └────┬─────┘
///             │                  │
///             └────── 派发 ───────┘
///                    ▼
///               AsyncTask        ← 引用计数句柄
///
///    ┌────────────────────────────────────────┐
///    │  横切关注点                             │
///    │  ─────────────                          │
///    │  TaskObserver        ← 任务级钩子（2 点）│
///    │  WorkerHandler       ← 线程级钩子（3 点）│
///    │  Semaphore           ← 任务级限流       │
///    │  Topology            ← 生命周期 + 引计数 │
///    │  ExplicitAnchorGuard ← 异常归档 RAII    │
///    └────────────────────────────────────────┘
///
///    ┌─────────────────────────────────────────┐
///    │  基础设施                                │
///    │  ────────                                │
///    │  BoundedQueue    ← Chase-Lev LIFO/FIFO   │
///    │  UnboundedQueue  ← 自适应扩容版           │
///    │  Notifier        ← 无锁两阶段 wait       │
///    │  SplitMix64      ← 12B PRNG             │
///    │  D2Renderer      ← 可视化（友元聚合）      │
///    │  Exception       ← source_location 自动 │
///    └─────────────────────────────────────────┘
/// @endcode
///
/// ============================================================================
///  快速索引（按使用层次）
/// ============================================================================
///
/// | 层次     | 头文件                  | 主要类型                                  |
/// |----------|------------------------|------------------------------------------|
/// | 用户 API | flow.hpp               | `Flow`                                   |
/// |          | task.hpp               | `Task` / `TaskView`                      |
/// |          | async_task.hpp         | `AsyncTask` / `DeferredAsyncTask`        |
/// |          | runtime.hpp            | `Runtime`                                |
/// |          | executor.hpp           | `Executor`                               |
/// | 节点能力 | branch.hpp             | `Branch` / `MultiBranch`                 |
/// |          | jump.hpp               | `Jump` / `MultiJump`                     |
/// |          | semaphore.hpp          | `Semaphore`                              |
/// |          | observer.hpp           | `TaskObserver`                           |
/// | 核心运行时| work.hpp              | `Work` / `ExplicitAnchorGuard`           |
/// |          | works.hpp              | `*Work` / `*Invoker` 家族                 |
/// |          | topology.hpp           | `Topology`                               |
/// |          | graph.hpp              | `Graph`                                  |
/// |          | worker.hpp             | `Worker` / `WorkerView` / `WorkerHandler` |
/// | 并发原语 | bounded_queue.hpp      | `BoundedQueue`                           |
/// |          | unbounded_queue.hpp    | `AtomicRingBuffer` / `UnboundedQueue`    |
/// |          | notifier.hpp           | `Notifier` / `Notifier::Waiter`          |
/// |          | random.hpp             | `SplitMix64`                             |
/// | 诊断可视 | exception.hpp          | `Exception` / `TraceException`           |
/// |          | d2_render.hpp          | `D2Renderer`                             |
/// | 基础工具 | utility.hpp            | `Immovable` / `MoveOnly` / `Located` ... |
/// |          | traits.hpp             | concepts + `pack`                        |
/// |          | enums.hpp              | `TaskType` / `Direction`                 |
/// |          | small_vector.hpp       | `SmallVector` (LLVM 移植)                 |
/// |          | unordered_dense.hpp    | (第三方哈希容器)                           |
/// |          | macros.hpp             | 跨平台编译器指令封装                        |
/// |          | work_factory*.hpp       | `make_*` 工厂 + `destroy`                |


#pragma once

namespace tfl {

class Flow;
class Task;
class TaskView;
class AsyncTask;
class DeferredAsyncTask;
class Runtime;
class Executor;
class Branch;
class MultiBranch;
class Jump;
class MultiJump;
class Semaphore;
class TaskObserver;
class Work;
class Graph;
class Worker;
class WorkerView;


// Work 子类前向声明
template <typename F, typename... Args>                             class BasicInvoker;
template <typename F, typename... Args>                             class BranchInvoker;
template <typename F, typename... Args>                             class MultiBranchInvoker;
template <typename F, typename... Args>                             class JumpInvoker;
template <typename F, typename... Args>                             class MultiJumpInvoker;
template <typename F, typename... Args>                             class RuntimeInvoker;
template <typename GhStore, typename P>                             class SubflowInvoker;
template <typename A, typename F, typename... Args>                 class SilentAsyncBasicInvoker;
template <typename A, typename F, typename... Args>                 class SilentAsyncRuntimeInvoker;
template <typename A, typename GhStore, typename P, typename C>     class SilentAsyncFlowInvoker;
template <typename A, typename F, typename R, typename... Args>     class AsyncBasicInvoker;
template <typename A, typename F, typename R, typename... Args>     class AsyncRuntimeInvoker;
template <typename A, typename GhStore, typename P, typename C>     class AsyncFlowInvoker;
template <typename A, typename F, typename... Args>                 class DepAsyncBasicInvoker;
template <typename A, typename F, typename... Args>                 class DepAsyncRuntimeInvoker;
template <typename A, typename GhStore, typename P, typename C>     class DepAsyncFlowInvoker;
template <typename A, typename F, typename... Args>                 class DepDeferredAsyncBasicInvoker;
template <typename A, typename F, typename... Args>                 class DepDeferredAsyncRuntimeInvoker;
template <typename A, typename GhStore, typename P, typename C>     class DepDeferredAsyncFlowInvoker;
template <typename A>                                               class AnchorWork;


#define TFL_WORK_SUBCLASS_FRIENDS                                                                                 \
    template <typename, typename...>                        friend class ::tfl::BasicInvoker;                     \
    template <typename, typename...>                        friend class ::tfl::BranchInvoker;                    \
    template <typename, typename...>                        friend class ::tfl::MultiBranchInvoker;               \
    template <typename, typename...>                        friend class ::tfl::JumpInvoker;                      \
    template <typename, typename...>                        friend class ::tfl::MultiJumpInvoker;                 \
    template <typename, typename...>                        friend class ::tfl::RuntimeInvoker;                   \
    template <typename, typename>                           friend class ::tfl::SubflowInvoker;                   \
    template <typename, typename, typename...>              friend class ::tfl::SilentAsyncBasicInvoker;          \
    template <typename, typename, typename...>              friend class ::tfl::SilentAsyncRuntimeInvoker;        \
    template <typename, typename, typename, typename>       friend class ::tfl::SilentAsyncFlowInvoker;           \
    template <typename, typename, typename, typename...>    friend class ::tfl::AsyncBasicInvoker;                \
    template <typename, typename, typename, typename...>    friend class ::tfl::AsyncRuntimeInvoker;              \
    template <typename, typename, typename, typename>       friend class ::tfl::AsyncFlowInvoker;                 \
    template <typename, typename, typename...>              friend class ::tfl::DepAsyncBasicInvoker;             \
    template <typename, typename, typename...>              friend class ::tfl::DepAsyncRuntimeInvoker;           \
    template <typename, typename, typename, typename>       friend class ::tfl::DepAsyncFlowInvoker;              \
    template <typename, typename, typename...>              friend class ::tfl::DepDeferredAsyncBasicInvoker;     \
    template <typename, typename, typename...>              friend class ::tfl::DepDeferredAsyncRuntimeInvoker;   \
    template <typename, typename, typename, typename>       friend class ::tfl::DepDeferredAsyncFlowInvoker;      \
    template <typename>                                     friend class ::tfl::AnchorWork;


} // namespace tfl
