/// @file executor.hpp
/// @brief 任务调度器核心 - Work-Stealing 并行执行引擎
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cassert>

#include "flow.hpp"
#include "future.hpp"
#include "worker.hpp"
#include "unordered_dense.hpp"
#include "unbounded_queue.hpp"

namespace tfl {

/// @brief 任务调度器 —— Work-Stealing 并行执行引擎。
///
/// @details
/// `Executor` 是框架的运行时执行核心：管理 worker 线程、本地工作窃取队列、
/// 共享提交队列、任务唤醒器以及顶层 Topology 生命周期计数。`Flow`、`Task`、
/// `Runtime`、`AsyncTask` 等用户层对象最终都通过 `Executor` 落地执行。
///
/// ============================================================================
///  调度结构
/// ============================================================================
/// @code
///   Flow / AsyncTask / Runtime
///              │ schedule
///              ▼
///        ┌────────────┐
///        │  Executor  │
///        └─────┬──────┘
///              │
///     ┌────────┴────────┐
///     ▼                 ▼
///  Worker local queues   Shared buffers
///  BoundedQueue          UnboundedQueue
/// @endcode
///
/// ============================================================================
///  工作窃取三阶段
/// ============================================================================
/// 1. 本地队列优先：worker 从自己的 `BoundedQueue` 尾部 LIFO pop，缓存友好；
/// 2. 随机窃取：空闲 worker 从其他 worker 或 shared buffer 头部 FIFO steal；
/// 3. 阻塞等待：多轮失败后通过 `Notifier` 进入两阶段 park，避免 lost wake-up。
///
/// `m_shared_buffers` 是对数级分片的共享队列集合，用于外部线程提交或本地队列溢出。
/// `_push_shared` 通过任务地址哈希选择 buffer，优先 try_lock，失败后线性探测，
/// 最后才阻塞锁定目标 buffer。
///
/// ============================================================================
///  Topology 生命周期计数
/// ============================================================================
/// `m_num_topologies` 只统计顶层执行实例：
/// - 顶层任务提交时 +1；
/// - 顶层任务完成时 -1；
/// - 归零时唤醒 `wait_for_all()`。
///
/// 嵌套任务或 Runtime 派生任务不重复计入全局计数，而是由父节点的
/// `m_join_counter` 管理完成关系。这样 `wait_for_all()` 等待的是用户提交的根任务，
/// 而不是内部执行过程中临时派生出的每个子任务。
///
/// `~Executor()` 会先等待所有顶层任务完成，再设置 worker 终止标志，唤醒全部
/// 等待线程并 join worker。
///
/// ============================================================================
///  提交 API
/// ============================================================================
/// | API               | 返回值                  | 启动方式        | 语义 |
/// |-------------------|-------------------------|-----------------|------|
/// | `silent_async`    | `void`                  | 立即启动        | 即发即弃 |
/// | `async`           | `Future<R>`             | 立即启动        | 异步返回值 |
/// | `dependent_async` | `AsyncTask`             | 立即启动/等待依赖 | 动态依赖任务 |
/// | `deferred_async`  | `DeferredAsyncTask`     | 手动 `start()`  | 启动前可配置 |
///
/// 每组 API 都按 `graph_holder`、`basic_invocable`、`runtime_invocable` 等概念分发，
/// 最终收敛到 Work 工厂、依赖初始化和 `_schedule` 调度入口。
///
/// ============================================================================
///  上下文感知调度
/// ============================================================================
/// - 当前线程是 worker：优先推入该 worker 的本地队列；
/// - 当前线程不是 worker：推入 shared buffer；
/// - 本地队列满：溢出到 shared buffer。
///
/// 这让任务体内递归派生任务走最快路径，同时保证外部线程提交也不会争抢单个
/// 全局队列。
///
/// ============================================================================
///  内部协议接口
/// ============================================================================
/// `_set_up_graph` / `_process_dependent_async` / `_tear_down_*` /
/// `_schedule_parent` / `_cowait_until` 等函数是 `Executor` 与 `Work`、`Runtime`、
/// `Topology` 之间的内部调度协议，不属于用户 API。
///
/// ============================================================================
///  内存序约定
/// ============================================================================
/// - `m_num_topologies`：用于 `wait_for_all()` 的 acquire/wait/notify 同步；
/// - `m_join_counter`：后继就绪判断使用 acq_rel，保证前驱结果对后继可见；
/// - `m_terminate`：终止轮询使用 relaxed；
/// - `Topology::m_state`：动态依赖边插入使用 CAS 进入 Locking 状态。
///
/// 调度路径只需要局部 happens-before，不需要全局 `seq_cst` 总序。
///
/// ============================================================================
///  线程安全
/// ============================================================================
/// `Executor` 的 public API 可被多个线程并发调用。内部共享状态由原子变量、
/// work-stealing 队列、`Notifier` 和分片 mutex 保护。
///
/// 用户 callable 和正在执行中的 `Flow` 结构修改不由 Executor 保护：
/// - callable 内部共享数据需用户自行同步；
/// - `Flow` 提交执行后应视为只读，不能并发 emplace / erase / clear。
///
/// @see Worker     实际执行任务的工作线程对象
/// @see Notifier   worker 空闲时的无锁等待/唤醒机制
/// @see Topology   单次执行实例与动态任务生命周期
/// @see Runtime    任务体内的动态调度上下文
/// @see AsyncTask  动态任务强引用句柄

class Executor : public Immovable<Executor> {
    friend class Work;
    friend class Flow;
    friend class AsyncTask;
    friend class DeferredAsyncTask;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;

public:
    /// @brief 创建调度器并启动工作线程
    /// @param handler 异常处理策略（WorkerHandler）
    /// @param num_workers 工作线程数量（默认 CPU 核心数）
    explicit Executor(WorkerHandler& handler, std::size_t num_workers = std::thread::hardware_concurrency());

    /// @brief 销毁调度器
    /// @post 等待所有已提交任务执行完成，停止所有工作线程并释放资源
    ~Executor() noexcept;

    // ========================================================================
    //  任务提交 API
    // ========================================================================

