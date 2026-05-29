/// @file runtime.hpp
/// @brief 运行时调度上下文 - 任务执行期间的动态调度接口
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include "executor.hpp"
namespace tfl {

/// @brief 运行时调度上下文 —— 任务体内动态派生/激活子任务的唯一通道。
///
/// Immovable，由 Worker 在栈上构造并按 Runtime& 注入用户 callable。
/// 提供 detach（fire-and-forget 派生）、async（派生 + Future 返回值）、
/// schedule（激活兄弟任务）、cowait（协作式等待不阻塞 worker）四组 API。
/// 派生子任务通过父节点 join_counter 挂接，保证子任务引用资源存活。
///
/// stop_requested() / has_exception() 暴露父节点终止/异常状态，用于协作式取消。
///
/// @note 构造函数私有、仅友元可造；栈帧内自动析构，引用不可逸出栈外。
///


class Runtime : public Immovable<Runtime> {
    template <typename> friend class AsyncTask;

    TFL_WORK_SUBCLASS_FRIENDS;

public:
    /// @brief Fire-and-forget 派发：提交子图执行一次，不返回结果，不阻塞当前任务。
    /// @tparam A 锚点策略（anchor::none_t 或 explicit_t），控制子节点如何挂接到父节点生命周期。
    /// @tparam Gh 满足 graph_holder concept 的类型。
    /// @param gh 要执行的子图。
    template <anchor_tag A = anchor::none_t, typename Gh>
        requires graph_holder<Gh>
    void detach(Gh&& gh);

    /// @brief Fire-and-forget 派发：提交子图执行一次，子图所有节点完成后调用 @p cb。
    /// @tparam A 锚点策略。
    /// @tparam Gh 满足 graph_holder concept 的类型。
    /// @tparam C 满足 callback concept 的回调类型。
    /// @param gh 要执行的子图。
    /// @param cb 子图完成后的回调（无参数 callable）。
    template <anchor_tag A = anchor::none_t, typename Gh, typename C>
        requires (capturable<C> && graph_holder<Gh> && callback<C>)
    void detach(Gh&& gh, C&& cb);

    /// @brief Fire-and-forget 派发：提交子图循环执行 @p num 次。
    /// @tparam A 锚点策略。
    /// @tparam Gh 满足 graph_holder concept 的类型。
    /// @param gh 要执行的子图。
    /// @param num 循环次数。
    template <anchor_tag A = anchor::none_t, typename Gh>
        requires graph_holder<Gh>
    void detach(Gh&& gh, std::uint64_t num);

    /// @brief Fire-and-forget 派发：提交子图循环执行 @p num 次，完成后调用 @p cb。
    /// @tparam A 锚点策略。
    /// @tparam Gh 满足 graph_holder concept 的类型。
    /// @tparam C 满足 callback concept 的回调类型。
    /// @param gh 要执行的子图。
    /// @param num 循环次数。
    /// @param cb 子图完成后的回调。
    template <anchor_tag A = anchor::none_t, typename Gh, typename C>
        requires (capturable<C> && graph_holder<Gh> && callback<C>)
    void detach(Gh&& gh, std::uint64_t num, C&& cb);

    /// @brief Fire-and-forget 派发：提交子图条件循环执行，每次迭代前调用 @p pred()，返回 true 时停止。
    /// @tparam A 锚点策略。
    /// @tparam Gh 满足 graph_holder concept 的类型。
    /// @tparam P 满足 predicate concept 的可调用对象。
    /// @param gh 要执行的子图。
    /// @param pred 循环终止谓词。
    template <anchor_tag A = anchor::none_t, typename Gh, typename P>
        requires (capturable<P> && graph_holder<Gh> && predicate<P>)
    void detach(Gh&& gh, P&& pred);

    /// @brief Fire-and-forget 派发：提交子图条件循环执行，完成后调用 @p cb。
    /// @tparam A 锚点策略。
    /// @tparam Gh 满足 graph_holder concept 的类型。
    /// @tparam P 满足 predicate concept 的可调用对象。
    /// @tparam C 满足 callback concept 的回调类型。
    /// @param gh 要执行的子图。
    /// @param pred 循环终止谓词。
    /// @param cb 子图完成后的回调。
    template <anchor_tag A = anchor::none_t, typename Gh, typename P, typename C>
        requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
    void detach(Gh&& gh, P&& pred, C&& cb);


    /// @brief Fire-and-forget 派发：异步执行单个无参 callable，不返回结果，不阻塞当前任务。
    /// @tparam A 锚点策略。
    /// @tparam T 满足 basic_invocable_plain concept 的可调用对象。
    /// @tparam Args 转交给 task 的附加参数。
    /// @param task 要执行的可调用对象。
    /// @param args 转发给 task 的参数。
    template <anchor_tag A = anchor::none_t, typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable_plain<T, Args...>)
    void detach(T&& task, Args&&... args);

