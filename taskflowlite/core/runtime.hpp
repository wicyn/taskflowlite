/// @file runtime.hpp
/// @brief Runtime —— 任务执行期间的动态派生、子图调度与协作式等待上下文。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>

#include "context.hpp"
#include "task.hpp"
#include "async_future.hpp"
#include "work_factory_fwd.hpp"

namespace tfl {

/// @brief 为正在执行的 Work 提供动态派生任务、子图调度和协作式等待能力.
///
/// Runtime 由框架在 runtime callable 执行期间临时构造，并借用当前 `Work`、
/// `Worker` 与 `Executor`。通过 Runtime 派生的任务都会计入当前 Work 的
/// `join_counter`，从而保证父任务不会在其派生任务完成前结束.
///
/// `InheritTopology=true` 时，新建异步任务额外将当前 Work 的 Topology 作为
/// 父 Topology，用于继承停止请求和异常传播链路；设置为 false 仅切断该
/// Topology 父链，不改变新任务仍作为当前 Work 子任务参与生命周期计数的事实.
///
/// `wait()`、`wait_until()` 和 `corun()` 采用协作式等待：当前 Worker 在等待
/// 条件满足期间继续执行其他就绪任务，而不是阻塞工作线程.
///
/// @warning Runtime 仅在当前 runtime callable 调用期间有效，不得保存、跨线程
///          传递，也不得在 callable 返回后继续访问.

class Runtime final : public Context {
    friend class ScopedExceptionAnchor;
    template <typename> friend class AsyncTask;
    TFL_WORK_SUBCLASS_FRIENDS;

public:
    /// @brief 派生一个即发即弃的子图任务，并执行子图一次.
    /// @tparam InheritTopology 是否将当前 Work 的 Topology 作为新任务的父 Topology.
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型.
    /// @tparam C 完成回调类型，默认 noop_callback.
    /// @param gh 要捕获或借用并执行的子图.
    /// @param cb 子图全部完成后调用的无参回调.
    /// @note 新任务始终计入当前 Work 的 join_counter；InheritTopology 只控制 Topology 父链.
    template <bool InheritTopology = true, graph_holder Gh, callback C = noop_callback>
        requires capturable<C>
    void silent_async(Gh&& gh, C&& cb = C{});

    /// @brief 派生一个即发即弃的子图任务，并循环执行子图指定次数.
    /// @tparam InheritTopology 是否将当前 Work 的 Topology 作为新任务的父 Topology.
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型.
    /// @tparam C 完成回调类型，默认 noop_callback.
    /// @param gh 要捕获或借用并执行的子图.
    /// @param num 请求执行的循环次数.
    /// @param cb 全部循环结束后调用的无参回调.
    /// @note 新任务始终计入当前 Work 的 join_counter；InheritTopology 只控制 Topology 父链.
    template <bool InheritTopology = true, graph_holder Gh, callback C = noop_callback>
        requires capturable<C>
    void silent_async(Gh&& gh, std::uint64_t num, C&& cb = C{});

    /// @brief 派生一个即发即弃的子图任务，并由谓词控制子图循环.
    /// @tparam InheritTopology 是否将当前 Work 的 Topology 作为新任务的父 Topology.
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型.
    /// @tparam P 满足 predicate concept 的循环终止谓词类型.
    /// @tparam C 完成回调类型，默认 noop_callback.
    /// @param gh 要捕获或借用并执行的子图.
    /// @param pred 每轮执行前调用的终止谓词；返回 true 时结束循环.
    /// @param cb 循环结束后调用的无参回调.
    /// @note 新任务始终计入当前 Work 的 join_counter；InheritTopology 只控制 Topology 父链.
    template <bool InheritTopology = true, graph_holder Gh, predicate P, callback C = noop_callback>
        requires capturable<P, C>
    void silent_async(Gh&& gh, P&& pred, C&& cb = C{});

    /// @brief 派生一个即发即弃的普通 callable 子任务.
    /// @tparam InheritTopology 是否将当前 Work 的 Topology 作为新任务的父 Topology.
    /// @tparam T 满足 basic_invocable concept 的 callable 类型.
    /// @param task 要捕获并执行的 callable.
    /// @note 本函数立即返回，不提供结果访问句柄；任务仍计入当前 Work 的 join_counter.
    template <bool InheritTopology = true, typename T>
        requires (basic_invocable<T> && capturable<T>)
    void silent_async(T&& task);