    /// @brief 提交任务图执行一次
    template <typename Gh>
        requires graph_holder<Gh>
    [[nodiscard]] DeferredAsyncTask deferred_async(Gh&& gh);

    /// @brief 提交任务图执行一次，完成后执行回调
    template <typename Gh, typename C>
        requires (capturable<C> && graph_holder<Gh> && callback<C>)
    [[nodiscard]] DeferredAsyncTask deferred_async(Gh&& gh, C&& cb);

    /// @brief 提交任务图执行指定次数
    template <typename Gh>
        requires graph_holder<Gh>
    [[nodiscard]] DeferredAsyncTask deferred_async(Gh&& gh, std::uint64_t num);

    /// @brief 提交任务图循环执行指定次数，完成后执行回调
    template <typename Gh, typename C>
        requires (capturable<C> && graph_holder<Gh> && callback<C>)
    [[nodiscard]] DeferredAsyncTask deferred_async(Gh&& gh, std::uint64_t num, C&& cb);

    /// @brief 提交任务图条件循环执行
    template <typename Gh, typename P>
        requires (capturable<P> && graph_holder<Gh> && predicate<P>)
    [[nodiscard]] DeferredAsyncTask deferred_async(Gh&& gh, P&& pred);

    /// @brief 提交任务图条件循环执行，完成后执行回调
    template <typename Gh, typename P, typename C>
        requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
    [[nodiscard]] DeferredAsyncTask deferred_async(Gh&& gh, P&& pred, C&& cb);

    /// @brief 提交单个异步任务
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    [[nodiscard]] DeferredAsyncTask deferred_async(T&& task, Args&&... args);

    /// @brief 提交单个运行时任务（可动态操纵图结构）
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    [[nodiscard]] DeferredAsyncTask deferred_async(T&& task, Args&&... args);

    template <typename Gh, typename... Deps>
        requires graph_holder<Gh> && (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...)
    [[nodiscard]] AsyncTask dependent_async(Gh&& gh, Deps&&... deps);

