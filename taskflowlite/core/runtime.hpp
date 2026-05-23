/// @file runtime.hpp
/// @brief 运行时调度上下文 - 任务执行期间的动态调度接口
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include "async_task.hpp"
#include "executor.hpp"
namespace tfl {

/// @brief 运行时调度上下文 —— 任务体内的动态调度代理。
///
/// @details
/// `Runtime` 是 **运行期** 注入到任务体内的能力句柄：当一个 `Work` 被
/// `Worker` 拣出执行时，调度器会构造一个 `Runtime` 实例并以引用形式传入
/// 用户的可调用对象。用户通过它在任务执行的过程中 **动态派生子任务**、
/// **激活兄弟任务**、**协作式等待** 子任务完成 —— 而无需提前知道这些
/// 操作的存在。它是框架"任务可以反过来命令调度器"的唯一通道。
///
/// 在框架的"静态 vs 动态"主线上，`Runtime` 与 `Flow` / `Executor` 动态提交接口形成
/// 能力光谱。三者都能让任务进入 `Executor`，区别在于 **构图时机** 与
/// **谁决定下一步**：
///
/// | 维度          | `Flow`              | `Executor` 动态任务        | `Runtime`            |
/// |---------------|---------------------|----------------------------|----------------------|
/// | 构图时机      | 提交 **前**（静态） | API 调用时（图外动态）      | 任务体 **内**（图内动态） |
/// | 调用者        | 用户主线程 / 外部线程 | 用户主线程 / 外部线程      | 任务体（被调度的代码）|
/// | 谁拥有节点    | Flow 自己           | Executor 内存池 / Topology | Executor 内存池 / Topology |
/// | 适用场景      | 结构稳定的批处理    | 外部线程的一次性派发        | 数据驱动的自适应分支  |
/// | 重复执行成本  | 极低（图复用）      | 一次性                     | 一次性                |
///
/// ============================================================================
///  注入机制与生命周期
/// ============================================================================
/// `Runtime` 是 **不可移动、不可拷贝**（继承自 `Immovable`），构造函数私有，
/// 仅 `Work` / `Flow` / `Executor` / `Worker` 友元可造。这套限制不是审美 ——
/// `Runtime` 持有三个对内部对象的引用：`m_work`（当前正在执行的节点）、
/// `m_worker`（执行线程）、`m_executor`（全局调度器）。一旦该任务调用结束，
/// 这些引用所指向的语境立刻失效，因此 `Runtime` 必须在调用栈帧内自动析构、
/// 不允许逸出。
///
/// 用户拿到的总是 `Runtime&`，由调度器的内部循环按值构造、按引用注入：
///
/// @code
///                        ┌───────────────────────┐
///                        │     Worker thread     │
///                        └────────────┬──────────┘
///                                     │ pop work
///                                     ▼
///                        ┌───────────────────────┐
///                        │   Runtime rt{w, ...}  │  ← 栈上构造
///                        └────────────┬──────────┘
///                                     │ 引用注入
///                                     ▼
///                        ┌───────────────────────┐
///                        │   user task body      │
///                        │   void f(Runtime& rt) │
///                        │   { rt.silent_async   │
///                        │     (...);  ...     } │
///                        └────────────┬──────────┘
///                                     │ 返回
///                                     ▼
///                        ┌───────────────────────┐
///                        │   rt.~Runtime()       │  ← 栈展开自动析构
///                        └───────────────────────┘
/// @endcode
///
/// 派生子任务通过将 `m_work` 标记为父节点、`fetch_add` 提升其 `join_counter`
/// 来挂接：父节点的 tear_down 在所有子任务完成前不会触发，从而保证子任务
/// 引用的资源仍然存活。
///
/// ============================================================================
///  能力分组
/// ============================================================================
/// 公共接口按语义分四组，对应任务体内可能出现的四类需求：
///
///   1. **dependent_async** —— 派生 *有前驱依赖* 的子任务，前驱以
///      `AsyncTask` 句柄列表传入。返回的 `AsyncTask` 句柄可继续作为
///      下游任务的依赖，构成动态 DAG。
///
///   2. **silent_async** —— Fire-and-forget 派生：父节点持有 join 计数，
///      但调用方拿不到句柄。适合"我只关心父任务等到孩子完成、不关心
///      孩子结果"的场景。
///
///   3. **async** —— 派生 + `Future` 结果回收。运行成本比
///      `silent_async` 略高（需要 `promise/future` pair），仅在确实需要
///      返回值时使用。
///
///   4. **schedule / cowait*** —— 兄弟任务激活与协作式等待。`schedule`
///      要求目标 `Task` 与本 Runtime 共享同一非空父节点（兄弟约束，
///      违反抛 `Exception`）；`cowait()` 在等待期间不会阻塞 worker，
///      而是让本 worker 继续偷其他任务，回到这里时再检查谓词。
///
/// `stop_requested()` / `has_exception()` 暴露父节点的提前终止与异常
/// 状态，用于 long-running 任务里的协作式取消。
///
/// ============================================================================
///  使用示例
/// ============================================================================
/// @code
///   Flow flow;
///   flow.emplace([](Runtime& rt) {
///       // 数据驱动的动态分支：依据运行时大小决定派生几个子任务
///       const std::size_t shards = compute_shard_count();
///       for (std::size_t i = 0; i < shards; ++i) {
///           rt.silent_async([i]{ process_shard(i); });
///       }
///       rt.cowait();   // 协作式等待全部 shard 完成（不阻塞 worker）
///       finalize();
///   }).name("dynamic-fan-out");
/// @endcode
///
/// @see Flow            静态侧对偶 —— 提交前编辑的 DAG 蓝图
/// @see Executor        动态任务提交与调度入口
/// @see AsyncTask       Runtime 派生的子任务句柄类型
/// @see Executor        Runtime 背后的真正执行引擎


class Runtime : public Immovable<Runtime> {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    TFL_WORK_SUBCLASS_FRIENDS;
public:
    /// @brief 提交带依赖的子图任务 —— 在前驱 Deps 全部完成后启动 `gh`。
    template <anchor_tag A = anchor::explicit_t, typename Gh, typename... Deps>
        requires graph_holder<Gh> && (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...)
    [[nodiscard]] AsyncTask dependent_async(Gh&& gh, Deps&&... deps);