    /// @brief 派生一个即发即弃、执行时接收 `Runtime&` 的子任务.
    /// @tparam InheritTopology 是否将当前 Work 的 Topology 作为新任务的父 Topology.
    /// @tparam T 满足 runtime_invocable concept 的 callable 类型.
    /// @param task 要捕获并执行的 callable.
    /// @note 子任务可通过框架注入的 Runtime 继续派生任务或进行协作式等待.
    template <bool InheritTopology = true, typename T>
        requires (runtime_invocable<T> && capturable<T>)
    void silent_async(T&& task);

    /// @brief 派生一个即发即弃、执行时接收 `SubFlow&` 的子任务.
    /// @tparam InheritTopology 是否将当前 Work 的 Topology 作为新任务的父 Topology.
    /// @tparam T 满足 subflow_invocable concept 的 callable 类型.
    /// @param task 要捕获并执行的 callable；框架在调用时注入栈绑定 `SubFlow&`.
    /// @note 本函数立即返回且不提供结果句柄.
    /// @warning callable 不得保存或使框架注入的 `SubFlow&` 逸出本次调用.
    template <bool InheritTopology = true, typename T>
        requires (subflow_invocable<T> && capturable<T>)
    void silent_async(T&& task);


    /// @brief 派生一个带结果通道的子图任务，并执行子图一次.
    /// @tparam InheritTopology 是否将当前 Work 的 Topology 作为新任务的父 Topology.
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型.
    /// @tparam Deps 前驱异步任务类型包.
    /// @param gh 要捕获或借用并执行的子图.
    /// @param deps 可选前驱任务；只有所有未完成依赖解除后当前任务才进入调度队列.
    /// @return 共享该任务完成状态和结果槽的 `AsyncFuture<void>`.
    template <bool InheritTopology = true, graph_holder Gh, async_future... Deps>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, Deps&&... deps);