    /// @brief Fire-and-forget 派发：异步执行单个 runtime callable，task 可接收 Runtime& 进行嵌套调度。
    /// @tparam A 锚点策略。
    /// @tparam T 满足 runtime_invocable_plain concept 的可调用对象。
    /// @tparam Args 转交给 task 的附加参数。
    /// @param task 要执行的可调用对象。
    /// @param args 转发给 task 的参数。
    template <anchor_tag A = anchor::none_t, typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable_plain<T, Args...>)
    void detach(T&& task, Args&&... args);


    /// @brief 异步执行子图一次，返回 Future<void> 用于等待完成或请求停止。
    /// @tparam A 锚点策略（默认 explicit_t，子节点显式挂接到父节点生命周期）。
    /// @tparam Gh 满足 graph_holder concept 的类型。
    /// @param gh 要执行的子图。
    /// @return Future<void>，支持 get()/wait()/request_stop()。
    template <anchor_tag A = anchor::explicit_t, graph_holder Gh>
    [[nodiscard]] Future<void> async(Gh&& gh);

    /// @brief 异步执行子图一次，完成后调用 @p cb，返回 Future<void>。
    /// @tparam A 锚点策略。
    /// @tparam Gh 满足 graph_holder concept 的类型。
    /// @tparam C 满足 callback concept 的回调类型。
    /// @param gh 要执行的子图。
    /// @param cb 子图完成后调用的回调。
    /// @return Future<void>。
    template <anchor_tag A = anchor::explicit_t, graph_holder Gh, typename C>
        requires (capturable<C> && callback<C>)
    [[nodiscard]] Future<void> async(Gh&& gh, C&& cb);

    /// @brief 异步执行子图 @p num 次，返回 Future<void>。
    /// @tparam A 锚点策略。
    /// @tparam Gh 满足 graph_holder concept 的类型。
    /// @param gh 要执行的子图。
    /// @param num 循环次数。
    /// @return Future<void>。
    template <anchor_tag A = anchor::explicit_t, graph_holder Gh>
    [[nodiscard]] Future<void> async(Gh&& gh, std::uint64_t num);

    /// @brief 异步执行子图 @p num 次，完成后调用 @p cb，返回 Future<void>。
    /// @tparam A 锚点策略。
    /// @tparam Gh 满足 graph_holder concept 的类型。
    /// @tparam C 满足 callback concept 的回调类型。
    /// @param gh 要执行的子图。
    /// @param num 循环次数。
    /// @param cb 子图完成后的回调。
    /// @return Future<void>。
    template <anchor_tag A = anchor::explicit_t, graph_holder Gh, typename C>
        requires (capturable<C> && callback<C>)
    [[nodiscard]] Future<void> async(Gh&& gh, std::uint64_t num, C&& cb);

    /// @brief 异步执行子图条件循环：每次迭代前调用 @p pred()，返回 true 时停止。
    /// @tparam A 锚点策略。
    /// @tparam Gh 满足 graph_holder concept 的类型。
    /// @tparam P 满足 predicate concept 的可调用对象。
    /// @param gh 要执行的子图。
    /// @param pred 循环终止谓词（每次迭代前调用，返回 true 停止）。
    /// @return Future<void>。
    template <anchor_tag A = anchor::explicit_t, graph_holder Gh, typename P>
        requires (capturable<P> && predicate<P>)
    [[nodiscard]] Future<void> async(Gh&& gh, P&& pred);

    /// @brief 异步执行子图条件循环，完成后调用 @p cb。
    /// @tparam A 锚点策略。
    /// @tparam Gh 满足 graph_holder concept 的类型。
    /// @tparam P 满足 predicate concept 的可调用对象。
    /// @tparam C 满足 callback concept 的回调类型。
    /// @param gh 要执行的子图。
    /// @param pred 循环终止谓词。
    /// @param cb 子图完成后的回调。
    /// @return Future<void>。
    template <anchor_tag A = anchor::explicit_t, graph_holder Gh, typename P, typename C>
        requires (capturable<P, C> && predicate<P> && callback<C>)
    [[nodiscard]] Future<void> async(Gh&& gh, P&& pred, C&& cb);

    /// @brief 异步执行单个 callable，返回 Future<R> 携带返回值。
    /// @tparam A 锚点策略。
    /// @tparam T 满足 basic_invocable concept 的可调用对象。
    /// @tparam Args 转交给 task 的附加参数。
    /// @param task 要执行的可调用对象。
    /// @param args 转发给 task 的参数。
    /// @return Future<R>，R = basic_return_t<T, Args...> 即 callable 的返回类型。
    template <anchor_tag A = anchor::explicit_t, typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    [[nodiscard]] auto async(T&& task, Args&&... args) -> Future<basic_return_t<T, Args...>>;

    /// @brief 异步执行单个 runtime callable（可接收 Runtime&），返回 Future<R> 携带返回值。
    /// @tparam A 锚点策略。
    /// @tparam T 满足 runtime_invocable concept 的可调用对象。
    /// @tparam Args 转交给 task 的附加参数。
    /// @param task 要执行的可调用对象。
    /// @param args 转发给 task 的参数。
    /// @return Future<R>，R = runtime_return_t<T, Args...> 即 callable 的返回类型。
    template <anchor_tag A = anchor::explicit_t, typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    [[nodiscard]] auto async(T&& task, Args&&... args) -> Future<runtime_return_t<T, Args...>>;

    /// @brief 检查当前 Runtime 对应的父节点是否已收到停止请求（通过 Topology::m_stop_source）。
    /// @return 已请求停止时返回 true；用户可在 callable 中查询此值实现协作式取消。
    [[nodiscard]] bool stop_requested() const noexcept;

    /// @brief 检查当前 Runtime 对应的父节点执行过程中是否已捕获异常。
    /// @return 有异常时返回 true，cowait() 结束后可检查此值决定是否处理异常。
    [[nodiscard]] bool has_exception() const noexcept;
    // ========================================================================
    //  同步执行 / 协作式等待
    // ========================================================================
    /// @brief 立即调度一个兄弟任务（同父子关系）到当前 worker 的本地 LIFO 队列执行。
    /// @param task 兄弟 Task，必须与 Runtime 的 work 共享同一非空父节点，否则抛出 Exception。
    void submit(Task task);

    /// @brief 立即调度整个子图到当前 worker，采用 cowait 语义逐节点注入队列。
    /// @tparam Gh 满足 graph_holder concept 的类型。
    /// @param gh 要调度的子图。
    template <typename Gh>
        requires graph_holder<Gh>
    void submit(Gh& gh);

    /// @brief 提交 AsyncTask 及其依赖列表：注入前置依赖节点后调度执行。
    /// @tparam A 锚点策略。
    /// @tparam T 满足 any_async_task concept 的 AsyncTask 类型。
    /// @tparam Deps 满足 nonrepeat_async_task concept 的前置依赖（AsyncTask 列表）。
    /// @param task 要调度的 AsyncTask。
    /// @param deps 前置依赖 AsyncTask，当前 task 将在所有 deps 完成后执行。
    /// @return lvalue 引用或值（取决于 task 是 lvalue 还是 rvalue）。
    template <anchor_tag A = anchor::explicit_t, typename T, typename... Deps>
        requires (any_async_task<T> && (nonrepeat_async_task<Deps> && ...))
    auto submit(T&& task, Deps&&... deps) -> std::conditional_t<std::is_lvalue_reference_v<T>, std::remove_reference_t<T>&, std::remove_cvref_t<T>>;

    /// @brief 提交 AsyncTask 及迭代器范围的依赖：注入迭代器指向的所有前置节点后调度执行。
    /// @tparam A 锚点策略。
    /// @tparam T 满足 any_async_task concept 的 AsyncTask 类型。
    /// @tparam I 输入迭代器，其 value_type 满足 nonrepeat_async_task。
    /// @tparam S I 的 sentinel 类型。
    /// @param task 要调度的 AsyncTask。
    /// @param first, last 前置依赖的迭代器范围。
    /// @return lvalue 引用或值。
    template <anchor_tag A = anchor::explicit_t, typename T, std::input_iterator I, std::sentinel_for<I> S>
        requires (any_async_task<T> && nonrepeat_async_task<std::iter_value_t<I>>)
    auto submit(T&& task, I first, S last) -> std::conditional_t<std::is_lvalue_reference_v<T>, std::remove_reference_t<T>&, std::remove_cvref_t<T>>;


    /// @brief 协作式等待：阻塞当前 worker 直到 @p gh 所有节点执行完毕，期间 worker 持续窃取/执行其它任务。
    /// @tparam Gh 满足 graph_holder concept 的类型。
    /// @param gh 要等待的子图。
    /// @note 不阻塞 OS 线程，worker 在等待期间继续参与调度；这一过程称为 cowait。
    template <typename Gh>
        requires graph_holder<Gh>
    void cowait(Gh& gh);

    /// @brief 协作式等待：阻塞直到当前 Runtime 派生的所有子任务完成（join_counter 回到 1）。
    /// @note join_counter == 1 表示只剩父节点自身，即所有子任务均已 tear_down。
    void cowait();

    /// @brief 协作式等直到 @p pred() 返回 true，期间 worker 继续执行其它任务。
    /// @tparam Pred 满足 predicate concept 的可调用对象。
    /// @param pred 轮询谓词，返回 true 时停止等待。
    template <predicate Pred>
    void cowait_until(Pred&& pred);

    // ========================================================================
    //  Worker 访问
    // ========================================================================
    /// @brief 获取当前执行上下文所属 Worker 的可变引用（可查询 worker_id、本地队列等）。
    [[nodiscard]] Worker& worker() noexcept;
    /// @brief 获取当前执行上下文所属 Worker 的只读引用。
    [[nodiscard]] const Worker& worker() const noexcept;

    /// @brief 获取当前执行上下文所属 Executor 的可变引用（可提交外部任务、获取线程数等）。
    [[nodiscard]] Executor& executor() noexcept;
    /// @brief 获取当前执行上下文所属 Executor 的只读引用。
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

