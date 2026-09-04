/// @file task_group.hpp
/// @brief 当前执行上下文上用于组织、停止和等待一组动态任务的栈绑定对象。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-08-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <utility>

#include "context.hpp"
#include "executor.hpp"
#include "async_future.hpp"
#include "work_factory_fwd.hpp"

namespace tfl {

/// @brief 在当前任务执行作用域内提交、停止并协作式等待一组动态任务。
///
/// TaskGroup 由执行期 `Context` 构造，并借用该 Context 所绑定的 Worker 和 Executor。
/// 内部 `AnchorWork` 以 Context 当前 Work 为父节点，统一跟踪组内任务的完成计数、
/// 停止域和异常状态。
///
/// `silent_async/async` 默认将 AnchorWork 所属 Topology 作为新任务的父 Topology；
/// 显式指定 `InheritTopology = false` 时只切断停止请求与异常传播使用的 Topology
/// 父链，任务仍以 AnchorWork 为父 Work、计入本组完成计数，并由 `wait()` 或析构统一等待。
///
/// TaskGroup 构造时从 Context 提取 Worker 和 Executor 的直接引用，不保存 Context
/// 本身，从而避免 silent_async/async/run 等调度热路径增加额外一级间接访问。
///
/// @warning 对象必须在传入 Context 所属任务回调和 Worker 线程内使用及销毁，
///          不得逃逸当前作用域或被多个线程并发访问。
class TaskGroup : public Immovable<TaskGroup> {
    friend class Runtime;
    friend class Executor;
    TFL_WORK_SUBCLASS_FRIENDS;

public:
    /// @brief 创建绑定指定执行上下文的任务组。
    ///
    /// 构造时从 Context 提取当前 Work、Worker 和 Executor，并建立独立 AnchorWork
    /// 作为所有组内任务的完成、停止及异常聚合锚点。
    ///
    /// @param context 当前任务执行上下文。
    /// @warning context 及其绑定的 Work、Worker 和 Executor 必须覆盖本 TaskGroup 生命周期。
    explicit TaskGroup(Context& context) noexcept;

    /// @brief 协作式等待组内尚未完成的任务，并在正常析构路径中传播已归档异常。
    ///
    /// 正常离开作用域时，析构函数先协作等待全部组内任务完成，再重新抛出锚点归档的异常；
    /// 若析构发生在其他异常引起的栈展开过程中，则仍等待全部组内任务完成，但不会再次
    /// 调用异常重抛逻辑，以避免析构期间出现第二个异常而触发 `std::terminate()`。
    ///
    /// @warning 必须在创建本对象的 Worker 线程和任务回调内析构。
    ~TaskGroup() noexcept(false);

    /// @brief Fire-and-forget 提交子图执行一次，并把其生命周期挂接到本组。
    /// @tparam InheritTopology 是否将 TaskGroup 锚点的 Topology 作为新任务的父 Topology，默认为 true。
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型。
    /// @tparam C 完成回调类型，默认不执行回调。
    /// @param gh 要执行的子图。
    /// @param cb 全部节点完成后调用的无参回调。
    /// @note 本函数立即返回；非拥有捕获必须存活到异步执行完成。
    template <bool InheritTopology = true, graph_holder Gh, callback C = noop_callback>
        requires capturable<C>
    void silent_async(Gh&& gh, C&& cb = C{});

    /// @brief Fire-and-forget 提交子图循环执行指定次数。
    /// @tparam InheritTopology 是否将 TaskGroup 锚点的 Topology 作为新任务的父 Topology，默认为 true。
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型。
    /// @tparam C 完成回调类型，默认不执行回调。
    /// @param gh 要执行的子图。
    /// @param num 循环次数。
    /// @param cb 全部循环完成后调用的无参回调。
    template <bool InheritTopology = true, graph_holder Gh, callback C = noop_callback>
        requires capturable<C>
    void silent_async(Gh&& gh, std::uint64_t num, C&& cb = C{});

    /// @brief Fire-and-forget 提交由谓词控制循环终止的子图。
    /// @tparam InheritTopology 是否将 TaskGroup 锚点的 Topology 作为新任务的父 Topology，默认为 true。
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型。
    /// @tparam P 无参且返回 bool 的终止谓词类型。
    /// @tparam C 完成回调类型，默认不执行回调。
    /// @param gh 要执行的子图。
    /// @param pred 每轮前调用；返回 true 时停止继续循环。
    /// @param cb 循环结束后调用的无参回调。
    template <bool InheritTopology = true, graph_holder Gh, predicate P, callback C = noop_callback>
        requires capturable<P, C>
    void silent_async(Gh&& gh, P&& pred, C&& cb = C{});