    /// @brief 派生一个带结果通道的子图任务，并执行子图一次，在完成后调用回调.
    /// @tparam InheritTopology 是否将当前 Work 的 Topology 作为新任务的父 Topology.
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型.
    /// @tparam C 完成回调类型.
    /// @tparam Deps 前驱异步任务类型包.
    /// @param gh 要捕获或借用并执行的子图.
    /// @param cb 子图全部完成后调用的无参回调.
    /// @param deps 可选前驱任务；只有所有未完成依赖解除后当前任务才进入调度队列.
    /// @return 共享该任务完成状态和结果槽的 `AsyncFuture<void>`.
    template <bool InheritTopology = true, graph_holder Gh, callback C, async_future... Deps>
        requires capturable<C>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, C&& cb, Deps&&... deps);

    /// @brief 派生一个带结果通道的子图任务，并循环执行子图指定次数.
    /// @tparam InheritTopology 是否将当前 Work 的 Topology 作为新任务的父 Topology.
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型.
    /// @tparam Deps 前驱异步任务类型包.
    /// @param gh 要捕获或借用并执行的子图.
    /// @param num 请求执行的循环次数.
    /// @param deps 可选前驱任务；只有所有未完成依赖解除后当前任务才进入调度队列.
    /// @return 共享该任务完成状态和结果槽的 `AsyncFuture<void>`.
    template <bool InheritTopology = true, graph_holder Gh, async_future... Deps>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, std::uint64_t num, Deps&&... deps);

    /// @brief 派生一个带结果通道的子图任务，循环执行指定次数并在完成后调用回调.
    /// @tparam InheritTopology 是否将当前 Work 的 Topology 作为新任务的父 Topology.
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型.
    /// @tparam C 完成回调类型.
    /// @tparam Deps 前驱异步任务类型包.
    /// @param gh 要捕获或借用并执行的子图.
    /// @param num 请求执行的循环次数.
    /// @param cb 全部循环结束后调用的无参回调.
    /// @param deps 可选前驱任务；只有所有未完成依赖解除后当前任务才进入调度队列.
    /// @return 共享该任务完成状态和结果槽的 `AsyncFuture<void>`.
    template <bool InheritTopology = true, graph_holder Gh, callback C, async_future... Deps>
        requires capturable<C>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, std::uint64_t num, C&& cb, Deps&&... deps);

    /// @brief 派生一个带结果通道的子图任务，并由谓词控制子图循环.
    /// @tparam InheritTopology 是否将当前 Work 的 Topology 作为新任务的父 Topology.
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型.
    /// @tparam P 满足 predicate concept 的循环终止谓词类型.
    /// @tparam Deps 前驱异步任务类型包.
    /// @param gh 要捕获或借用并执行的子图.
    /// @param pred 每轮执行前调用的终止谓词；返回 true 时结束循环.
    /// @param deps 可选前驱任务；只有所有未完成依赖解除后当前任务才进入调度队列.
    /// @return 共享该任务完成状态和结果槽的 `AsyncFuture<void>`.
    template <bool InheritTopology = true, graph_holder Gh, predicate P, async_future... Deps>
        requires capturable<P>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, P&& pred, Deps&&... deps);

    /// @brief 派生一个带结果通道的子图任务，由谓词控制循环并在完成后调用回调.
    /// @tparam InheritTopology 是否将当前 Work 的 Topology 作为新任务的父 Topology.
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型.
    /// @tparam P 满足 predicate concept 的循环终止谓词类型.
    /// @tparam C 完成回调类型.
    /// @tparam Deps 前驱异步任务类型包.
    /// @param gh 要捕获或借用并执行的子图.
    /// @param pred 每轮执行前调用的终止谓词；返回 true 时结束循环.
    /// @param cb 循环结束后调用的无参回调.
    /// @param deps 可选前驱任务；只有所有未完成依赖解除后当前任务才进入调度队列.
    /// @return 共享该任务完成状态和结果槽的 `AsyncFuture<void>`.
    template <bool InheritTopology = true, graph_holder Gh, predicate P, callback C, async_future... Deps>
        requires capturable<P, C>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, P&& pred, C&& cb, Deps&&... deps);

    /// @brief 派生一个普通 callable 子任务，并返回可共享访问结果的 AsyncFuture.
    /// @tparam InheritTopology 是否将当前 Work 的 Topology 作为新任务的父 Topology.
    /// @tparam T 满足 basic_invocable concept 的 callable 类型.
    /// @tparam Deps 前驱异步任务类型包.
    /// @param task 要捕获并执行的 callable.
    /// @param deps 可选前驱任务；只有所有未完成依赖解除后当前任务才进入调度队列.
    /// @return `AsyncFuture<R>`，其中 `R = basic_return_t<T>`.
    /// @note 新任务计入当前 Work 的 join_counter，返回 Future 额外持有任务强引用.
    template <bool InheritTopology = true, typename T, async_future... Deps>
        requires (basic_invocable<T> && capturable<T>)
    [[nodiscard]] auto async(T&& task, Deps&&... deps) -> AsyncFuture<basic_return_t<T>>;

    /// @brief 派生一个执行时接收 `Runtime&` 的子任务，并返回结果 AsyncFuture.
    /// @tparam InheritTopology 是否将当前 Work 的 Topology 作为新任务的父 Topology.
    /// @tparam T 满足 runtime_invocable concept 的 callable 类型.
    /// @tparam Deps 前驱异步任务类型包.
    /// @param task 要捕获并执行的 callable.
    /// @param deps 可选前驱任务；只有所有未完成依赖解除后当前任务才进入调度队列.
    /// @return `AsyncFuture<R>`，其中 `R = runtime_return_t<T>`.
    /// @note 子任务可通过框架注入的 Runtime 继续进行动态调度.
    template <bool InheritTopology = true, typename T, async_future... Deps>
        requires (runtime_invocable<T> && capturable<T>)
    [[nodiscard]] auto async(T&& task, Deps&&... deps) -> AsyncFuture<runtime_return_t<T>>;

    /// @brief 派生一个执行时接收 `SubFlow&` 的子任务，并返回结果 AsyncFuture.
    /// @tparam InheritTopology 是否将当前 Work 的 Topology 作为新任务的父 Topology.
    /// @tparam T 满足 subflow_invocable concept 的 callable 类型.
    /// @tparam Deps 前驱异步任务类型包.
    /// @param task 要捕获并执行的 callable；框架在调用时注入栈绑定 `SubFlow&`.
    /// @param deps 可选前驱任务；只有所有未完成依赖解除后当前任务才进入调度队列.
    /// @return `AsyncFuture<R>`，其中 `R = subflow_return_t<T>`.
    /// @warning callable 不得保存或使框架注入的 `SubFlow&` 逸出本次调用.
    template <bool InheritTopology = true, typename T, async_future... Deps>
        requires (subflow_invocable<T> && capturable<T>)
    [[nodiscard]] auto async(T&& task, Deps&&... deps) -> AsyncFuture<subflow_return_t<T>>;

    // ============================================================================
    // 执行 / 协作式等待
    // ============================================================================

    /// @brief 将子图挂接到当前 Work，并调度其所有物理零入度源节点后立即返回.
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型.
    /// @param gh 要执行的子图；其 Graph 及节点必须在本轮执行结束前保持有效.
    ///
    /// 每个 source 都会占用当前 Work 的一个 join_counter slot，完成后通过父节点
    /// tear-down 归还，因此后续 `wait()` 会等待本次提交的子图一起完成.
    ///
    /// @note 本函数不等待子图完成；需要隔离等待时使用 `corun()`.
    template <graph_holder Gh>
    void run(Gh& gh);

    /// @brief 启动一个 AsyncTask，并将其挂接为当前 Work 的动态子任务.
    /// @tparam InheritTopology 是否将当前 Work 的 Topology 作为新任务的父 Topology.
    /// @tparam T 满足 async_task concept 的任务句柄类型.
    /// @tparam Deps 满足 async_future concept 的前置依赖类型.
    /// @param task 要启动的 AsyncTask，必须非空且尚未成功启动.
    /// @param deps task 的动态前置依赖；空依赖和 task 自身不会形成等待边.
    /// @return 若 task 为左值则返回其引用，否则按值返回移动后的句柄.
    /// @throws Exception task 为空或已经启动时抛出异常.
    ///
    /// @note 启动成功后 task 占用当前 Work 的一个 join_counter slot；本函数不等待其完成.
    template <bool InheritTopology = true, async_task T, async_future... Deps>
    auto run(T&& task, Deps&&... deps) -> forward_return_t<T>;

    // ============================================================================
    // 独立子图协作执行
    // ============================================================================

    /// @brief 在独立锚点下执行指定子图，并协作式等待该子图完成.
    ///
    /// 本函数为目标子图建立独立 `AnchorWork`，使其 join_counter 与当前 Runtime
    /// 已经派生的其他任务隔离；等待期间当前 Worker 继续执行其他就绪任务.
    /// 子图完成后会检查锚点归档的异常并重新抛出.
    ///
    /// @tparam Gh 满足 graph_holder concept 的子图持有者类型.
    /// @param gh 待执行的子图，必须存活到本函数返回.
    /// @note 空图直接返回.
    template <graph_holder Gh>
    void corun(Gh& gh);

    // ============================================================================
    // 派生任务等待
    // ============================================================================

    /// @brief 协作式等待当前 Runtime 所属 Work 的全部已挂接子任务完成.
    ///
    /// Runtime callable 自身占用一个 join_counter 基准 slot，因此等待条件为
    /// `join_counter == 1`。等待期间当前 Worker 会继续运行其他就绪任务.
    ///
    /// 本函数不会自动建立显式异常锚点；等待结束后仅调用当前 Work 的异常重抛
    /// 逻辑。若希望当前 Runtime 截获并归档子任务异常，应在派生任务之前创建
    /// `ScopedExceptionAnchor`，并保证守卫至少存活到 `wait()` 返回.
    ///
    /// @code
    /// ScopedExceptionAnchor guard{runtime};
    ///
    /// runtime.run(...);
    /// runtime.wait();
    /// @endcode
    ///
    /// @note 可以重复调用；每次都会等待调用时仍挂接在当前 Work 上的子任务.
    /// @note 没有显式异常锚点时，异常按框架默认父链继续向外传播.
    /// @throws 重新抛出当前 Work 已归档的异常.
    void wait();

    /// @brief 协作运行当前 Executor，直到 @p pred() 返回 true.
    /// @tparam Pred 满足 predicate concept 的等待谓词类型.
    /// @param pred 结束条件；每次检测返回 true 时结束等待.
    /// @note 本函数不是线程阻塞等待；当前 Worker 会继续执行或窃取其他就绪任务.
    /// @warning pred 在等待期间可能被多次调用，不应依赖单次调用副作用.
    template <predicate Pred>
    void wait_until(Pred&& pred);