template <anchor_tag A, typename Gh>
    requires graph_holder<Gh>
inline void Runtime::detach(Gh&& gh) {
    return detach<A>(std::forward<Gh>(gh), 1ULL);
}

template <anchor_tag A, typename Gh, typename C>
    requires (capturable<C> && graph_holder<Gh> && callback<C>)
inline void Runtime::detach(Gh&& gh, C&& cb) {
    return detach<A>(std::forward<Gh>(gh), 1ULL, std::forward<C>(cb));
}

template <anchor_tag A, typename Gh>
    requires graph_holder<Gh>
inline void Runtime::detach(Gh&& gh, std::uint64_t num) {
    return detach<A>(std::forward<Gh>(gh), num, []() noexcept {});
}

template <anchor_tag A, typename Gh, typename C>
    requires (capturable<C> && graph_holder<Gh> && callback<C>)
inline void Runtime::detach(Gh&& gh, std::uint64_t num, C&& cb) {
    // 将次数转换为谓词
    return detach<A>(std::forward<Gh>(gh)
                           ,[num]() mutable noexcept { return num-- == 0; }
                           ,std::forward<C>(cb));
}

template <anchor_tag A, typename Gh, typename P>
    requires (capturable<P> && graph_holder<Gh> && predicate<P>)
inline void Runtime::detach(Gh&& gh, P&& pred) {
    return detach<A>(std::forward<Gh>(gh)
                           ,std::forward<P>(pred)
                           ,[]() noexcept {});
}