    /// @brief Fire-and-forget 执行普通 callable，不保存返回值。
    /// @tparam InheritTopology 是否将 TaskGroup 锚点的 Topology 作为新任务的父 Topology，默认为 true。
    /// @tparam T 满足 basic_invocable concept 的 callable 类型。
    /// @param task 要执行的 callable。
    template <bool InheritTopology = true, typename T>
        requires (basic_invocable<T> && capturable<T>)
    void silent_async(T&& task);

    /// @brief Fire-and-forget 执行可接收 `Runtime&` 的 callable。
    /// @tparam InheritTopology 是否将 TaskGroup 锚点的 Topology 作为新任务的父 Topology，默认为 true。
    /// @tparam T 满足 runtime_invocable concept 的 callable 类型。
    /// @param task 要执行的 callable；框架在调用时注入栈绑定 `Runtime&`。
    template <bool InheritTopology = true, typename T>
        requires (runtime_invocable<T> && capturable<T>)
    void silent_async(T&& task);

    /// @brief Fire-and-forget 执行可接收 `SubFlow&` 的 callable。
    /// @tparam InheritTopology 是否将 TaskGroup 锚点的 Topology 作为新任务的父 Topology，默认为 true。
    /// @tparam T 满足 subflow_invocable concept 的 callable 类型。
    /// @param task 要执行的 callable；框架在调用时注入栈绑定 `SubFlow&`。
    /// @warning callable 不得保存框架注入的 `SubFlow&`。
    template <bool InheritTopology = true, typename T>
        requires (subflow_invocable<T> && capturable<T>)
    void silent_async(T&& task);

    /// @brief 异步执行子图一次并返回可等待、可请求停止的结果通道。
    /// @tparam InheritTopology 是否将 TaskGroup 锚点的 Topology 作为新任务的父 Topology，默认为 true。
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型。
    /// @tparam Deps 前驱异步任务类型包。
    /// @param gh 要执行的子图。
    /// @param deps 可选前驱任务；所有未完成依赖解除后当前任务才进入调度队列。
    /// @return 与本次执行关联的 `AsyncFuture<void>`。
    /// @warning `InheritTopology` 为 true 时，TaskGroup 必须在返回句柄仍可能查询继承停止状态期间保持有效。
    template <bool InheritTopology = true, graph_holder Gh, async_future... Deps>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, Deps&&... deps);