private:
    /// @brief 由框架构造并绑定当前 Work、Worker 与 Executor 的临时 Runtime.
    /// @param work 当前正在执行的 Work.
    /// @param worker 实际执行 work 的 Worker.
    /// @param executor 当前调度 work 的 Executor.
    /// @pre 三个对象的生命周期均覆盖本次 Runtime 使用期间.
    explicit Runtime(Work& work, Worker& worker, Executor& executor) noexcept
        : Context{work, worker, executor} {}

    /// @brief 提交 SilentAsync 子任务，并维护当前 Work 的在飞任务计数.
    ///
    /// 调度前先占用当前 Work 的一个 join_counter slot；如果任务尚未成功发布
    /// 到调度器就发生异常，则撤销该 slot 并销毁尚未执行的 Work.
    ///
    /// @param work 已完成构造、尚未发布的 SilentAsync Work.
    /// @pre work 非空且尚未被任何调度队列持有.
    void _launch_silent_async(Work* work);

    /// @brief 提交 Async 子任务，并建立 Future 引用和执行生命周期引用.
    ///
    /// Future 持有一份外部强引用，调度执行再额外持有一份执行强引用.
    /// 如果任务尚未成功发布就发生异常，则依次撤销 parent slot、Future 引用和
    /// 执行引用，并在最后一个强引用离开时销毁 Work.
    ///
    /// @tparam R 子任务结果类型.
    /// @param work 已完成构造、尚未发布的 Async Work.
    /// @param result 与 work 绑定的结果槽.
    /// @return 关联该任务的 AsyncFuture.
    /// @pre work 与 result 均非空，且 work 尚未被任何调度队列持有.
    template <typename R, async_future... Deps>
    [[nodiscard]] AsyncFuture<R> _launch_async(Work* work, ResultSlot<R>* result, const Deps&... deps);

};