    /// @brief 提交带依赖和完成回调的子图任务。
    template <anchor_tag A = anchor::explicit_t, typename Gh, typename C, typename... Deps>
        requires (capturable<C> && graph_holder<Gh> && callback<C> &&
                 (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
    [[nodiscard]] AsyncTask dependent_async(Gh&& gh, C&& cb, Deps&&... deps);

    /// @brief 提交带依赖的子图任务 —— 循环执行 `num` 次。
    template <anchor_tag A = anchor::explicit_t, typename Gh, typename... Deps>
        requires graph_holder<Gh> && (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...)
    [[nodiscard]] AsyncTask dependent_async(Gh&& gh, std::uint64_t num, Deps&&... deps);

    /// @brief 提交带依赖和完成回调的子图任务 —— 循环执行 `num` 次。
    template <anchor_tag A = anchor::explicit_t, typename Gh, typename C, typename... Deps>
        requires (capturable<C> && graph_holder<Gh> && callback<C> &&
                 (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
    [[nodiscard]] AsyncTask dependent_async(Gh&& gh, std::uint64_t num, C&& cb, Deps&&... deps);

    /// @brief 提交带依赖和终止谓词的子图任务 —— 谓词返回 `true` 时停止循环。
    template <anchor_tag A = anchor::explicit_t, typename Gh, typename P, typename... Deps>
        requires (capturable<P> && graph_holder<Gh> && predicate<P> &&
                 (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
    [[nodiscard]] AsyncTask dependent_async(Gh&& gh, P&& pred, Deps&&... deps);

    /// @brief 提交带依赖、终止谓词和完成回调的子图任务。
    template <anchor_tag A = anchor::explicit_t, typename Gh, typename P, typename C, typename... Deps>
        requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C> &&
                 (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
    [[nodiscard]] AsyncTask dependent_async(Gh&& gh, P&& pred, C&& cb, Deps&&... deps);

    /// @brief 提交带依赖的子图任务 —— 通过迭代器范围指定前驱。
    template <anchor_tag A = anchor::explicit_t, typename Gh, typename P, typename C, std::input_iterator I, std::sentinel_for<I> S>
        requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C> &&
                 std::derived_from<std::iter_value_t<I>, AsyncTask>)
    [[nodiscard]] AsyncTask dependent_async(Gh&& gh, P&& pred, C&& cb, I first, S last);

    /// @brief 提交带依赖的普通任务 —— 在前驱 Deps 全部完成后执行。
    template <anchor_tag A = anchor::explicit_t, typename T, typename... Deps>
        requires (basic_invocable<T> && (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
    [[nodiscard]] AsyncTask dependent_async(T&& task, Deps&&... deps);

    /// @brief 提交带依赖的普通任务 —— 通过迭代器范围指定前驱。
    template <anchor_tag A = anchor::explicit_t, typename T, std::input_iterator I, std::sentinel_for<I> S>
        requires (basic_invocable<T> && std::derived_from<std::iter_value_t<I>, AsyncTask>)
    [[nodiscard]] AsyncTask dependent_async(T&& task, I first, S last);

    /// @brief 提交带依赖的运行时任务 —— 注入 `Runtime&` 上下文。
    template <anchor_tag A = anchor::explicit_t, typename T, typename... Deps>
        requires (runtime_invocable<T> && (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
    [[nodiscard]] AsyncTask dependent_async(T&& task, Deps&&... deps);

    /// @brief 提交带依赖的运行时任务 —— 通过迭代器范围指定前驱。
    template <anchor_tag A = anchor::explicit_t, typename T, std::input_iterator I, std::sentinel_for<I> S>
        requires (runtime_invocable<T> && std::derived_from<std::iter_value_t<I>, AsyncTask>)
    [[nodiscard]] AsyncTask dependent_async(T&& task, I first, S last);



    /// @brief 提交任务图执行一次
    template <anchor_tag A = anchor::none_t, typename Gh>
        requires graph_holder<Gh>
    void silent_async(Gh&& gh);

    /// @brief 提交任务图执行一次，完成后执行回调
    template <anchor_tag A = anchor::none_t, typename Gh, typename C>
        requires (capturable<C> && graph_holder<Gh> && callback<C>)
    void silent_async(Gh&& gh, C&& cb);

    /// @brief 提交任务图执行指定次数
    template <anchor_tag A = anchor::none_t, typename Gh>
        requires graph_holder<Gh>
    void silent_async(Gh&& gh, std::uint64_t num);

    /// @brief 提交任务图循环执行指定次数，完成后执行回调
    template <anchor_tag A = anchor::none_t, typename Gh, typename C>
        requires (capturable<C> && graph_holder<Gh> && callback<C>)
    void silent_async(Gh&& gh, std::uint64_t num, C&& cb);

    /// @brief 提交任务图条件循环执行
    template <anchor_tag A = anchor::none_t, typename Gh, typename P>
        requires (capturable<P> && graph_holder<Gh> && predicate<P>)
    void silent_async(Gh&& gh, P&& pred);

    /// @brief 提交任务图条件循环执行，完成后执行回调
    template <anchor_tag A = anchor::none_t, typename Gh, typename P, typename C>
        requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
    void silent_async(Gh&& gh, P&& pred, C&& cb);


    /// @brief Fire-and-forget 异步执行
    template <anchor_tag A = anchor::none_t, typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable_plain<T, Args...>)
    void silent_async(T&& task, Args&&... args);

    /// @brief Fire-and-forget 运行时任务
    template <anchor_tag A = anchor::none_t, typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable_plain<T, Args...>)
    void silent_async(T&& task, Args&&... args);


    /// @brief 异步执行任务图一次，返回 Future<void>
    template <anchor_tag A = anchor::explicit_t, graph_holder Gh>
    [[nodiscard]] Future<void> async(Gh&& gh);

    /// @brief 异步执行任务图一次，完成后执行回调，返回 Future<void>
    template <anchor_tag A = anchor::explicit_t, graph_holder Gh, typename C>
        requires (capturable<C> && callback<C>)
    [[nodiscard]] Future<void> async(Gh&& gh, C&& cb);

    /// @brief 异步执行任务图指定次数，返回 Future<void>
    template <anchor_tag A = anchor::explicit_t, graph_holder Gh>
    [[nodiscard]] Future<void> async(Gh&& gh, std::uint64_t num);

    /// @brief 异步执行任务图指定次数，完成后执行回调，返回 Future<void>
    template <anchor_tag A = anchor::explicit_t, graph_holder Gh, typename C>
        requires (capturable<C> && callback<C>)
    [[nodiscard]] Future<void> async(Gh&& gh, std::uint64_t num, C&& cb);

    /// @brief 异步执行任务图条件循环，返回 Future<void>
    template <anchor_tag A = anchor::explicit_t, graph_holder Gh, typename P>
        requires (capturable<P> && predicate<P>)
    [[nodiscard]] Future<void> async(Gh&& gh, P&& pred);

    /// @brief 异步执行任务图条件循环，完成后执行回调，返回 Future<void>
    template <anchor_tag A = anchor::explicit_t, graph_holder Gh, typename P, typename C>
        requires (capturable<P, C> && predicate<P> && callback<C>)
    [[nodiscard]] Future<void> async(Gh&& gh, P&& pred, C&& cb);

    /// @brief 异步执行并返回 Future
    template <anchor_tag A = anchor::explicit_t, typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    [[nodiscard]] auto async(T&& task, Args&&... args) -> Future<basic_return_t<T, Args...>>;

    /// @brief 异步执行运行时任务并返回 Future
    template <anchor_tag A = anchor::explicit_t, typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    [[nodiscard]] auto async(T&& task, Args&&... args) -> Future<runtime_return_t<T, Args...>>;

    /// @brief 检测当前 Runtime 上下文是否已收到停止请求。
    [[nodiscard]] bool stop_requested() const noexcept;

    /// @brief 检测当前 Runtime 上下文是否已记录异常。
    [[nodiscard]] bool has_exception() const noexcept;
    // ========================================================================
    //  同步执行 / 协作式等待
    // ========================================================================
    /// @brief 立即调度一个兄弟任务到当前 worker 的本地队列。
    void schedule(Task task);

    /// @brief 立即调度一个子图到当前 worker 的本地队列。
    template <typename Gh>
        requires graph_holder<Gh>
    void schedule(Gh& gh);

    /// @brief 协作式等待子图完成 —— 阻塞当前 worker 直到 `gh` 所有节点执行完毕。
    template <typename Gh>
        requires graph_holder<Gh>
    void cowait(Gh& gh);

    /// @brief 协作式等待 —— 阻塞当前 worker 直到所有已调度的子任务完成。
    void cowait();

    /// @brief 协作式等待 —— 阻塞直到 `pred()` 返回 `true`。
    template <predicate Pred>
    void cowait_until(Pred&& pred);

    // ========================================================================
    //  Worker 访问
    // ========================================================================
    /// @brief 获取所属 Worker 的可变引用。
    [[nodiscard]] Worker& worker() noexcept;
    /// @brief 获取所属 Worker 的只读引用。
    [[nodiscard]] const Worker& worker() const noexcept;

    /// @brief 获取所属 Executor 的可变引用。
    [[nodiscard]] Executor& executor() noexcept;
    /// @brief 获取所属 Executor 的只读引用。
    [[nodiscard]] const Executor& executor() const noexcept;
private:
    Work&       m_work;
    Worker&     m_worker;
    Executor&   m_executor;

    explicit Runtime(Work& work, Worker& wr, Executor& exec) noexcept
        : m_work{work}, m_worker{wr}, m_executor{exec} {}
};

// ============================================================================
// Implementation
// ============================================================================

template <anchor_tag A, typename Gh, typename... Deps>
    requires graph_holder<Gh> &&
             (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...)
inline AsyncTask Runtime::dependent_async(Gh&& gh, Deps&&... deps) {
    return dependent_async<A>(std::forward<Gh>(gh), 1ULL, std::forward<Deps>(deps)...);
}

template <anchor_tag A, typename Gh, typename C, typename... Deps>
    requires (capturable<C> && graph_holder<Gh> && callback<C> &&
             (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
inline AsyncTask Runtime::dependent_async(Gh&& gh, C&& cb, Deps&&... deps) {
    return dependent_async<A>(std::forward<Gh>(gh), 1ULL, std::forward<C>(cb),
                              std::forward<Deps>(deps)...);
}

template <anchor_tag A, typename Gh, typename... Deps>
    requires graph_holder<Gh> &&
             (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...)
inline AsyncTask Runtime::dependent_async(Gh&& gh, std::uint64_t num, Deps&&... deps) {
    return dependent_async<A>(std::forward<Gh>(gh), num, []() noexcept {},
                              std::forward<Deps>(deps)...);
}

template <anchor_tag A, typename Gh, typename C, typename... Deps>
    requires (capturable<C> && graph_holder<Gh> && callback<C> &&
             (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
inline AsyncTask Runtime::dependent_async(Gh&& gh, std::uint64_t num, C&& cb,
                                          Deps&&... deps) {
    return dependent_async<A>(
        std::forward<Gh>(gh),
        [num]() mutable noexcept { return num-- == 0; },
        std::forward<C>(cb),
        std::forward<Deps>(deps)...);
}

template <anchor_tag A, typename Gh, typename P, typename... Deps>
    requires (capturable<P> && graph_holder<Gh> && predicate<P> &&
             (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
inline AsyncTask Runtime::dependent_async(Gh&& gh, P&& pred, Deps&&... deps) {
    return dependent_async<A>(std::forward<Gh>(gh),
                              std::forward<P>(pred),
                              []() noexcept {},
                              std::forward<Deps>(deps)...);
}

template <anchor_tag A, typename Gh, typename P, typename C, typename... Deps>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C> &&
             (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
inline AsyncTask Runtime::dependent_async(Gh&& gh, P&& pred, C&& cb, Deps&&... deps) {
    std::array<AsyncTask, sizeof...(Deps)> arr{
        AsyncTask(static_cast<const AsyncTask&>(deps))...
    };
    return dependent_async<A>(
        std::forward<Gh>(gh),
        std::forward<P>(pred),
        std::forward<C>(cb),
        arr.begin(), arr.end()
        );
}

template <anchor_tag A, typename Gh, typename P, typename C, std::input_iterator I, std::sentinel_for<I> S>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C> &&
             std::derived_from<std::iter_value_t<I>, AsyncTask>)
inline AsyncTask Runtime::dependent_async(Gh&& gh, P&& pred, C&& cb, I first, S last) {
    Work* work = make_dep_async_flow<A>(
        *this, std::addressof(m_work),
        std::forward<Gh>(gh),
        std::forward<P>(pred),
        std::forward<C>(cb));

    m_work.m_join_counter.fetch_add(1, std::memory_order_relaxed);
    auto* topo = work->m_topology;
    topo->m_state.store(Topology::State::Running, std::memory_order_relaxed);
    topo->_incref();

    std::size_t num_predecessors = static_cast<std::size_t>(std::ranges::distance(first, last));
    work->m_join_counter.store(num_predecessors, std::memory_order_relaxed);

    m_executor._process_dependent_async(work, first, last, num_predecessors);

    if (num_predecessors == 0) {
        m_executor._schedule(m_worker, work);
    }

    return AsyncTask{work};
}

template <anchor_tag A, typename T, typename... Deps>
    requires (basic_invocable<T> &&
             (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
inline AsyncTask Runtime::dependent_async(T&& task, Deps&&... deps) {
    std::array<AsyncTask, sizeof...(Deps)> arr{
        AsyncTask(static_cast<const AsyncTask&>(deps))...
    };
    return dependent_async<A>(std::forward<T>(task), arr.begin(), arr.end());
}

template <anchor_tag A, typename T, std::input_iterator I, std::sentinel_for<I> S>
    requires (basic_invocable<T> && std::derived_from<std::iter_value_t<I>, AsyncTask>)
inline AsyncTask Runtime::dependent_async(T&& task, I first, S last) {
    Work* work = make_dep_async_basic<A>(
        *this, std::addressof(m_work),
        std::forward<T>(task));

    m_work.m_join_counter.fetch_add(1, std::memory_order_relaxed);
    auto* topo = work->m_topology;
    topo->m_state.store(Topology::State::Running, std::memory_order_relaxed);
    topo->_incref();

    std::size_t num_predecessors = static_cast<std::size_t>(std::ranges::distance(first, last));
    work->m_join_counter.store(num_predecessors, std::memory_order_relaxed);

    m_executor._process_dependent_async(work, first, last, num_predecessors);

    if (num_predecessors == 0) {
        m_executor._schedule(m_worker, work);
    }

    return AsyncTask{work};
}

template <anchor_tag A, typename T, typename... Deps>
    requires (runtime_invocable<T> &&
             (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
inline AsyncTask Runtime::dependent_async(T&& task, Deps&&... deps) {
    std::array<AsyncTask, sizeof...(Deps)> arr{
        AsyncTask(static_cast<const AsyncTask&>(deps))...
    };
    return dependent_async<A>(std::forward<T>(task), arr.begin(), arr.end());
}

template <anchor_tag A, typename T, std::input_iterator I, std::sentinel_for<I> S>
    requires (runtime_invocable<T> && std::derived_from<std::iter_value_t<I>, AsyncTask>)
inline AsyncTask Runtime::dependent_async(T&& task, I first, S last) {
    Work* work = make_dep_async_runtime<A>(
        *this, std::addressof(m_work),
        std::forward<T>(task));

    m_work.m_join_counter.fetch_add(1, std::memory_order_relaxed);
    auto* topo = work->m_topology;
    topo->m_state.store(Topology::State::Running, std::memory_order_relaxed);
    topo->_incref();

    std::size_t num_predecessors = static_cast<std::size_t>(std::ranges::distance(first, last));
    work->m_join_counter.store(num_predecessors, std::memory_order_relaxed);

    m_executor._process_dependent_async(work, first, last, num_predecessors);

    if (num_predecessors == 0) {
        m_executor._schedule(m_worker, work);
    }

    return AsyncTask{work};
}

template <anchor_tag A, typename Gh>
    requires graph_holder<Gh>
inline void Runtime::silent_async(Gh&& gh) {
    return silent_async<A>(std::forward<Gh>(gh), 1ULL);
}

template <anchor_tag A, typename Gh, typename C>
    requires (capturable<C> && graph_holder<Gh> && callback<C>)
inline void Runtime::silent_async(Gh&& gh, C&& cb) {
    return silent_async<A>(std::forward<Gh>(gh), 1ULL, std::forward<C>(cb));
}

template <anchor_tag A, typename Gh>
    requires graph_holder<Gh>
inline void Runtime::silent_async(Gh&& gh, std::uint64_t num) {
    return silent_async<A>(std::forward<Gh>(gh), num, []() noexcept {});
}

template <anchor_tag A, typename Gh, typename C>
    requires (capturable<C> && graph_holder<Gh> && callback<C>)
inline void Runtime::silent_async(Gh&& gh, std::uint64_t num, C&& cb) {
    // 将次数转换为谓词
    return silent_async<A>(std::forward<Gh>(gh)
                           ,[num]() mutable noexcept { return num-- == 0; }
                           ,std::forward<C>(cb));
}

template <anchor_tag A, typename Gh, typename P>
    requires (capturable<P> && graph_holder<Gh> && predicate<P>)
inline void Runtime::silent_async(Gh&& gh, P&& pred) {
    return silent_async<A>(std::forward<Gh>(gh)
                           ,std::forward<P>(pred)
                           ,[]() noexcept {});
}

template <anchor_tag A, typename Gh, typename P, typename C>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
inline void Runtime::silent_async(Gh&& gh, P&& pred, C&& cb) {
    Work* work = make_silent_async_flow<A>(
        std::addressof(m_work),
        std::forward<Gh>(gh),
        std::forward<P>(pred),
        std::forward<C>(cb));
    m_work.m_join_counter.fetch_add(1, std::memory_order_relaxed);
    m_executor._schedule(m_worker, work);
}

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable_plain<T, Args...>)
inline void Runtime::silent_async(T&& task, Args&&... args) {
    Work* work = make_silent_async_basic<A>(
        std::addressof(m_work),
        std::forward<T>(task), std::forward<Args>(args)...);
    m_work.m_join_counter.fetch_add(1, std::memory_order_relaxed);
    m_executor._schedule(m_worker, work);
}

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable_plain<T, Args...>)
inline void Runtime::silent_async(T&& task, Args&&... args) {
    Work* work = make_silent_async_runtime<A>(
        std::addressof(m_work),
        std::forward<T>(task), std::forward<Args>(args)...);
    m_work.m_join_counter.fetch_add(1, std::memory_order_relaxed);
    m_executor._schedule(m_worker, work);
}


template <anchor_tag A, graph_holder Gh>
inline Future<void> Runtime::async(Gh&& gh) {
    return async<A>(std::forward<Gh>(gh), 1ULL);
}

template <anchor_tag A, graph_holder Gh, typename C>
    requires (capturable<C> && callback<C>)
inline Future<void> Runtime::async(Gh&& gh, C&& cb) {
    return async<A>(std::forward<Gh>(gh), 1ULL, std::forward<C>(cb));
}

template <anchor_tag A, graph_holder Gh>
inline Future<void> Runtime::async(Gh&& gh, std::uint64_t num) {
    return async<A>(std::forward<Gh>(gh), num, []() noexcept {});
}

template <anchor_tag A, graph_holder Gh, typename C>
    requires (capturable<C> && callback<C>)
inline Future<void> Runtime::async(Gh&& gh, std::uint64_t num, C&& cb) {
    return async<A>(std::forward<Gh>(gh),
                    [num]() mutable noexcept { return num-- == 0; },
                    std::forward<C>(cb));
}

template <anchor_tag A, graph_holder Gh, typename P>
    requires (capturable<P> && predicate<P>)
inline Future<void> Runtime::async(Gh&& gh, P&& pred) {
    return async<A>(std::forward<Gh>(gh),
                    std::forward<P>(pred),
                    []() noexcept {});
}

template <anchor_tag A, graph_holder Gh, typename P, typename C>
    requires (capturable<P, C> && predicate<P> && callback<C>)
inline Future<void> Runtime::async(Gh&& gh, P&& pred, C&& cb) {
    std::promise<void> promise;
    std::future<void>  std_future = promise.get_future();

    Work* work = make_async_flow<A>(
        m_executor, std::addressof(m_work),
        std::forward<Gh>(gh),
        std::forward<P>(pred),
        std::forward<C>(cb),
        std::move(promise));

    // Why: 派发前抓 stop_source 拷贝——派发后 work 可能被其它 worker 跑完
    //      并 tear_down，work->m_topology 立即 UAF
    std::stop_source ss = work->m_topology->m_stop_source;

    // Why: join_counter +1 必须严格先于 _schedule，避免 worker 立即完成时
    //      触发父 -1 而 +1 尚未到位导致短暂下溢
    m_work.m_join_counter.fetch_add(1, std::memory_order_relaxed);

    m_executor._schedule(m_worker, work);

    return Future<void>{std::move(std_future), std::move(ss)};
}

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
inline auto Runtime::async(T&& task, Args&&... args) -> Future<basic_return_t<T, Args...>> {
    using R = basic_return_t<T, Args...>;

    std::promise<R> promise;
    std::future<R> std_future = promise.get_future();

    Work* work = make_async_basic<A>(
        m_executor, std::addressof(m_work),
        std::forward<T>(task), std::move(promise),
        std::forward<Args>(args)...);

    // Why: 必须在 _schedule 之前抓拷贝。派发后 work 可能被其它 worker
    //      立即执行并 tear_down，work->m_topology 变 use-after-free。
    //      stop_source 是 shared_ptr 语义，拷贝即共享 control block。
    std::stop_source ss = work->m_topology->m_stop_source;

    // Why: join_counter +1 必须严格先于 _schedule。派发后 worker 可能
    //      立即跑 work 并 tear_down 触发父 join_counter -1，若 +1 滞后则
    //      counter 短暂下溢、父 last-arriver 仲裁误判提前完成。
    m_work.m_join_counter.fetch_add(1, std::memory_order_relaxed);

    m_executor._schedule(m_worker, work);

    return Future<R>{std::move(std_future), std::move(ss)};
}

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
inline auto Runtime::async(T&& task, Args&&... args) -> Future<runtime_return_t<T, Args...>> {
    using R = runtime_return_t<T, Args...>;

    std::promise<R> promise;
    std::future<R> std_future = promise.get_future();

    Work* work = make_async_runtime<A>(
        m_executor, std::addressof(m_work),
        std::forward<T>(task), std::move(promise),
        std::forward<Args>(args)...);

    std::stop_source ss = work->m_topology->m_stop_source;

    m_work.m_join_counter.fetch_add(1, std::memory_order_relaxed);

    m_executor._schedule(m_worker, work);

    return Future<R>{std::move(std_future), std::move(ss)};
}


inline bool Runtime::stop_requested() const noexcept {
    return m_work._stop_requested();
}

inline bool Runtime::has_exception() const noexcept {
    return m_work._has_exception();
}

/// @brief 立即触发一个兄弟任务的执行。
///
/// @par 约束
///   - @p task 必须与本 Runtime 的 work 共享同一父节点（兄弟关系），
///     即两者归属同一隔离生命周期。
///   - 父节点不可为 @c nullptr —— 拒绝顶级孤立任务（无法占位）。
///   - 违反任一约束抛出 @c Exception。
inline void Runtime::schedule(Task task) {
    auto* w = task.m_work;

    // 兄弟关系校验：task 与 m_work 必须共享同一非空父节点
    if (!w->m_parent || w->m_parent != m_work.m_parent) {
        throw Exception("Task must be a sibling of this Runtime's work.");
    }

    // 重置依赖计数 —— 调用方约定 task 此刻可立即执行
    w->m_join_counter.store(0, std::memory_order_relaxed);

    // 父节点占位 —— 阻止父在 task 完成前 tear_down
    w->m_parent->m_join_counter.fetch_add(1, std::memory_order_acq_rel);

    // 投递到当前 Worker 队列
    m_executor._schedule(m_worker, w);
}

template <typename Gh>
    requires graph_holder<Gh>
inline void Runtime::schedule(Gh& gh) {
    auto& graph = detail::to_graph(detail::unwrap(gh));
    if (graph.empty()) {
        return;
    }
    m_executor._cowait_graph(m_worker, graph, std::addressof(m_work));
}


inline void Runtime::cowait() {
    auto pred = [this]() noexcept {
        return m_work.m_join_counter.load(std::memory_order_acquire) == 1;
    };
    if (pred()) {
        return;
    }
    {
        ExplicitAnchorGuard guard{std::addressof(m_work)};
        cowait_until(std::move(pred));
    }
    m_work._rethrow_exception();
}

template <predicate Pred>
inline void Runtime::cowait_until(Pred&& pred) {
    m_executor._cowait_until(m_worker, std::forward<Pred>(pred));
}

inline Worker& Runtime::worker() noexcept { return m_worker; }
inline const Worker& Runtime::worker() const noexcept { return m_worker; }

inline Executor& Runtime::executor() noexcept { return m_executor; }
inline const Executor& Runtime::executor() const noexcept { return m_executor; }
} // namespace tfl