    /// @brief 异步执行子图一次，在完成后调用回调并返回结果通道。
    /// @tparam InheritTopology 是否将 TaskGroup 锚点的 Topology 作为新任务的父 Topology，默认为 true。
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型。
    /// @tparam C 完成回调类型。
    /// @tparam Deps 前驱异步任务类型包。
    /// @param gh 要执行的子图。
    /// @param cb 全部节点完成后调用的无参回调。
    /// @param deps 可选前驱任务；所有未完成依赖解除后当前任务才进入调度队列。
    /// @return 与本次执行关联的 `AsyncFuture<void>`。
    /// @warning `InheritTopology` 为 true 时，TaskGroup 必须在返回句柄仍可能查询继承停止状态期间保持有效。
    template <bool InheritTopology = true, graph_holder Gh, callback C, async_future... Deps>
        requires capturable<C>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, C&& cb, Deps&&... deps);

    /// @brief 异步循环执行子图指定次数并返回结果通道。
    /// @tparam InheritTopology 是否将 TaskGroup 锚点的 Topology 作为新任务的父 Topology，默认为 true。
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型。
    /// @tparam Deps 前驱异步任务类型包。
    /// @param gh 要执行的子图。
    /// @param num 循环次数。
    /// @param deps 可选前驱任务；所有未完成依赖解除后当前任务才进入调度队列。
    /// @return 与本次执行关联的 `AsyncFuture<void>`。
    /// @warning `InheritTopology` 为 true 时，TaskGroup 必须在返回句柄仍可能查询继承停止状态期间保持有效。
    template <bool InheritTopology = true, graph_holder Gh, async_future... Deps>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, std::uint64_t num, Deps&&... deps);

    /// @brief 异步循环执行子图指定次数，在完成后调用回调并返回结果通道。
    /// @tparam InheritTopology 是否将 TaskGroup 锚点的 Topology 作为新任务的父 Topology，默认为 true。
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型。
    /// @tparam C 完成回调类型。
    /// @tparam Deps 前驱异步任务类型包。
    /// @param gh 要执行的子图。
    /// @param num 循环次数。
    /// @param cb 全部循环完成后调用的无参回调。
    /// @param deps 可选前驱任务；所有未完成依赖解除后当前任务才进入调度队列。
    /// @return 与本次执行关联的 `AsyncFuture<void>`。
    /// @warning `InheritTopology` 为 true 时，TaskGroup 必须在返回句柄仍可能查询继承停止状态期间保持有效。
    template <bool InheritTopology = true, graph_holder Gh, callback C, async_future... Deps>
        requires capturable<C>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, std::uint64_t num, C&& cb, Deps&&... deps);

    /// @brief 异步执行由谓词控制循环终止的子图并返回结果通道。
    /// @tparam InheritTopology 是否将 TaskGroup 锚点的 Topology 作为新任务的父 Topology，默认为 true。
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型。
    /// @tparam P 无参且返回 bool 的终止谓词类型。
    /// @tparam Deps 前驱异步任务类型包。
    /// @param gh 要执行的子图。
    /// @param pred 每轮前调用；返回 true 时停止继续循环。
    /// @param deps 可选前驱任务；所有未完成依赖解除后当前任务才进入调度队列。
    /// @return 与本次执行关联的 `AsyncFuture<void>`。
    /// @warning `InheritTopology` 为 true 时，TaskGroup 必须在返回句柄仍可能查询继承停止状态期间保持有效。
    template <bool InheritTopology = true, graph_holder Gh, predicate P, async_future... Deps>
        requires capturable<P>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, P&& pred, Deps&&... deps);

    /// @brief 异步执行由谓词控制循环终止的子图，在结束后调用回调并返回结果通道。
    /// @tparam InheritTopology 是否将 TaskGroup 锚点的 Topology 作为新任务的父 Topology，默认为 true。
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型。
    /// @tparam P 无参且返回 bool 的终止谓词类型。
    /// @tparam C 完成回调类型。
    /// @tparam Deps 前驱异步任务类型包。
    /// @param gh 要执行的子图。
    /// @param pred 每轮前调用；返回 true 时停止继续循环。
    /// @param cb 循环结束后调用的无参回调。
    /// @param deps 可选前驱任务；所有未完成依赖解除后当前任务才进入调度队列。
    /// @return 与本次执行关联的 `AsyncFuture<void>`。
    /// @warning `InheritTopology` 为 true 时，TaskGroup 必须在返回句柄仍可能查询继承停止状态期间保持有效。
    template <bool InheritTopology = true, graph_holder Gh, predicate P, callback C, async_future... Deps>
        requires capturable<P, C>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, P&& pred, C&& cb, Deps&&... deps);

    /// @brief 异步执行普通 callable 并保存其返回值。
    /// @tparam InheritTopology 是否将 TaskGroup 锚点的 Topology 作为新任务的父 Topology，默认为 true。
    /// @tparam T 满足 basic_invocable concept 的 callable 类型。
    /// @tparam Deps 前驱异步任务类型包。
    /// @param task 要执行的 callable。
    /// @param deps 可选前驱任务；所有未完成依赖解除后当前任务才进入调度队列。
    /// @return `AsyncFuture<R>`，其中 `R = basic_return_t<T>`。
    /// @warning `InheritTopology` 为 true 时，TaskGroup 必须在返回句柄仍可能查询继承停止状态期间保持有效。
    template <bool InheritTopology = true, typename T, async_future... Deps>
        requires (basic_invocable<T> && capturable<T>)
    [[nodiscard]] auto async(T&& task, Deps&&... deps) -> AsyncFuture<basic_return_t<T>>;

    /// @brief 异步执行可接收 `Runtime&` 的 callable 并保存其返回值。
    /// @tparam InheritTopology 是否将 TaskGroup 锚点的 Topology 作为新任务的父 Topology，默认为 true。
    /// @tparam T 满足 runtime_invocable concept 的 callable 类型。
    /// @tparam Deps 前驱异步任务类型包。
    /// @param task 要执行的 callable。
    /// @param deps 可选前驱任务；所有未完成依赖解除后当前任务才进入调度队列。
    /// @return `AsyncFuture<R>`，其中 `R = runtime_return_t<T>`。
    /// @warning `InheritTopology` 为 true 时，TaskGroup 必须在返回句柄仍可能查询继承停止状态期间保持有效。
    template <bool InheritTopology = true, typename T, async_future... Deps>
        requires (runtime_invocable<T> && capturable<T>)
    [[nodiscard]] auto async(T&& task, Deps&&... deps) -> AsyncFuture<runtime_return_t<T>>;

    /// @brief 异步执行可接收 `SubFlow&` 的 callable 并保存其返回值。
    /// @tparam InheritTopology 是否将 TaskGroup 锚点的 Topology 作为新任务的父 Topology，默认为 true。
    /// @tparam T 满足 subflow_invocable concept 的 callable 类型。
    /// @tparam Deps 前驱异步任务类型包。
    /// @param task 要执行的 callable。
    /// @param deps 可选前驱任务；所有未完成依赖解除后当前任务才进入调度队列。
    /// @return `AsyncFuture<R>`，其中 `R = subflow_return_t<T>`。
    /// @warning callable 不得保存框架注入的 `SubFlow&`。
    /// @warning `InheritTopology` 为 true 时，TaskGroup 必须在返回句柄仍可能查询继承停止状态期间保持有效。
    template <bool InheritTopology = true, typename T, async_future... Deps>
        requires (subflow_invocable<T> && capturable<T>)
    [[nodiscard]] auto async(T&& task, Deps&&... deps) -> AsyncFuture<subflow_return_t<T>>;

    /// @brief 将子图源节点挂接到本组并立即提交。
    /// @tparam Gh 满足 graph_holder concept 的图持有者类型。
    /// @param gh 非拥有引用；Graph 及其节点必须存活到组内执行结束。
    /// @note 本函数只提交任务，不等待；需要同步并观察异常时调用 `wait()`。
    /// @note source 发布依赖 Executor 批量调度路径的发布语义；Graph 不得在执行完成前销毁或修改。
    template <graph_holder Gh>
    void run(Gh& gh);

    /// @brief 提交 AsyncTask，并建立其前置依赖后异步执行。
    /// @tparam InheritTopology 是否将 TaskGroup 锚点的 Topology 作为新任务的父 Topology，默认为 true。
    /// @tparam T 满足 async_task concept 的任务句柄类型。
    /// @tparam Deps 满足 async_future concept 的前置依赖类型。
    /// @param task 要执行的 AsyncTask。
    /// @param deps task 的前置依赖。
    /// @return 若 task 为左值则返回引用，否则按值返回移动后的句柄。
    /// @note 本函数只负责提交，不等待 task 完成。
    /// @warning `InheritTopology` 为 true 时，TaskGroup 必须在 task 仍可能查询继承停止状态期间保持有效。
    template <bool InheritTopology = true, async_task T, async_future... Deps>
    auto run(T&& task, Deps&&... deps) -> forward_return_t<T>;

    /// @brief 协作式等待本组所有未完成任务，并重新抛出归档异常。
    ///
    /// 等待期间当前 Worker 会继续执行自己的本地任务以及窃取其他 Worker
    /// 或共享队列中的任务，直到 AnchorWork 的 join_counter 归零；随后检查并
    /// 重新抛出锚点已经归档的异常。
    void wait();

    /// @brief 查询本组锚点或其父 Topology 链是否存在停止请求。
    /// @return 当前锚点或任一父停止域已请求停止时返回 true。
    [[nodiscard]] bool stop_requested() const noexcept;

    /// @brief 向本组锚点所属 Topology 发起协作式停止请求。
    /// @return 本次调用首次设置停止请求时返回 true，否则返回 false。
    /// @note 不等待，也不能强制中断正在运行的 callable。
    bool request_stop() noexcept;

    /// @brief 返回内部 AnchorWork 当前未完成计数的 relaxed 快照。
    /// @return 当前观察到的未完成工作计数。
    /// @note 该值是运行期 join 计数，不应解释为稳定的任务容器大小。
    [[nodiscard]] std::size_t size() const noexcept;