// ============================================================================
// Runtime：内部提交
// ============================================================================

inline void Runtime::_launch_silent_async(Work* work) {
    TFL_ASSERT(work);

    m_work.m_join_counter.fetch_add(1, std::memory_order_relaxed);

    try {
        m_executor._schedule(m_worker, work);
    } catch (...) {
        m_work.m_join_counter.fetch_sub(1, std::memory_order_relaxed);
        destroy_work(work);
        throw;
    }
}

template <typename R, async_future... Deps>
inline AsyncFuture<R> Runtime::_launch_async(Work* work, ResultSlot<R>* result, const Deps&... deps) {
    TFL_ASSERT(work);
    TFL_ASSERT(result);

    AsyncFuture<R> future{work, result};

    auto& control = work->m_topology->m_control;
    const auto current = control.load(std::memory_order_relaxed);

    TFL_ASSERT(Topology::Control::status(current) == Topology::Control::Status::Idle);
    TFL_ASSERT(!Topology::Control::locked(current));

    control.store(Topology::Control::set_status(current, Topology::Control::Status::Running), std::memory_order_relaxed);

    // 执行生命周期额外持有一份强引用，直到 Async tear-down 完成。
    work->_increment_ref();

    // 当前 Runtime 等待时必须把该异步任务计入派生任务数量。
    m_work.m_join_counter.fetch_add(1, std::memory_order_relaxed);

    if constexpr (sizeof...(Deps) != 0) {
        std::array<Work*, sizeof...(Deps)> predecessors{deps.m_work...};
        std::size_t num_predecessors = sizeof...(Deps);

        work->m_join_counter.store(num_predecessors, std::memory_order_relaxed);
        m_executor._link_predecessors(work, predecessors.begin(), predecessors.end(), num_predecessors);

        if (num_predecessors != 0) {
            return future;
        }
    }

    try {
        m_executor._schedule(m_worker, work);
    } catch (...) {
        // 当前 Runtime callable 自身仍然占有基准 slot，
        // 因此这里撤销新增 slot 不需要触发父节点调度。
        m_work.m_join_counter.fetch_sub(1, std::memory_order_relaxed);

        if (work->_decrement_ref()) {
            destroy_work(work);
        }

        throw;
    }

    return future;
}

// ============================================================================
// Runtime：fire-and-forget 子任务
// ============================================================================

template <bool InheritTopology, graph_holder Gh, callback C>
    requires capturable<C>
