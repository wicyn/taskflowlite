/// @file runtime.hpp
/// @brief 运行时调度上下文 - 任务执行期间的动态调度接口
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once
#include "worker.hpp"
#include "flow.hpp"

namespace tfl {
/// @brief 运行时调度上下文 - 任务执行期间的动态调度接口
///
/// @details
/// 当任务签名包含 `Runtime&` 参数时，调度器传递此代理对象。
/// 通过它可以在任务执行期间：
/// - 动态提交新任务/子图
/// - 协作式等待其他任务完成
/// - 访问当前 Worker 上下文
class Runtime : public MoveOnly<Runtime> {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;
    TFL_WORK_SUBCLASS_FRIENDS;
public:
    // ========================================================================
    //  动态任务提交 — Flow
    // ========================================================================
    template <typename F>
        requires flow_type<F>
    AsyncTask submit(F&& flow);

    template <typename F, typename C>
        requires (capturable<C> && flow_type<F> && callback<C>)
    AsyncTask submit(F&& flow, C&& callback);

    template <typename F>
        requires flow_type<F>
    AsyncTask submit(F&& flow, std::uint64_t num);

    template <typename F, typename C>
        requires (capturable<C> && flow_type<F> && callback<C>)
    AsyncTask submit(F&& flow, std::uint64_t num, C&& callback);

    template <typename F, typename P>
        requires (capturable<P> && flow_type<F> && predicate<P>)
    AsyncTask submit(F&& flow, P&& pred);

    template <typename F, typename P, typename C>
        requires (capturable<P, C> && flow_type<F> && predicate<P> && callback<C>)
    AsyncTask submit(F&& flow, P&& pred, C&& callback);

    // ========================================================================
    //  动态任务提交 — 单任务
    // ========================================================================
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    AsyncTask submit(T&& task, Args&&... args);

    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    AsyncTask submit(T&& task, Args&&... args);

    // ========================================================================
    //  Fire-and-forget
    // ========================================================================
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    void silent_async(T&& task, Args&&... args);

    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    void silent_async(T&& task, Args&&... args);

    // ========================================================================
    //  异步 Future
    // ========================================================================
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    auto async(T&& task, Args&&... args) -> std::future<basic_return_t<T, Args...>>;

    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    auto async(T&& task, Args&&... args) -> std::future<runtime_return_t<T, Args...>>;

    // ========================================================================
    //  同步执行 / 协作式等待
    // ========================================================================
    void run(Task task);
    void run(Flow& flow);

    template <predicate Pred>
    void wait_until(Pred&& pred);

    // ========================================================================
    //  Worker 访问
    // ========================================================================
    Worker& worker() noexcept;
    const Worker& worker() const noexcept;

private:
    Work& m_work;
    Worker& m_worker;
    Topology& m_topology;
    Executor& m_executor;

    explicit Runtime(Work& work, Worker& wr, Topology& topo, Executor& exec) noexcept
        : m_work{work}, m_worker{wr}, m_topology{topo}, m_executor{exec} {}
};

// ============================================================================
// Implementation
// ============================================================================
inline Worker& Runtime::worker() noexcept { return m_worker; }
inline const Worker& Runtime::worker() const noexcept { return m_worker; }

} // namespace tfl