private:
    Worker& m_worker;
    Executor& m_executor;
    AnchorWork m_anchor;

    /// @brief 发布一个组内 SilentAsync Work，并维护 AnchorWork 的完成 slot。
    ///
    /// 发布前先为 AnchorWork 增加一个 join slot；若调度在 Work 尚未成功发布前
    /// 抛出异常，则撤销该 slot 并销毁尚未执行的 SilentAsync Work。
    ///
    /// @param work 已完成构造、尚未发布的 SilentAsync Work。
    /// @pre work 非空，且 `_schedule()` 抛异常时保证 work 尚未被调度器发布。
    void _launch_silent_async(Work* work);

    /// @brief 启动一个组内 Async Work，并根据动态前置依赖决定是否立即调度。
    ///
    /// 返回的 AsyncFuture 持有一份外部强引用，执行生命周期额外持有一份强引用；
    /// 同时为 AnchorWork 占用一个完成 slot。存在前置依赖时初始化目标 join_counter，
    /// 并将目标 Work 注册到尚未完成的前驱；所有依赖解除后才进入调度队列。
    ///
    /// @tparam R 子任务结果类型。
    /// @tparam Deps 前驱异步任务类型包。
    /// @param work 已完成构造、尚未发布的 Async Work。
    /// @param result 与 work 绑定的结果槽。
    /// @param deps 可选前驱任务。
    /// @return 与该任务关联的 AsyncFuture。
    /// @pre work 与 result 均非空，且 work 尚未被任何调度队列持有。
    template <typename R, async_future... Deps>
    [[nodiscard]] AsyncFuture<R> _launch_async(Work* work, ResultSlot<R>* result, const Deps&... deps);
};