inline void Runtime::silent_async(Gh&& gh, C&& cb) {
    silent_async<InheritTopology>(std::forward<Gh>(gh), 1ULL, std::forward<C>(cb));
}

template <bool InheritTopology, graph_holder Gh, callback C>
    requires capturable<C>
inline void Runtime::silent_async(Gh&& gh, std::uint64_t num, C&& cb) {
    silent_async<InheritTopology>(std::forward<Gh>(gh), [num]() mutable noexcept -> bool { return num-- == 0; }, std::forward<C>(cb));
}

template <bool InheritTopology, graph_holder Gh, predicate P, callback C>
    requires capturable<P, C>
inline void Runtime::silent_async(Gh&& gh, P&& pred, C&& cb) {
    Topology* parent_topology = nullptr;

    if constexpr (InheritTopology) {
        parent_topology = m_work.m_topology;
        TFL_ASSERT(parent_topology);
    }

    Work* work = make_silent_async_module(m_executor, std::addressof(m_work), parent_topology, std::forward<Gh>(gh), std::forward<P>(pred), std::forward<C>(cb));
    _launch_silent_async(work);
}

template <bool InheritTopology, typename T>
    requires (basic_invocable<T> && capturable<T>)
inline void Runtime::silent_async(T&& task) {
    Topology* parent_topology = nullptr;

    if constexpr (InheritTopology) {
        parent_topology = m_work.m_topology;
        TFL_ASSERT(parent_topology);
    }

    Work* work = make_silent_async_basic(m_executor, std::addressof(m_work), parent_topology, std::forward<T>(task));
    _launch_silent_async(work);
}

template <bool InheritTopology, typename T>
    requires (runtime_invocable<T> && capturable<T>)
inline void Runtime::silent_async(T&& task) {
    Topology* parent_topology = nullptr;

    if constexpr (InheritTopology) {
        parent_topology = m_work.m_topology;
        TFL_ASSERT(parent_topology);
    }

    Work* work = make_silent_async_runtime(m_executor, std::addressof(m_work), parent_topology, std::forward<T>(task));
    _launch_silent_async(work);
}

template <bool InheritTopology, typename T>
    requires (subflow_invocable<T> && capturable<T>)
inline void Runtime::silent_async(T&& task) {
    Topology* parent_topology = nullptr;

    if constexpr (InheritTopology) {
        parent_topology = m_work.m_topology;
        TFL_ASSERT(parent_topology);
    }

    Work* work = make_silent_async_subflow(m_executor, std::addressof(m_work), parent_topology, std::forward<T>(task));
    _launch_silent_async(work);
}

// ============================================================================
// Runtime：带结果通道的子任务
// ============================================================================

template <bool InheritTopology, graph_holder Gh, async_future... Deps>
inline AsyncFuture<void> Runtime::async(Gh&& gh, Deps&&... deps) {
    return async<InheritTopology>(std::forward<Gh>(gh), 1ULL, noop_callback{}, std::forward<Deps>(deps)...);
}

template <bool InheritTopology, graph_holder Gh, callback C, async_future... Deps>
    requires capturable<C>
inline AsyncFuture<void> Runtime::async(Gh&& gh, C&& cb, Deps&&... deps) {
    return async<InheritTopology>(std::forward<Gh>(gh), 1ULL, std::forward<C>(cb), std::forward<Deps>(deps)...);
}

template <bool InheritTopology, graph_holder Gh, async_future... Deps>
inline AsyncFuture<void> Runtime::async(Gh&& gh, std::uint64_t num, Deps&&... deps) {
    return async<InheritTopology>(std::forward<Gh>(gh), [num]() mutable noexcept -> bool { return num-- == 0; }, noop_callback{}, std::forward<Deps>(deps)...);
}

template <bool InheritTopology, graph_holder Gh, callback C, async_future... Deps>
    requires capturable<C>
inline AsyncFuture<void> Runtime::async(Gh&& gh, std::uint64_t num, C&& cb, Deps&&... deps) {
    return async<InheritTopology>(std::forward<Gh>(gh), [num]() mutable noexcept -> bool { return num-- == 0; }, std::forward<C>(cb), std::forward<Deps>(deps)...);
}

template <bool InheritTopology, graph_holder Gh, predicate P, async_future... Deps>
    requires capturable<P>