template <anchor_tag A, typename Gh, typename P, typename C>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
inline void Runtime::detach(Gh&& gh, P&& pred, C&& cb) {
    Work* work = make_detached_flow<A>(
        std::addressof(m_executor),
        std::addressof(m_work),
        std::forward<Gh>(gh),
        std::forward<P>(pred),
        std::forward<C>(cb));

    m_work.m_join_counter.fetch_add(1, std::memory_order_relaxed);
    m_executor._schedule(m_worker, work);
}

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable_plain<T, Args...>)
inline void Runtime::detach(T&& task, Args&&... args) {
    Work* work = make_detached_basic<A>(
        std::addressof(m_executor),
        std::addressof(m_work),
        std::forward<T>(task),
        std::forward<Args>(args)...);

    m_work.m_join_counter.fetch_add(1, std::memory_order_relaxed);
    m_executor._schedule(m_worker, work);
}

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable_plain<T, Args...>)
inline void Runtime::detach(T&& task, Args&&... args) {
    Work* work = make_detached_runtime<A>(
        std::addressof(m_executor),
        std::addressof(m_work),
        std::forward<T>(task),
        std::forward<Args>(args)...);

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

    Work* work = make_promised_flow<A>(
        std::addressof(m_executor),
        std::addressof(m_work),
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

    Work* work = make_promised_basic<A>(
        std::addressof(m_executor),
        std::addressof(m_work),
        std::forward<T>(task),
        std::move(promise),
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

    Work* work = make_promised_runtime<A>(
        std::addressof(m_executor),
        std::addressof(m_work),
        std::forward<T>(task),
        std::move(promise),
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
///   - @p task 必须与本 Runtime 的 work 共享同一父节点（兄弟关系），
///     即两者归属同一隔离生命周期。
///   - 父节点不可为 @c nullptr —— 拒绝顶级孤立任务（无法占位）。
///   - 违反任一约束抛出 @c Exception。
inline void Runtime::submit(Task task) {
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
inline void Runtime::submit(Gh& gh) {
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