// ============================================================================
// TaskGroup 构造与析构
// ============================================================================

inline TaskGroup::TaskGroup(Context& context) noexcept
    : m_worker{context.m_worker}
    , m_executor{context.m_executor}
    , m_anchor{context.m_work, context.m_executor} {}

inline TaskGroup::~TaskGroup() noexcept(false) {
    if (std::uncaught_exceptions() == 0) {
        wait();
    } else {
        m_executor._corun_until(m_worker, [this]() noexcept {
            return m_anchor.m_join_counter.load(std::memory_order_acquire) == 0;
        });
    }
}

// ============================================================================
// TaskGroup：内部提交
// ============================================================================

inline void TaskGroup::_launch_silent_async(Work* work) {
    TFL_ASSERT(work);

    // 每个组内任务占用 AnchorWork 的一个完成 slot；必须先计数再发布，
    // 避免任务在计数建立前快速完成并使锚点提前归零。
    m_anchor.m_join_counter.fetch_add(1, std::memory_order_relaxed);
    m_executor._schedule(m_worker, work);
}

template <typename R, async_future... Deps>
inline AsyncFuture<R> TaskGroup::_launch_async(Work* work, ResultSlot<R>* result, const Deps&... deps) {
    TFL_ASSERT(work);
    TFL_ASSERT(result);

    AsyncFuture<R> future{work, result};

    auto& control = work->m_topology->m_control;
    const auto current = control.load(std::memory_order_relaxed);

    TFL_ASSERT(Topology::Control::status(current) == Topology::Control::Status::Idle);
    TFL_ASSERT(!Topology::Control::locked(current));

    control.store(Topology::Control::set_status(current, Topology::Control::Status::Running), std::memory_order_relaxed);

    // Future 持有一份外部强引用；执行路径额外持有一份强引用，
    // 由 Async tear-down 或提交失败路径释放。
    work->_increment_ref();

    // 任务执行期间占用 AnchorWork 的一个完成 slot。
    m_anchor.m_join_counter.fetch_add(1, std::memory_order_relaxed);

    if constexpr (sizeof...(Deps) != 0) {
        std::array<Work*, sizeof...(Deps)> predecessors{deps.m_work...};
        std::size_t num_predecessors = sizeof...(Deps);

        work->m_join_counter.store(num_predecessors, std::memory_order_relaxed);
        m_executor._link_predecessors(work, predecessors.begin(), predecessors.end(), num_predecessors);

        if (num_predecessors != 0) {
            return future;
        }
    }

    m_executor._schedule(m_worker, work);
    return future;
}

// ============================================================================
// TaskGroup：fire-and-forget
// ============================================================================

template <bool InheritTopology, graph_holder Gh, callback C>
    requires capturable<C>
inline void TaskGroup::silent_async(Gh&& gh, C&& cb) {
    silent_async<InheritTopology>(std::forward<Gh>(gh), 1ULL, std::forward<C>(cb));
}