    template <typename Gh, typename C, typename... Deps>
        requires (capturable<C> && graph_holder<Gh> && callback<C> &&
                 (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
    [[nodiscard]] AsyncTask dependent_async(Gh&& gh, C&& cb, Deps&&... deps);

    template <typename Gh, typename... Deps>
        requires graph_holder<Gh> && (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...)
    [[nodiscard]] AsyncTask dependent_async(Gh&& gh, std::uint64_t num, Deps&&... deps);

    template <typename Gh, typename C, typename... Deps>
        requires (capturable<C> && graph_holder<Gh> && callback<C> &&
                 (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
    [[nodiscard]] AsyncTask dependent_async(Gh&& gh, std::uint64_t num, C&& cb, Deps&&... deps);

    template <typename Gh, typename P, typename... Deps>
        requires (capturable<P> && graph_holder<Gh> && predicate<P> &&
                 (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
    [[nodiscard]] AsyncTask dependent_async(Gh&& gh, P&& pred, Deps&&... deps);

    template <typename Gh, typename P, typename C, typename... Deps>
        requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C> &&
                 (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
    [[nodiscard]] AsyncTask dependent_async(Gh&& gh, P&& pred, C&& cb, Deps&&... deps);

    template <typename Gh, typename P, typename C, std::input_iterator I, std::sentinel_for<I> S>
        requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C> &&
                 std::derived_from<std::iter_value_t<I>, AsyncTask>)
    [[nodiscard]] AsyncTask dependent_async(Gh&& gh, P&& pred, C&& cb, I first, S last);


    template <typename T, typename... Deps>
        requires (basic_invocable<T> && (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
    [[nodiscard]] AsyncTask dependent_async(T&& task, Deps&&... deps);

    template <typename T, std::input_iterator I, std::sentinel_for<I> S>
        requires (basic_invocable<T> && std::derived_from<std::iter_value_t<I>, AsyncTask>)
    [[nodiscard]] AsyncTask dependent_async(T&& task, I first, S last);

    template <typename T, typename... Deps>
        requires (runtime_invocable<T> && (std::derived_from<std::remove_cvref_t<Deps>, AsyncTask> && ...))
    [[nodiscard]] AsyncTask dependent_async(T&& task, Deps&&... deps);

    template <typename T, std::input_iterator I, std::sentinel_for<I> S>
        requires (runtime_invocable<T> && std::derived_from<std::iter_value_t<I>, AsyncTask>)
    [[nodiscard]] AsyncTask dependent_async(T&& task, I first, S last);


    /// @brief 提交任务图执行一次
    template <typename Gh>
        requires graph_holder<Gh>
    void silent_async(Gh&& gh);

    /// @brief 提交任务图执行一次，完成后执行回调
    template <typename Gh, typename C>
        requires (capturable<C> && graph_holder<Gh> && callback<C>)
    void silent_async(Gh&& gh, C&& cb);

    /// @brief 提交任务图执行指定次数
    template <typename Gh>
        requires graph_holder<Gh>
    void silent_async(Gh&& gh, std::uint64_t num);

    /// @brief 提交任务图循环执行指定次数，完成后执行回调
    template <typename Gh, typename C>
        requires (capturable<C> && graph_holder<Gh> && callback<C>)
    void silent_async(Gh&& gh, std::uint64_t num, C&& cb);

    /// @brief 提交任务图条件循环执行
    template <typename Gh, typename P>
        requires (capturable<P> && graph_holder<Gh> && predicate<P>)
    void silent_async(Gh&& gh, P&& pred);

    /// @brief 提交任务图条件循环执行，完成后执行回调
    template <typename Gh, typename P, typename C>
        requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
    void silent_async(Gh&& gh, P&& pred, C&& cb);

    /// @brief 即发即弃异步执行
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable_plain<T, Args...>)
    void silent_async(T&& task, Args&&... args);

    /// @brief 即发即弃运行时任务
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable_plain<T, Args...>)
    void silent_async(T&& task, Args&&... args);


    /// @brief 异步执行任务图一次，返回 Future<void>
    template <graph_holder Gh>
    [[nodiscard]] Future<void> async(Gh&& gh);

    /// @brief 异步执行任务图一次，完成后执行回调，返回 Future<void>
    /// @note 回调异常归档到 Future——通过 future.get() 可见
    template <graph_holder Gh, typename C>
        requires (capturable<C> && callback<C>)
    [[nodiscard]] Future<void> async(Gh&& gh, C&& cb);

    /// @brief 异步执行任务图指定次数，返回 Future<void>
    template <graph_holder Gh>
    [[nodiscard]] Future<void> async(Gh&& gh, std::uint64_t num);

    /// @brief 异步执行任务图指定次数，完成后执行回调，返回 Future<void>
    template <graph_holder Gh, typename C>
        requires (capturable<C> && callback<C>)
    [[nodiscard]] Future<void> async(Gh&& gh, std::uint64_t num, C&& cb);

    /// @brief 异步执行任务图条件循环，返回 Future<void>
    template <graph_holder Gh, typename P>
        requires (capturable<P> && predicate<P>)
    [[nodiscard]] Future<void> async(Gh&& gh, P&& pred);

    /// @brief 异步执行任务图条件循环，完成后执行回调，返回 Future<void>
    template <graph_holder Gh, typename P, typename C>
        requires (capturable<P, C> && predicate<P> && callback<C>)
    [[nodiscard]] Future<void> async(Gh&& gh, P&& pred, C&& cb);


    /// @brief 异步执行并返回 Future
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    [[nodiscard]] auto async(T&& task, Args&&... args) -> Future<basic_return_t<T, Args...>>;

    /// @brief 异步执行运行时任务并返回 Future
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    [[nodiscard]] auto async(T&& task, Args&&... args) -> Future<runtime_return_t<T, Args...>>;


    /// @brief 阻塞等待所有任务完成
    void wait_for_all() const noexcept;

    // ========================================================================
    //  状态查询接口
    // ========================================================================
    [[nodiscard]] std::size_t num_workers() const noexcept;
    [[nodiscard]] std::size_t num_waiters() const noexcept;
    [[nodiscard]] std::size_t num_queues() const noexcept;
    [[nodiscard]] std::size_t num_topologies() const noexcept;

private:
    struct alignas(2 * cache_line_size) Buffer {
        std::mutex mutex;
        UnboundedQueue<Work*> queue{2 * TFL_DEFAULT_QUEUE_SIZE};
    };

    // 64 字节对齐：防止多线程修改 m_num_topologies 时产生伪共享
    alignas(2 * cache_line_size) std::atomic<std::size_t> m_num_topologies{0};

    std::vector<Worker>                             m_workers;
    std::vector<Buffer>                             m_shared_buffers; ///< 队列缓冲区数组
    Notifier                                        m_notifier;
    WorkerHandler&                                  m_handler;
    unordered_dense::map<std::thread::id, Worker*>  m_thread_worker_map;

    void _spawn(std::size_t num_workers);
    void _shutdown() noexcept;
    void _invoke(Worker& wr, Work* w);

    /// @brief 等待并获取可执行任务
    /// @algorithm 三阶段策略：
    ///   1. 自旋与窃取：本地队列空，尝试从其他队列窃取
    ///   2. 退避让出：多次窃取失败，自适应退让
    ///   3. 阻塞等待：无可用任务，阻塞等待唤醒
    [[nodiscard]] Work* _wait_for_work(Worker& wr) noexcept;

    [[nodiscard]] std::size_t _set_up_graph(Graph& g, Topology* topo, Work* parent) noexcept;

    /// @brief 任务完成后的后处理
    /// @details
    /// 1. 恢复 join_counter（支持循环图）
    /// 2. 检查后继任务是否满足执行条件
    /// 3. 链式调度：满足条件的后继直接放入 cache，避免队列操作
    void _tear_down_task(Work* w, Worker& wr, Work*& cache);

    void _tear_down_branch_task(Work* w, Worker& wr, Work*& cache, Work* target);

    void _tear_down_multi_branch_task(Work* w, Worker& wr, Work*& cache, const SmallVector<Work*>& targets);

    /// @brief Jump 任务完成后强制触发目标节点
    void _tear_down_jump_task(Work* w, Worker& wr, Work*& cache, Work* target);

    /// @brief MultiJump 任务完成后触发多个目标节点
    void _tear_down_multi_jump_task(Work* w, Worker& wr, Work*& cache, const SmallVector<Work*>& targets);

    /// @brief 动态依赖任务完成处理
    void _tear_down_async_task(Work* w, Worker& wr, Work*& cache);

    /// @brief 动态任务依赖设置
    template <typename I, typename S>
        requires std::sentinel_for<S, I>
    void _process_dependent_async(Work* w, I first, S last, std::size_t& num_predecessors);

    /// @brief 动态依赖任务完成处理
    void _tear_down_dep_async_task(Work* w, Worker& wr, Work*& cache);

    void _push_shared(Work* val);

    template <std::random_access_iterator Iterator>
        requires std::convertible_to<std::iter_reference_t<Iterator>, Work*>
    void _push_shared(Iterator first, std::size_t n);

    // 任务调度入口
    template <std::random_access_iterator Iterator>
    void _schedule(Worker& wr, Iterator first, std::size_t n);
    template <std::random_access_iterator Iterator>
    void _schedule(Iterator first, std::size_t n);
    void _schedule(Worker& wr, Work* w);
    void _schedule(Work* w);

    /// @brief 父任务完成处理（PREEMPTED 机制）
    void _schedule_parent(Work* parent, Worker& wr, Work*& cache);

    void _schedule_from_semaphore(Worker& w, SmallVector<Work*>& waiters);

    /// @brief 协作式等待：等待条件满足期间继续执行其他任务
    template <predicate Pred>
    void _cowait_until(Worker& wr, Pred&& pred);

    /// @brief 展开执行图并协作式等待完成
    void _cowait_graph(Worker& wr, Graph& g, Work* parent);

    void _increment_topology() noexcept;
    void _decrement_topology() noexcept;

    /// @brief 获取当前线程对应的 Worker
    [[nodiscard]] Worker* _this_worker();

};

// ============================================================================
// Executor 生命周期与查询
// ============================================================================

inline Executor::Executor(WorkerHandler& handler, std::size_t num_workers)
    : m_workers{num_workers}
    , m_shared_buffers{static_cast<std::size_t>(std::bit_width(num_workers))}
    , m_notifier{num_workers}
    , m_handler{handler} {
    if (num_workers == 0) {
        throw Exception("Executor must define at least one worker.");
    }
    _spawn(num_workers);
}

inline Executor::~Executor() noexcept {
    _shutdown();
}

/// @brief 阻塞等待所有拓扑任务完成
///
/// @memory_order
/// - load(acquire): 获取当前活跃拓扑计数
/// - wait: 原子等待，底层使用 OS 挂起机制（futex/condvar）
/// - notify_all: 由 _decrement_topology 触发
inline void Executor::wait_for_all() const noexcept {
    std::size_t n = m_num_topologies.load(std::memory_order_acquire);
    while (n != 0) {
        m_num_topologies.wait(n, std::memory_order_acquire);
        n = m_num_topologies.load(std::memory_order_acquire);
    }
}

inline std::size_t Executor::num_workers() const noexcept {
    return m_workers.size();
}

inline std::size_t Executor::num_waiters() const noexcept {
    return m_notifier.num_waiters();
}

inline std::size_t Executor::num_queues() const noexcept {
    return m_workers.size() + m_shared_buffers.size();
}

inline std::size_t Executor::num_topologies() const noexcept {
    return m_num_topologies.load(std::memory_order_relaxed);
}

/// @brief 优雅关闭：等待任务完成 → 设置终止标志 → 唤醒等待线程 → 回收线程资源
inline void Executor::_shutdown() noexcept {
    wait_for_all();

    for (auto& wr : m_workers) {
        // Why: release 保证 worker 端的 acquire/relaxed load 能看到 terminate flag，
        // 避免 ARM/PowerPC 等弱一致性架构上 worker 无限期看不到终止信号。
        wr.m_terminate.test_and_set(std::memory_order_release);
    }

    m_notifier.notify_all();

    for (auto& w : m_workers) {
        if (w.m_thread.joinable()) {
            w.m_thread.join();
        }
    }
}


// ============================================================================
//  工作窃取调度核心
// ============================================================================

/// @brief 工作线程启动入口
///
/// @par 核心调度循环
/// 1. 本地队列 pop → 执行（LIFO，缓存友好）
/// 2. 窃取其他队列 → 执行（FIFO，负载均衡）
/// 3. 无可用任务 → 阻塞等待
inline void Executor::_spawn(std::size_t num_workers) {
    for (std::size_t id = 0; id < num_workers; ++id) {
        auto& wr = m_workers[id];
        wr.m_id = id;
        wr.m_vtm = id;
        wr.m_adaptive_factor = 4;
        wr.m_max_steals = static_cast<std::uint32_t>(num_queues() * 2);
        wr.m_thread = std::thread([this, &wr]() noexcept {
            wr.m_rng.seed(std::hash<std::thread::id>{}(std::this_thread::get_id()), static_cast<std::uint32_t>(num_queues()));
            m_handler.on_start(wr);

            Work* w = nullptr;
            for (;;) {
                // 本地队列优先：LIFO 顺序，缓存命中率最高
                while (w) {
                    try {
                        _invoke(wr, w);
                    } catch (...) {
                        if (!m_handler.on_exception(wr, std::current_exception())) {
                            goto exit;
                        }
                    }
                    w = wr.m_wslq.pop();
                }

                // 窃取阶段
                w = _wait_for_work(wr);

                if (wr.m_terminate.test(std::memory_order_acquire)) [[unlikely]] {
                    break;
                }
            }
        exit:
            m_handler.on_stop(wr);
        });

        m_thread_worker_map.emplace(wr.m_thread.get_id(), std::addressof(wr));
    }
}

/// @brief 工作窃取与阻塞等待
///
/// @par 三阶段策略
/// 1. 自旋与窃取: 随机选择 victim 队列尝试窃取
/// 2. 退避让出: 连续失败后让出 CPU
/// 3. 阻塞等待: 彻底无任务时阻塞等待
///
/// @memory_order
/// - steal(): acquire 读取队列状态
/// - notify_one: release 唤醒等待线程
inline Work* Executor::_wait_for_work(Worker& wr) noexcept {
    std::size_t const nw = m_workers.size();
    std::size_t const nb = m_shared_buffers.size();

explore:
    std::size_t vtm = wr.m_vtm;
    std::size_t num_steals = 0;
    std::size_t const yield_limit = nw * wr.m_adaptive_factor + wr.m_max_steals;

    // 阶段一：窃取
    for (;;) {
        Work* w = (vtm < nw)
        ? m_workers[vtm].m_wslq.steal()
        : m_shared_buffers[vtm - nw].queue.steal();

        if (w) {
            wr.m_vtm = vtm;
            wr.m_adaptive_factor = std::min(8u, wr.m_adaptive_factor + 1);
            return w;
        }

        // 阶段二：退让
        if (++num_steals > wr.m_max_steals) {
            std::this_thread::yield();
            if (num_steals > yield_limit) {
                wr.m_adaptive_factor = std::max(1u, wr.m_adaptive_factor - 1);
                break;
            }
        }

        if (wr.m_terminate.test(std::memory_order_acquire)) [[unlikely]] {
            return nullptr;
        }

        vtm = wr.m_rng();
    }

    // 阶段三：阻塞等待
    m_notifier.prepare_wait(wr.m_id);

    // 二次确认：唤醒后再次检查队列
    for (std::size_t i = 0; i < nb; ++i) {
        if (!m_shared_buffers[i].queue.empty()) {
            m_notifier.cancel_wait(wr.m_id);
            wr.m_vtm = i + nw;
            goto explore;
        }
    }

    for (std::size_t i = 0; i < wr.m_id; ++i) {
        if (!m_workers[i].m_wslq.empty()) {
            m_notifier.cancel_wait(wr.m_id);
            wr.m_vtm = i;
            goto explore;
        }
    }

    for (std::size_t i = wr.m_id + 1; i < nw; ++i) {
        if (!m_workers[i].m_wslq.empty()) {
            m_notifier.cancel_wait(wr.m_id);
            wr.m_vtm = i;
            goto explore;
        }
    }

    if (wr.m_terminate.test(std::memory_order_acquire)) [[unlikely]] {
        m_notifier.cancel_wait(wr.m_id);
        return nullptr;
    }

    m_notifier.commit_wait(wr.m_id);
    goto explore;
}

/// @brief 重置子图所有节点到下一轮执行的初始状态。
///
/// @par 执行步骤（每个节点）
///   -# 注入运行时上下文（topology / parent）
///   -# 清空异常路径标记（@c EXCEPTION + @c CAUGHT），保留其他位（如 @c ANCHORED）
///   -# 仅在该节点上轮归档过异常时（@c CAUGHT 位为 1）清空 @c m_exception_ptr
///   -# 重新计算入度并写入 @c m_join_counter（屏障值）
///   -# 零入度节点 swap 到 @c data 前段，便于调用方批量调度
///
/// @par 异常位清理策略
/// 使用单次 @c fetch_and 同时完成"清位"和"读旧 CAUGHT 位"两件事——
/// 比 @c load + @c fetch_and 两次原子操作省一次 RMW，但代价是
/// **每个节点都会触发一次原子写**，可能让该 cache line 在多核间反弹。
///
/// 当前选择 @c fetch_and 是因为：
///   - 节点 set_up 通常发生在 owner 线程独占阶段（图重置时无并发执行），
///     cache line 反弹概率低；
///   - 异常归档是冷路径，避免传播路径节点的残留 @c EXCEPTION 位污染下轮
///     状态判断（如 @c _should_abort）必须无条件清位。
///
/// @param g       目标子图
/// @param topo    本轮归属的拓扑上下文
/// @param parent  父节点指针（嵌套图内部子节点指向外层抢占节点）
/// @return        零入度源节点数量；调用方通常按此数量批量 schedule data 前段
TFL_FORCE_INLINE std::size_t Executor::_set_up_graph(Graph& g, Topology* topo, Work* parent) noexcept {
    Work** const data = g.m_works.data();
    std::size_t const size = g.m_works.size();
    std::size_t n = 0;

    constexpr auto exc_mask = Work::Explicit::EXCEPTION | Work::Explicit::CAUGHT;
    for (std::size_t i = 0; i < size; ++i) {
        Work* w = data[i];
        w->m_topology = topo;
        w->m_parent = parent;


        // 清异常位 + 探测是否为上轮归档点
        // 单次 fetch_and 同时承担"清位"和"读旧值判断"两个职责
        // 仅归档点（CAUGHT == 1）需要释放 m_exception_ptr；
        // 路径节点（仅 EXCEPTION == 1）的 m_exception_ptr 本就为空
        if (w->m_explicit.fetch_and(~exc_mask, std::memory_order_relaxed) & Work::Explicit::CAUGHT) [[unlikely]] {
            w->m_exception_ptr = nullptr;
        }

        // 重新计算入度（前驱权重之和）并初始化 join_counter 屏障
        w->m_join_weight = w->_join_weight();
        w->m_join_counter.store(w->m_join_weight, std::memory_order_relaxed);

        // 零入度节点 swap 到前段 [0, n)
        // 调用方按返回值 n 批量调度这段连续区间
        if (w->_num_predecessors() == 0) {
            std::swap(data[i], data[n++]);
        }
    }
    return n;
}

/// @brief 任务完成后的依赖传播 —— 通过 cache 接力实现零调度开销的串行链。
///
/// @par 不变量(slot 守恒)
/// w 在被调度时已占 parent 的 1 个 slot。本函数退出前必须做到二选一:
///   1) 把这个 slot 平移给某个就绪后继(令其继承,放入 cache 接力执行);
///   2) 通过 _schedule_parent 把 slot 归还给 parent。
/// 二者只能选其一,否则 parent counter 账本会失衡 → topology 永远完不成。
///
/// @par 路径分发
/// - 异常路径:不向后传播,直接归还 slot
/// - 串行快路径(sz==1 且 suc 入度==1):store(0) 强制就绪 + cache 平移
/// - 通用 fan-out:逐边 fetch_sub,就绪者轮流占据 cache,最后留在 cache 的
///   那个继承 w 的 slot,其余被挤出者各自 fetch_add 占新 slot 后推入队列
/// - 兜底(ready==0,常见于 fan-in 中段):cache 仍为空,归还 slot
TFL_FORCE_INLINE void Executor::_tear_down_task(Work* w, Worker& wr, Work*& cache) {
    // 还原 w 自身 counter:供循环子图 / Jump 目标复用,counter 簿记不可省
    w->m_join_counter.fetch_add(w->m_join_weight, std::memory_order_relaxed);

    auto* const parent = w->m_parent;
    const std::size_t sz = w->m_num_successors;

    // 无后继 或 异常:不向后传播,直接归还 w 占用的 parent slot
    if (sz == 0 || w->_has_exception()) [[unlikely]] {
        _schedule_parent(parent, wr, cache);
        return;
    }

    // ── 单后继路径 ──
    // 1) 若 suc 静态入度为 1,则当前 w 是其唯一前驱。
    //    w 完成后 suc 必然 ready,可直接 store(0) + cache 接力。
    // 2) 若 suc 静态入度 > 1,则必须走正常 fetch_sub 计数协议。
    if (sz == 1) [[unlikely]] {
        Work* const suc = w->m_edges[0];

        if (suc->_num_predecessors() == 1) [[unlikely]] {
            suc->m_join_counter.store(0, std::memory_order_relaxed);
            cache = suc;
            return;
        }

        if (suc->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            cache = suc;
            return;
        }

        _schedule_parent(parent, wr, cache);
        return;
    }

    // ── 通用 fan-out ──
    // 不变量:第一个 ready 的 suc 继承 w 的 parent slot;
    //          后续 ready 的 suc 走 _schedule,各自占新 slot。
    for (std::size_t i = 0; i < sz; ++i) {
        Work* const suc = w->m_edges[i];

        if (suc->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (cache) {
                parent->m_join_counter.fetch_add(1, std::memory_order_relaxed);
                _schedule(wr, suc);
            } else {
                cache = suc;
            }
        }
    }

    // ready == 0:没人接班,w 占用的 parent slot 必须归还
    if (!cache) [[unlikely]] {
        _schedule_parent(parent, wr, cache);
    }
}

TFL_FORCE_INLINE void Executor::_tear_down_branch_task(Work* w, Worker& wr, Work*& cache, Work* target) {
    // 还原 w 自身 counter:为下一轮触发(循环子图 / 再调度)做准备
    w->m_join_counter.fetch_add(w->m_join_weight, std::memory_order_relaxed);
    auto* parent = w->m_parent;
    // 异常 或 无 target
    if (target == nullptr || w->_has_exception()) [[unlikely]] {
        _schedule_parent(parent, wr, cache);
        return;
    }

    // Branch 串行接力快路径:
    // target 只有当前 w 一个前驱,命中后必然 ready
    if (target->_num_predecessors() == 1) [[unlikely]] {
        target->m_join_counter.store(0, std::memory_order_relaxed);
        cache = target;
        return;
    }

    // 通用路径:target 可能还有其他前驱,必须走计数协议
    if (target->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        cache = target;
        return;
    }
    _schedule_parent(parent, wr, cache);
}


TFL_FORCE_INLINE void Executor::_tear_down_multi_branch_task(Work* w, Worker& wr, Work*& cache, const SmallVector<Work*>& targets) {
    w->m_join_counter.fetch_add(w->m_join_weight, std::memory_order_relaxed);
    auto* parent = w->m_parent;

    const std::size_t n = targets.size();
    // 无目标 或 异常:归还 slot
    if (n == 0 || w->_has_exception()) [[unlikely]] {
        _schedule_parent(parent, wr, cache);
        return;
    }

    // ── 单目标快路径 ──
    // MultiBranch 本次只命中 1 个 target,且 target 静态入度为 1,
    // 说明这个 target 只有当前 w 一个前驱,本次必然 ready。
    if (n == 1) [[unlikely]] {
        Work* const target = targets[0];

        if (target->_num_predecessors() == 1) [[unlikely]] {
            // target 入度为 1 → 本次 必然归零
            // 直接 store(0),省一次 acq_rel 原子递减。
            target->m_join_counter.store(0, std::memory_order_relaxed);
            cache = target;
            return;
        }

        // 单目标但 target 还有其他前驱,必须走正常计数协议。
        if(target->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            cache = target;
            return;
        }

        _schedule_parent(parent, wr, cache);
        return;
    }

    // ── 多目标通用路径 ──
    // 不变量:第一个 ready 的 target 继承 w 的 parent slot;
    //          后续 ready 的 target 走 _schedule,各自占新 slot。
    for (auto* target : targets) {
        if(target->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (cache) {
                parent->m_join_counter.fetch_add(1, std::memory_order_relaxed);
                _schedule(wr, target);
            } else {
                cache = target;
            }
        }
    }

    // ready == 0,没人接班,w 占的 slot 必须归还
    if (!cache) [[unlikely]] {
        _schedule_parent(parent, wr, cache);
    }
}

/// @brief Jump 任务的强制跳转处理 —— 单目标特权通道。
///
/// @par 设计意图
/// Jump 是"特权通道",运行期由用户在 invoker body 内通过 `Jump&` 选定 target,
/// 跳过图的静态依赖直接触发它。通过 store(0, relaxed) 把 target 的 join_counter
/// 强制清零实现 —— 等价于"所有前驱已到齐",绕开正常的 fetch_sub 计数协议。
///
/// @par 路径分发
/// - 异常路径(运行期偶发):不让 target 接力,与 _tear_down_task 异常语义一致
/// - 无 target(用户没在 Jump& 上设值):语义等同普通完成
/// - 上述两条共用慢路径:归还 w 占的 parent slot
/// - 快路径:store(0) 强制触发 target,slot 平移 w → target
TFL_FORCE_INLINE void Executor::_tear_down_jump_task(Work* w, Worker& wr, Work*& cache, Work* target) {
    // 还原 w 自身 counter:为下一轮触发(循环子图 / 再调度)做准备
    w->m_join_counter.fetch_add(w->m_join_weight, std::memory_order_relaxed);

    // 异常 或 无 target
    if (target == nullptr || w->_has_exception()) [[unlikely]] {
        _schedule_parent(w->m_parent, wr, cache);
        return;
    }

    // 强制触发:store(0) 等价于"所有前驱都到齐",绕过 fetch_sub 协议
    // slot 平移 w → target,parent counter 不动,本 worker 通过 cache 接力执行
    target->m_join_counter.store(0, std::memory_order_relaxed);
    cache = target;
}


/// @brief MultiJump 任务的多目标强制跳转处理 —— Jump 的 N 路扩展。
///
/// @par 路径分发
/// - n == 0:用户没 push 任何 target,等同普通完成,归还 slot
/// - 异常:与普通 tear_down 一致,归还 slot
/// - n == 1:退化为单 Jump,slot 平移,parent counter 不动
/// - n >= 2:前 n-1 个 target 各占一个新 parent slot,最后一个继承 w 的 slot
///
TFL_FORCE_INLINE void Executor::_tear_down_multi_jump_task(Work* w, Worker& wr, Work*& cache, const SmallVector<Work*>& targets) {
    w->m_join_counter.fetch_add(w->m_join_weight, std::memory_order_relaxed);
    auto* parent = w->m_parent;

    // 无目标 或 异常:归还 slot
    if (targets.empty() || w->_has_exception()) [[unlikely]] {
        _schedule_parent(parent, wr, cache);
        return;
    }

    // ── 通用路径(覆盖 n == 1 和 n >= 2)──
    // 不变量:最后留在 cache 的 target 继承 w 的 parent slot;
    //          其余被挤出 cache 的 target 走 _schedule,各自占新 slot
    // - n == 1: cache 入口为 nullptr,if (cache) 跳过,直接 cache = target
    //           等价于"slot 平移",parent counter 不动
    // - n >= 2: 前 n-1 次 fetch_add 占新 slot,最后一次平移 w 的 slot
    //           parent counter 净增 (n-1),严格变大,不会归零
    for (auto* target : targets) {
        target->m_join_counter.store(0, std::memory_order_relaxed);
        if (cache) {
            // 之前的 cache 让位推队列,为它新占一个 parent slot
            parent->m_join_counter.fetch_add(1, std::memory_order_relaxed);
            _schedule(wr, target);
        } else {
            cache = target;
        }

    }
    // n >= 1 时循环至少进 1 次,cache 必非空,slot 已平移(n==1)或累计 (n-1) 个新 slot
    // 不需要 _schedule_parent
}

TFL_FORCE_INLINE void Executor::_tear_down_async_task(Work* w, Worker& wr, Work*& cache) {

    if(auto parent = w->m_parent; parent) {
        _schedule_parent(parent, wr, cache);
    } else {
        _decrement_topology();
    }
    destroy(w);
}

// ============================================================================
//  动态依赖任务处理
// ============================================================================

/// @brief 设置外部依赖的动态任务
///
/// @par 同步协议
/// 使用 Topology::State 的 CAS 状态机防止竞争：
/// - Running → Locking: 尝试加锁
/// - Locking → Running: 添加依赖边
/// - Finished: 目标已完，无需添加依赖
template <typename I, typename S>
    requires std::sentinel_for<S, I>
inline void Executor::_process_dependent_async(Work* w, I first, S last, std::size_t& num_predecessors) {

    if (w->m_parent == nullptr) {
        _increment_topology();
    }

    for (; first != last; ++first) {
        auto* work = first->m_work;
        if (!work) {
            num_predecessors = w->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) - 1;
            continue;
        }

        auto& state = work->m_topology->m_state;

        for (;;) {
            // 1. 每次循环开头，直接获取内存中的最新状态
            auto target = state.load(std::memory_order_acquire);

            // 2. 目标已完成，直接跳出
            if (target == Topology::State::Finished) {
                num_predecessors = w->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) - 1;
                break;
            }

            // 3. 锁被占用：我们只自旋等待，坚决不触发 CAS 操作（减少总线竞争）
            if (target == Topology::State::Locking) {
                continue;
            }

            // 4. 此时 target 必然是 Idle 或 Running，尝试原子加锁
            if (state.compare_exchange_weak(target, Topology::State::Locking,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
                // 加锁成功！当前线程独占修改权限
                work->m_edges.push_back(w);
                ++work->m_num_successors;

                // 解锁并恢复为原状态 (target 中存的是替换前的 Idle 或 Running)
                state.store(target, std::memory_order_release);
                break;
            }
            // 5. 如果加锁失败，说明恰好有其他线程抢先了。
            // 循环会回到开头，重新 load 最新状态，完美闭环！
        }
    }
}

/// @brief 动态依赖任务完成处理
inline void Executor::_tear_down_dep_async_task(Work* w, Worker& wr, Work*& cache) {
    auto* topo = w->m_topology;

    auto target = Topology::State::Running;
    // 状态转移：Running → Finished
    while (!topo->m_state.compare_exchange_weak(target, Topology::State::Finished,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
        target = Topology::State::Running;
    }

    // 唤醒等待者
    topo->m_state.notify_all();

    const std::size_t sz = w->m_num_successors;
    for (std::size_t i = 0; i < sz; ++i) {
        auto* suc = w->m_edges[i];
        if ((suc->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1)) {
            auto& suc_exec = suc->m_topology->m_executor;
            if (std::addressof(suc_exec) == this) {
                if (cache) {
                    _schedule(wr, suc);
                } else {
                    cache = suc;
                }
            } else {
                // 跨调度器调度
                suc_exec._schedule(suc);
            }
        }
    }

    // parent == nullptr：顶级 async，由 topology 计数追踪生命周期
    // parent != nullptr：嵌套 async，由 parent 的 join_counter 追踪，topology 不介入
    if (auto parent = w->m_parent; parent) {
        _schedule_parent(parent, wr, cache);
    } else {
        _decrement_topology();
    }

    if (topo->_decref()) {
        destroy(w);
    }
}


inline void Executor::_push_shared(Work* val) {
    std::size_t const size = m_shared_buffers.size();
    std::size_t const b = detail::mulhi64(reinterpret_cast<std::uintptr_t>(val) * 11400714819323198485ULL, size);

    // 快路径
    for (std::size_t curr_b = b; curr_b < size; ++curr_b) {
        auto& buf = m_shared_buffers[curr_b];
        if (buf.mutex.try_lock()) {
            buf.queue.push(val);
            buf.mutex.unlock();
            return;
        }
    }
    for (std::size_t curr_b = 0; curr_b < b; ++curr_b) {
        auto& buf = m_shared_buffers[curr_b];
        if (buf.mutex.try_lock()) {
            buf.queue.push(val);
            buf.mutex.unlock();
            return;
        }
    }

    // 直接对目标队列加锁并推送，如果被占用则当前线程阻塞等待
    std::lock_guard<std::mutex> lock(m_shared_buffers[b].mutex);
    m_shared_buffers[b].queue.push(val);
}

template <std::random_access_iterator Iterator>
    requires std::convertible_to<std::iter_reference_t<Iterator>, Work*>
inline void Executor::_push_shared(Iterator first, std::size_t n) {
    std::size_t const size = m_shared_buffers.size();
    std::size_t const b = detail::mulhi64(reinterpret_cast<std::uintptr_t>(*first) * 11400714819323198485ULL, size);

    // 快路径
    for (std::size_t curr_b = b; curr_b < size; ++curr_b) {
        auto& buf = m_shared_buffers[curr_b];
        if (buf.mutex.try_lock()) {
            buf.queue.push(first, n);
            buf.mutex.unlock();
            return;
        }
    }
    for (std::size_t curr_b = 0; curr_b < b; ++curr_b) {
        auto& buf = m_shared_buffers[curr_b];
        if (buf.mutex.try_lock()) {
            buf.queue.push(first, n);
            buf.mutex.unlock();
            return;
        }
    }

    // 直接对目标队列加锁并批量推送
    std::lock_guard<std::mutex> lock(m_shared_buffers[b].mutex);
    m_shared_buffers[b].queue.push(first, n);
}

// ============================================================================
//  任务调度入口
// ============================================================================

template <std::random_access_iterator Iterator>
inline void Executor::_schedule(Worker& wr, Iterator first, std::size_t n) {
    if (n == 0) [[unlikely]] {
        return;
    }
    // 本地队列满时溢出到共享队列
    wr.m_wslq.push(first, n, [&](Iterator remaining, std::size_t count) {
        _push_shared(remaining, count);
    });

    m_notifier.notify_n(n);
}

template <std::random_access_iterator Iterator>
inline void Executor::_schedule(Iterator first, std::size_t n) {
    if (n == 0) [[unlikely]] {
        return;
    }
    _push_shared(first, n);
    m_notifier.notify_n(n);
}

inline void Executor::_schedule(Worker& wr, Work* w) {
    wr.m_wslq.push(w, [&]() {
        _push_shared(w);
    });
    m_notifier.notify_one();
}

inline void Executor::_schedule(Work* w) {
    _push_shared(w);
    m_notifier.notify_one();
}

/// @brief 父任务完成处理（PREEMPTED 机制）
///
/// @par 设计
/// 当子任务全部完成时：
/// - 普通父任务：等待后续调度
/// - PREEMPTED 父任务：直接插入 cache 继续执行（抢占执行权）
TFL_FORCE_INLINE void Executor::_schedule_parent(Work* parent, Worker& wr, Work*& cache) {
    if (parent->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (parent->m_implicit & Work::Implicit::PREEMPTED) {
            if (cache) {
                _schedule(wr, cache);
            }
            cache = parent;
        }
    }
}

/// @brief 将信号量释放/回滚产生的 waiters 批量重新入队
/// @param w        当前 worker (用于同 executor 的本地快路径)
/// @param waiters  待调度的 work 集合
///
/// @details
/// 按 waiter 归属的 executor 分派:
/// - 同本 executor: 走 worker-local deque 快路径
/// - 跨 executor:   推送到目标 executor 全局队列
///
/// 调用后 waiters 内容语义上消费完毕,但不主动 clear,
/// 由调用方根据生命周期决定是否复用容器
TFL_FORCE_INLINE void Executor::_schedule_from_semaphore(Worker& w, SmallVector<Work*>& waiters) {
    for (Work* t : waiters) {
        auto& target = t->m_topology->m_executor;
        if (std::addressof(target) == this) [[likely]] {
            _schedule(w, t);            // 同 executor 走 worker-local 快路径
        } else {
            target._schedule(t);        // 跨 executor 走全局慢路径
        }
    }
}
// ============================================================================
//  协作式等待
// ============================================================================

/// @brief 协作式等待：等待谓词满足期间继续执行其他任务
///
/// @par 用途
/// - Runtime::wait_until: 等待其他任务完成
/// - 避免线程阻塞导致死锁
template <predicate Pred>
inline void Executor::_cowait_until(Worker& wr, Pred&& pred) {
    while (!std::invoke_r<bool>(pred)) {
        if (auto* w = wr.m_wslq.pop()) [[likely]] {
            _invoke(wr, w);
            continue;
        }

        std::size_t const nw = m_workers.size();
        std::size_t num_steals = 0;
        std::size_t const yield_limit = nw * wr.m_adaptive_factor + wr.m_max_steals;
        std::size_t vtm = wr.m_vtm;

        while (!std::invoke_r<bool>(pred)) {
            Work* w = (vtm < nw)
            ? m_workers[vtm].m_wslq.steal()
            : m_shared_buffers[vtm - nw].queue.steal();;

            if (w) [[likely]] {
                wr.m_vtm = vtm;
                wr.m_adaptive_factor = std::min(8u, wr.m_adaptive_factor + 1);
                _invoke(wr, w);
                break;
            }

            if (++num_steals > wr.m_max_steals) [[unlikely]] {
                std::this_thread::yield();
                if (num_steals > yield_limit) [[unlikely]] {
                    wr.m_adaptive_factor = std::max(1u, wr.m_adaptive_factor - 1);
                    break;
                }
            }

            vtm = wr.m_rng();
        }
    }
}


inline void Executor::_cowait_graph(Worker& wr, Graph& g, Work* parent) {
    auto num_srcs = _set_up_graph(g, parent->m_topology, parent);
    if(num_srcs == 0) {
        return;
    }
    parent->m_join_counter.fetch_add(num_srcs, std::memory_order_relaxed);
    _schedule(wr, g.begin(), num_srcs);

    _cowait_until(wr, [parent]() noexcept { return parent->m_join_counter.load(std::memory_order_acquire) == 0; });
}

// ============================================================================
//  拓扑计数管理
// ============================================================================

/// @brief 活跃拓扑计数管理
///
/// @memory_order
/// - fetch_add(relaxed): 仅计数，无同步需求
/// - fetch_sub(acq_rel): 减至 0 时需唤醒等待线程
inline void Executor::_increment_topology() noexcept {
    m_num_topologies.fetch_add(1, std::memory_order_relaxed);
}

inline void Executor::_decrement_topology() noexcept {
    if (m_num_topologies.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        m_num_topologies.notify_all();
    }
}

inline Worker* Executor::_this_worker() {
    auto itr = m_thread_worker_map.find(std::this_thread::get_id());
    return itr == m_thread_worker_map.end() ? nullptr : itr->second;
}

/// @brief 任务执行入口（链式执行优化）
///
/// @par 链式执行
/// 任务完成后可能产生满足执行条件的后继（放入 cache）。
/// 这里直接执行 cache 中的任务，避免再次入队出队的开销。
TFL_FORCE_INLINE void Executor::_invoke(Worker& wr, Work* w) {
    do {
        Work* cache{nullptr};
        w->invoke(*this, wr, cache);
        w = cache;
    } while (w);
}

} // namespace tfl