inline AsyncFuture<void> Runtime::async(Gh&& gh, P&& pred, Deps&&... deps) {
    return async<InheritTopology>(std::forward<Gh>(gh), std::forward<P>(pred), noop_callback{}, std::forward<Deps>(deps)...);
}

template <bool InheritTopology, graph_holder Gh, predicate P, callback C, async_future... Deps>
    requires capturable<P, C>
inline AsyncFuture<void> Runtime::async(Gh&& gh, P&& pred, C&& cb, Deps&&... deps) {
    Topology* parent_topology = nullptr;

    if constexpr (InheritTopology) {
        parent_topology = m_work.m_topology;
        TFL_ASSERT(parent_topology);
    }

    auto [work, result] = make_async_module(m_executor, std::addressof(m_work), parent_topology, std::forward<Gh>(gh), std::forward<P>(pred), std::forward<C>(cb));
    return _launch_async(work, result, std::forward<Deps>(deps)...);
}

template <bool InheritTopology, typename T, async_future... Deps>
    requires (basic_invocable<T> && capturable<T>)
inline auto Runtime::async(T&& task, Deps&&... deps) -> AsyncFuture<basic_return_t<T>> {
    Topology* parent_topology = nullptr;

    if constexpr (InheritTopology) {
        parent_topology = m_work.m_topology;
        TFL_ASSERT(parent_topology);
    }

    auto [work, result] = make_async_basic(m_executor, std::addressof(m_work), parent_topology, std::forward<T>(task));
    return _launch_async(work, result, std::forward<Deps>(deps)...);
}

template <bool InheritTopology, typename T, async_future... Deps>
    requires (runtime_invocable<T> && capturable<T>)
inline auto Runtime::async(T&& task, Deps&&... deps) -> AsyncFuture<runtime_return_t<T>> {
    Topology* parent_topology = nullptr;

    if constexpr (InheritTopology) {
        parent_topology = m_work.m_topology;
        TFL_ASSERT(parent_topology);
    }

    auto [work, result] = make_async_runtime(m_executor, std::addressof(m_work), parent_topology, std::forward<T>(task));
    return _launch_async(work, result, std::forward<Deps>(deps)...);
}

template <bool InheritTopology, typename T, async_future... Deps>
    requires (subflow_invocable<T> && capturable<T>)
inline auto Runtime::async(T&& task, Deps&&... deps) -> AsyncFuture<subflow_return_t<T>> {
    Topology* parent_topology = nullptr;

    if constexpr (InheritTopology) {
        parent_topology = m_work.m_topology;
        TFL_ASSERT(parent_topology);
    }

    auto [work, result] = make_async_subflow(m_executor, std::addressof(m_work), parent_topology, std::forward<T>(task));
    return _launch_async(work, result, std::forward<Deps>(deps)...);
}

// ============================================================================
// Runtime::run
// ============================================================================

template <bool InheritTopology, async_task T, async_future... Deps>
inline auto Runtime::run(T&& task, Deps&&... deps) -> forward_return_t<T> {
    task.template _start<InheritTopology>(m_work, m_worker, m_executor, std::forward<Deps>(deps)...);
    return std::forward<T>(task);
}

template <graph_holder Gh>
inline void Runtime::corun(Gh& gh) {
    auto& graph = detail::to_graph(gh);

    if (graph.empty()) {
        return;
    }

    AnchorWork anchor{m_work, m_executor};

    m_executor._corun_graph(graph, anchor, m_worker);
    anchor._rethrow_exception();
}

template <graph_holder Gh>
inline void Runtime::run(Gh& gh) {
    auto& graph = detail::to_graph(gh);
    auto num_srcs = m_executor._set_up_graph(graph, m_work);
    if(num_srcs == 0) {
        return;
    }

    m_work.m_join_counter.fetch_add(num_srcs, std::memory_order_relaxed);
    m_executor._schedule(m_worker, graph.begin(), num_srcs);
}

inline void Runtime::wait() {
    wait_until([this]() noexcept {
        return m_work.m_join_counter.load(std::memory_order_acquire) == 1;
    });

    m_work._rethrow_exception();
}


template <predicate Pred>
inline void Runtime::wait_until(Pred&& pred) {
    m_executor._corun_until(m_worker, std::forward<Pred>(pred));
}

} // namespace tfl