template <bool InheritTopology, graph_holder Gh, callback C>
    requires capturable<C>
inline void TaskGroup::silent_async(Gh&& gh, std::uint64_t num, C&& cb) {
    silent_async<InheritTopology>(std::forward<Gh>(gh), [num]() mutable noexcept -> bool { return num-- == 0; }, std::forward<C>(cb));
}

template <bool InheritTopology, graph_holder Gh, predicate P, callback C>
    requires capturable<P, C>
inline void TaskGroup::silent_async(Gh&& gh, P&& pred, C&& cb) {
    Topology* parent_topology = nullptr;

    if constexpr (InheritTopology) {
        parent_topology = m_anchor.m_topology;
        TFL_ASSERT(parent_topology);
    }

    Work* work = make_silent_async_module(m_executor, std::addressof(m_anchor), parent_topology, std::forward<Gh>(gh), std::forward<P>(pred), std::forward<C>(cb));
    _launch_silent_async(work);
}

template <bool InheritTopology, typename T>
    requires (basic_invocable<T> && capturable<T>)
inline void TaskGroup::silent_async(T&& task) {
    Topology* parent_topology = nullptr;

    if constexpr (InheritTopology) {
        parent_topology = m_anchor.m_topology;
        TFL_ASSERT(parent_topology);
    }

    Work* work = make_silent_async_basic(m_executor, std::addressof(m_anchor), parent_topology, std::forward<T>(task));
    _launch_silent_async(work);
}

template <bool InheritTopology, typename T>
    requires (runtime_invocable<T> && capturable<T>)
inline void TaskGroup::silent_async(T&& task) {
    Topology* parent_topology = nullptr;

    if constexpr (InheritTopology) {
        parent_topology = m_anchor.m_topology;
        TFL_ASSERT(parent_topology);
    }

    Work* work = make_silent_async_runtime(m_executor, std::addressof(m_anchor), parent_topology, std::forward<T>(task));
    _launch_silent_async(work);
}

template <bool InheritTopology, typename T>
    requires (subflow_invocable<T> && capturable<T>)
inline void TaskGroup::silent_async(T&& task) {
    Topology* parent_topology = nullptr;

    if constexpr (InheritTopology) {
        parent_topology = m_anchor.m_topology;
        TFL_ASSERT(parent_topology);
    }

    Work* work = make_silent_async_subflow(m_executor, std::addressof(m_anchor), parent_topology, std::forward<T>(task));
    _launch_silent_async(work);
}

// ============================================================================
// TaskGroup：带结果通道的组内任务
// ============================================================================

template <bool InheritTopology, graph_holder Gh, async_future... Deps>
inline AsyncFuture<void> TaskGroup::async(Gh&& gh, Deps&&... deps) {
    return async<InheritTopology>(std::forward<Gh>(gh), 1ULL, noop_callback{}, std::forward<Deps>(deps)...);
}

template <bool InheritTopology, graph_holder Gh, callback C, async_future... Deps>
    requires capturable<C>
inline AsyncFuture<void> TaskGroup::async(Gh&& gh, C&& cb, Deps&&... deps) {
    return async<InheritTopology>(std::forward<Gh>(gh), 1ULL, std::forward<C>(cb), std::forward<Deps>(deps)...);
}

template <bool InheritTopology, graph_holder Gh, async_future... Deps>
inline AsyncFuture<void> TaskGroup::async(Gh&& gh, std::uint64_t num, Deps&&... deps) {
    return async<InheritTopology>(std::forward<Gh>(gh), [num]() mutable noexcept -> bool { return num-- == 0; }, noop_callback{}, std::forward<Deps>(deps)...);
}

template <bool InheritTopology, graph_holder Gh, callback C, async_future... Deps>
    requires capturable<C>
inline AsyncFuture<void> TaskGroup::async(Gh&& gh, std::uint64_t num, C&& cb, Deps&&... deps) {
    return async<InheritTopology>(std::forward<Gh>(gh), [num]() mutable noexcept -> bool { return num-- == 0; }, std::forward<C>(cb), std::forward<Deps>(deps)...);
}

template <bool InheritTopology, graph_holder Gh, predicate P, async_future... Deps>
    requires capturable<P>
inline AsyncFuture<void> TaskGroup::async(Gh&& gh, P&& pred, Deps&&... deps) {
    return async<InheritTopology>(std::forward<Gh>(gh), std::forward<P>(pred), noop_callback{}, std::forward<Deps>(deps)...);
}

template <bool InheritTopology, graph_holder Gh, predicate P, callback C, async_future... Deps>
    requires capturable<P, C>
inline AsyncFuture<void> TaskGroup::async(Gh&& gh, P&& pred, C&& cb, Deps&&... deps) {
    Topology* parent_topology = nullptr;

    if constexpr (InheritTopology) {
        parent_topology = m_anchor.m_topology;
        TFL_ASSERT(parent_topology);
    }

    auto [work, result] = make_async_module(m_executor, std::addressof(m_anchor), parent_topology, std::forward<Gh>(gh), std::forward<P>(pred), std::forward<C>(cb));
    return _launch_async(work, result, std::forward<Deps>(deps)...);
}

template <bool InheritTopology, typename T, async_future... Deps>
    requires (basic_invocable<T> && capturable<T>)
inline auto TaskGroup::async(T&& task, Deps&&... deps) -> AsyncFuture<basic_return_t<T>> {
    Topology* parent_topology = nullptr;

    if constexpr (InheritTopology) {
        parent_topology = m_anchor.m_topology;
        TFL_ASSERT(parent_topology);
    }

    auto [work, result] = make_async_basic(m_executor, std::addressof(m_anchor), parent_topology, std::forward<T>(task));
    return _launch_async(work, result, std::forward<Deps>(deps)...);
}

template <bool InheritTopology, typename T, async_future... Deps>
    requires (runtime_invocable<T> && capturable<T>)
inline auto TaskGroup::async(T&& task, Deps&&... deps) -> AsyncFuture<runtime_return_t<T>> {
    Topology* parent_topology = nullptr;

    if constexpr (InheritTopology) {
        parent_topology = m_anchor.m_topology;
        TFL_ASSERT(parent_topology);
    }

    auto [work, result] = make_async_runtime(m_executor, std::addressof(m_anchor), parent_topology, std::forward<T>(task));
    return _launch_async(work, result, std::forward<Deps>(deps)...);
}

template <bool InheritTopology, typename T, async_future... Deps>
    requires (subflow_invocable<T> && capturable<T>)
inline auto TaskGroup::async(T&& task, Deps&&... deps) -> AsyncFuture<subflow_return_t<T>> {
    Topology* parent_topology = nullptr;

    if constexpr (InheritTopology) {
        parent_topology = m_anchor.m_topology;
        TFL_ASSERT(parent_topology);
    }

    auto [work, result] = make_async_subflow(m_executor, std::addressof(m_anchor), parent_topology, std::forward<T>(task));
    return _launch_async(work, result, std::forward<Deps>(deps)...);
}

// ============================================================================
// TaskGroup::run
// ============================================================================

template <graph_holder Gh>
inline void TaskGroup::run(Gh& gh) {
    Graph& graph = detail::to_graph(gh);
    const std::size_t num_sources = m_executor._set_up_graph(graph, m_anchor);

    if (num_sources == 0) {
        return;
    }

    // 每个 source 占用 AnchorWork 的一个完成 slot。
    // 必须先建立完整计数再发布任务，避免 source 快速完成导致锚点提前归零。
    m_anchor.m_join_counter.fetch_add(num_sources, std::memory_order_relaxed);
    m_executor._schedule(m_worker, graph.begin(), num_sources);
}


template <bool InheritTopology, async_task T, async_future... Deps>
inline auto TaskGroup::run(T&& task, Deps&&... deps) -> forward_return_t<T> {
    task.template _start<InheritTopology>(m_anchor, m_worker, m_executor, std::forward<Deps>(deps)...);
    return std::forward<T>(task);
}

// ============================================================================
// TaskGroup::wait
// ============================================================================

inline void TaskGroup::wait() {
    m_executor._corun_until(m_worker, [this]() noexcept {
        return m_anchor.m_join_counter.load(std::memory_order_acquire) == 0;
    });

    m_anchor._rethrow_exception();
}

// ============================================================================
// TaskGroup 状态
// ============================================================================

inline bool TaskGroup::stop_requested() const noexcept {
    return m_anchor._stop_requested();
}

inline bool TaskGroup::request_stop() noexcept {
    return m_anchor._request_stop();
}

inline std::size_t TaskGroup::size() const noexcept {
    return m_anchor.m_join_counter.load(std::memory_order_relaxed);
}

} // namespace tfl
