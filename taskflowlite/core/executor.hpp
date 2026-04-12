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
#include "async_task.hpp"
#include "runtime.hpp"
#include "branch.hpp"
#include "jump.hpp"
#include "graph.hpp"
#include "worker.hpp"
#include "unordered_dense.hpp"
#include "unbounded_queue.hpp"

namespace tfl {

/// @brief 任务调度器核心 - Work-Stealing 并行执行引擎
///
/// @details
/// 负责管理工作线程池并执行任务调度。采用 Work-Stealing 算法实现高效的负载均衡：
/// - 本地队列：每个 Worker 线程拥有独立的无锁有界队列（LIFO）用于本地任务执行
/// - 共享队列：全局无界队列（FIFO）用于任务分发和跨线程负载均衡
/// - 通知机制：基于原子等待的防丢失唤醒机制（Notifier）
///
/// @par 核心调度流程
/// @code
/// while (!terminate) {
///     // 1. 本地队列优先执行
///     while (w = local.pop()) invoke(w);
///
///     // 2. 工作窃取（Steal）
///     w = steal_from_other_queues();
///     if (w) { invoke(w); continue; }
///
///     // 3. 阻塞等待（Park）
///     wait_for_work();
/// }
/// @endcode
///
/// @note 线程安全：此类为线程安全类，可被多个线程同时调用（submit/wait_for_all）
class Executor : public Immovable<Executor> {
    friend class Work;
    friend class Flow;
    friend class AsyncTask;
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
    template <typename F>
        requires flow_type<F>
    AsyncTask submit(F&& flow);

    /// @brief 提交任务图执行一次，完成后执行回调
    template <typename F, typename C>
        requires (capturable<C> && flow_type<F> && callback<C>)
    AsyncTask submit(F&& flow, C&& callback);

    /// @brief 提交任务图执行指定次数
    template <typename F>
        requires flow_type<F>
    AsyncTask submit(F&& flow, std::uint64_t num);

    /// @brief 提交任务图循环执行指定次数，完成后执行回调
    template <typename F, typename C>
        requires (capturable<C> && flow_type<F> && callback<C>)
    AsyncTask submit(F&& flow, std::uint64_t num, C&& callback);

    /// @brief 提交任务图条件循环执行
    template <typename F, typename P>
        requires (capturable<P> && flow_type<F> && predicate<P>)
    AsyncTask submit(F&& flow, P&& pred);

    /// @brief 提交任务图条件循环执行，完成后执行回调
    template <typename F, typename P, typename C>
        requires (capturable<P, C> && flow_type<F> && predicate<P> && callback<C>)
    AsyncTask submit(F&& flow, P&& pred, C&& callback);

    /// @brief 提交单个异步任务
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    AsyncTask submit(T&& task, Args&&... args);

    /// @brief 提交单个运行时任务（可动态操纵图结构）
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    AsyncTask submit(T&& task, Args&&... args);

    /// @brief Fire-and-forget 异步执行
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    void silent_async(T&& task, Args&&... args);

    /// @brief Fire-and-forget 运行时任务
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    void silent_async(T&& task, Args&&... args);

    /// @brief 异步执行并返回 std::future
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    auto async(T&& task, Args&&... args) -> std::future<basic_return_t<T, Args...>>;

    /// @brief 异步执行运行时任务并返回 std::future
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    auto async(T&& task, Args&&... args) -> std::future<runtime_return_t<T, Args...>>;

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
    alignas(2 * cache_line_size)
        std::atomic<std::size_t> m_num_topologies{0};

    std::vector<Worker> m_workers;
    std::vector<Buffer> m_buffers; ///< 队列缓冲区数组
    Notifier m_notifier;
    WorkerHandler& m_handler;
    unordered_dense::map<std::thread::id, Worker*> m_thread_worker_map;

    void _spawn(std::size_t num_workers);
    void _shutdown() noexcept;
    void _invoke(Worker& wr, Work* w);

    /// @brief 等待并获取可执行任务
    /// @algorithm 三阶段策略：
    ///   1. Spin & Steal：本地队列空，尝试从其他队列窃取
    ///   2. Yield Backoff：多次窃取失败，自适应退让
    ///   3. Park & Wait：无可用任务，阻塞等待唤醒
    [[nodiscard]] Work* _wait_for_work(Worker& wr) noexcept;

    [[nodiscard]] std::size_t _set_up_graph(Graph& g, Topology* topo, Work* parent);

    /// @brief 任务完成后的后处理
    /// @details
    /// 1. 恢复 join_counter（支持循环图）
    /// 2. 检查后继任务是否满足执行条件
    /// 3. 链式调度：满足条件的后继直接放入 cache，避免队列操作
    void _tear_down_task(Work* w, Worker& wr, Work*& cache);

    /// @brief Jump 任务完成后强制触发目标节点
    void _tear_down_jump_task(Work* w, Worker& wr, Work*& cache, Work* target);

    /// @brief MultiJump 任务完成后触发多个目标节点
    void _tear_down_multi_jump_task(Work* w, Worker& wr, Work*& cache, const SmallVector<Work*>& targets);

    /// @brief 动态任务依赖设置
    template <typename I, typename S>
        requires std::sentinel_for<S, I>
    void _set_up_dep_async_task(Work* w, I first, S last, std::size_t& num_predecessors);

    /// @brief 动态依赖任务完成处理
    void _tear_down_dep_async_task(Work* w, Worker& wr, Work*& cache);

    /// @brief 异常传播处理
    void _process_exception(Work* w);

    void _push_global(Work* val);

    template <std::random_access_iterator Iterator>
        requires std::convertible_to<std::iter_reference_t<Iterator>, Work*>
    void _push_global(Iterator first, std::size_t n);

    // 任务调度入口
    template <std::random_access_iterator Iterator>
    void _schedule(Worker& wr, Iterator first, std::size_t n);
    template <std::random_access_iterator Iterator>
    void _schedule(Iterator first, std::size_t n);
    void _schedule(Worker& wr, Work* w);
    void _schedule(Work* w);

    /// @brief 父任务完成处理（PREEMPTED 机制）
    void _schedule_parent(Work* parent, Worker& wr, Work*& cache);

    /// @brief 协作式等待：等待条件满足期间继续执行其他任务
    template <predicate Pred>
    void _corun_until(Worker& wr, Pred&& pred);

    /// @brief 展开执行图并协作式等待完成
    void _corun_graph(Worker& wr, Graph& g, Work* parent);

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
    , m_buffers{static_cast<std::size_t>(std::bit_width(num_workers))}
    , m_notifier{num_workers}
    , m_handler{handler} {
    if (num_workers == 0) {
        TFL_THROW("Executor must define at least one worker.");
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

inline std::size_t Executor::num_workers() const noexcept { return m_workers.size(); }
inline std::size_t Executor::num_waiters() const noexcept { return m_notifier.num_waiters(); }
inline std::size_t Executor::num_queues() const noexcept { return m_workers.size() + m_buffers.size(); }
inline std::size_t Executor::num_topologies() const noexcept { return m_num_topologies.load(std::memory_order_relaxed); }

/// @brief 优雅关闭：等待任务完成 → 设置终止标志 → 唤醒等待线程 → 回收线程资源
inline void Executor::_shutdown() noexcept {
    wait_for_all();

    for (auto& wr : m_workers) {
        wr.m_terminate.test_and_set(std::memory_order_relaxed);
    }

    m_notifier.notify_all();

    for (auto& w : m_workers) {
        if (w.m_thread.joinable()) {
            w.m_thread.join();
        }
    }
}

// ============================================================================
//  任务提交路由
// ============================================================================

template <typename F>
    requires flow_type<F>
inline AsyncTask Executor::submit(F&& flow) {
    return submit(std::forward<F>(flow), 1ULL);
}

template <typename F, typename C>
    requires (capturable<C> && flow_type<F> && callback<C>)
inline AsyncTask Executor::submit(F&& flow, C&& callback) {
    return submit(std::forward<F>(flow), 1ULL, std::forward<C>(callback));
}

template <typename F>
    requires flow_type<F>
inline AsyncTask Executor::submit(F&& flow, std::uint64_t num) {
    return submit(std::forward<F>(flow), num, []() noexcept {});
}

template <typename F, typename C>
    requires (capturable<C> && flow_type<F> && callback<C>)
inline AsyncTask Executor::submit(F&& flow, std::uint64_t num, C&& callback) {
    // 将次数转换为谓词
    return submit(std::forward<F>(flow)
                  ,[num]() mutable noexcept { return num-- == 0; }
                  ,std::forward<C>(callback));
}

template <typename F, typename P>
    requires (capturable<P> && flow_type<F> && predicate<P>)
inline AsyncTask Executor::submit(F&& flow, P&& pred) {
    return submit(std::forward<F>(flow)
                  ,std::forward<P>(pred)
                  ,[]() noexcept {});
}

template <typename F, typename P, typename C>
    requires (capturable<P, C> && flow_type<F> && predicate<P> && callback<C>)
inline AsyncTask Executor::submit(F&& flow, P&& pred, C&& callback) {
    constexpr auto options = Work::Option::ANCHORED | Work::Option::PREEMPTED;
    Work* work = Work::make_dep_flow(*this, options,
                                     std::forward<F>(flow), std::forward<P>(pred), std::forward<C>(callback));
    return AsyncTask{work};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
inline AsyncTask Executor::submit(T&& task, Args&&... args) {
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_dep_async_basic(*this, options,
                                            std::forward<T>(task), std::forward<Args>(args)...);
    return AsyncTask{work};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
inline AsyncTask Executor::submit(T&& task, Args&&... args) {
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_dep_async_runtime(*this, options,
                                              std::forward<T>(task), std::forward<Args>(args)...);
    return AsyncTask{work};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
inline void Executor::silent_async(T&& task, Args&&... args) {
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_async_basic(*this, options,
                                        std::forward<T>(task), std::forward<Args>(args)...);
    _increment_topology();

    // 线程本地提交：直接放入本地队列（零队列操作）
    if (Worker* wr = _this_worker()) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
inline void Executor::silent_async(T&& task, Args&&... args) {
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_async_runtime(*this, options,
                                          std::forward<T>(task), std::forward<Args>(args)...);
    _increment_topology();
    if (Worker* wr = _this_worker()) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
inline auto Executor::async(T&& task, Args&&... args) -> std::future<basic_return_t<T, Args...>> {
    using R = basic_return_t<T, Args...>;
    std::promise<R> promise;
    auto future = promise.get_future();
    constexpr auto options = Work::Option::ANCHORED;

    Work* work = Work::make_async_basic(*this, options,
                                        std::forward<T>(task), std::move(promise), std::forward<Args>(args)...);
    _increment_topology();
    if (Worker* wr = _this_worker()) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }
    return future;
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
inline auto Executor::async(T&& task, Args&&... args) -> std::future<runtime_return_t<T, Args...>> {
    using R = runtime_return_t<T, Args...>;
    std::promise<R> promise;
    auto future = promise.get_future();
    constexpr auto options = Work::Option::ANCHORED;

    Work* work = Work::make_async_runtime(*this, options,
                                          std::forward<T>(task), std::move(promise), std::forward<Args>(args)...);
    _increment_topology();
    if (Worker* wr = _this_worker()) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }
    return future;
}

// ============================================================================
//  Work-Stealing 调度核心
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

                if (wr.m_terminate.test(std::memory_order_relaxed)) [[unlikely]] {
                    break;
                }
            }
        exit:
            m_handler.on_stop(wr);
        });

        m_thread_worker_map.emplace(wr.m_thread.get_id(), &wr);
    }
}

/// @brief 工作窃取与阻塞等待
///
/// @par 三阶段策略
/// 1. Spin & Steal: 随机选择 victim 队列尝试窃取
/// 2. Yield Backoff: 连续失败后让出 CPU
/// 3. Park & Wait: 彻底无任务时阻塞等待
///
/// @memory_order
/// - steal(): acquire 读取队列状态
/// - notify_one: release 唤醒等待线程
inline Work* Executor::_wait_for_work(Worker& wr) noexcept {
    std::size_t const nw = m_workers.size();
    std::size_t const nb = m_buffers.size();

explore:
    std::size_t vtm = wr.m_vtm;
    std::size_t num_steals = 0;
    std::size_t const yield_limit = nw * wr.m_adaptive_factor + wr.m_max_steals;

    // Phase 1: 窃取
    for (;;) {
        Work* w = (vtm < nw)
        ? m_workers[vtm].m_wslq.steal()
        : m_buffers[vtm - nw].queue.steal();

        if (w) {
            wr.m_vtm = vtm;
            wr.m_adaptive_factor = std::min(8u, wr.m_adaptive_factor + 1);
            return w;
        }

        // Phase 2: 退让
        if (++num_steals > wr.m_max_steals) {
            std::this_thread::yield();
            if (num_steals > yield_limit) {
                wr.m_adaptive_factor = std::max(1u, wr.m_adaptive_factor - 1);
                break;
            }
        }

        if (wr.m_terminate.test(std::memory_order_relaxed)) [[unlikely]] {
            return nullptr;
        }

        vtm = wr.m_rng();
    }

    // Phase 3: 阻塞等待
    m_notifier.prepare_wait(wr.m_id);

    // Double-check: 唤醒后再次检查队列
    for (std::size_t i = 0; i < nb; ++i) {
        if (!m_buffers[i].queue.empty()) {
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

    if (wr.m_terminate.test(std::memory_order_relaxed)) [[unlikely]] {
        m_notifier.cancel_wait(wr.m_id);
        return nullptr;
    }

    m_notifier.commit_wait(wr.m_id);
    goto explore;
}

TFL_FORCE_INLINE std::size_t Executor::_set_up_graph(Graph& g, Topology* topo, Work* parent) {
    Work** const data = g.m_works.data();
    std::size_t const size = g.m_works.size();
    std::size_t n = 0;

    for (std::size_t i = 0; i < size; ++i) {
        Work* w = data[i];
        w->m_parent = parent;
        w->m_topology = topo;
        if (w->m_state.load(std::memory_order_relaxed) & Work::State::EXCEPTION) [[unlikely]] {
            w->m_exception_ptr = nullptr;
        }
        w->m_state.store(Work::State::NONE, std::memory_order_relaxed);
        w->m_join_counter.store(w->m_options & Work::Option::COUNT_MASK, std::memory_order_relaxed);

        if ((w->m_edges.size() - w->m_num_successors) == 0) {
            std::swap(data[i], data[n++]);
        }
    }
    return n;
}

/// @brief 任务完成后的依赖传播
///
/// @par 核心逻辑
/// 1. 恢复 join_counter（支持循环图）
/// 2. 遍历后继，检查依赖是否满足
/// 3. 链式调度：满足条件的后继直接放入 cache
///
/// @memory_order
/// - fetch_sub(acq_rel): 确保当前任务结果对后继可见
/// - fetch_add(relaxed): 仅计数，不需跨线程同步
TFL_FORCE_INLINE void Executor::_tear_down_task(Work* w, Worker& wr, Work*& cache) {
    w->m_join_counter.fetch_add(w->_join_count(), std::memory_order_relaxed);
    auto* parent = w->m_parent;
    if (!w->_is_exception()) [[likely]] {
        const std::size_t sz = w->m_num_successors;
        for (std::size_t i = 0; i < sz; ++i) {
            auto* suc = w->m_edges[i];
            // acq_rel: 确保任务执行结果对后继可见
            if ((suc->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1)) {
                parent->m_join_counter.fetch_add(1, std::memory_order_relaxed);
                if (cache) {
                    _schedule(wr, cache);
                }
                // 零开销接力：满足条件的后继直接放入 cache
                cache = suc;
            }
        }
    }
    _schedule_parent(parent, wr, cache);
}

/// @brief Jump 任务的强制跳转处理
///
/// @par 设计意图
/// Jump 任务是"特权通道"，可以无视正常依赖关系直接跳转到目标节点。
/// 这里通过直接将目标节点的 join_counter 置零来实现"强制执行"。
TFL_FORCE_INLINE void Executor::_tear_down_jump_task(Work* w, Worker& wr, Work*& cache, Work* target) {
    w->m_join_counter.fetch_add(w->_join_count(), std::memory_order_relaxed);
    auto* parent = w->m_parent;
    if (!w->_is_exception() && target) [[likely]] {
        target->m_join_counter.store(0, std::memory_order_relaxed);
        parent->m_join_counter.fetch_add(1, std::memory_order_relaxed);
        if (cache) {
            _schedule(wr, cache);
        }
        cache = target;
    }
    _schedule_parent(parent, wr, cache);
}

TFL_FORCE_INLINE void Executor::_tear_down_multi_jump_task(Work* w, Worker& wr, Work*& cache, const SmallVector<Work*>& targets) {
    w->m_join_counter.fetch_add(w->_join_count(), std::memory_order_relaxed);
    auto* parent = w->m_parent;
    if (!w->_is_exception()) [[likely]] {
        for (auto* target : targets) {
            target->m_join_counter.store(0, std::memory_order_relaxed);
            parent->m_join_counter.fetch_add(1, std::memory_order_relaxed);
            if (cache) {
                _schedule(wr, cache);
            }
            cache = target;
        }
    }
    _schedule_parent(parent, wr, cache);
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
inline void Executor::_set_up_dep_async_task(Work* w, I first, S last, std::size_t& num_predecessors) {
    _increment_topology();

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
            if (&suc_exec == this) {
                if (cache) {
                    _schedule(wr, cache);
                }
                cache = suc;
            } else {
                // 跨调度器调度
                suc_exec._schedule(suc);
            }
        }
    }
    _decrement_topology();

    if (topo->_decref()) {
        Work::destroy(w, w->m_topology);
    }
}

// ============================================================================
//  异常处理
// ============================================================================

/// @brief 异常传播与捕获
///
/// @par 算法
/// 1. 沿父链向上遍历，打上异常标记
/// 2. 找到 ANCHORED 节点作为"异常收集站"
/// 3. 首个到达的异常被存储（其他被覆盖）
inline void Executor::_process_exception(Work* w) {
    auto eptr = std::current_exception();

    Work* work = w;
    while (work && !work->_is_anchored()) {
        work->_set_exception();
        work = work->m_parent;
    }

    if (work && work->_try_catch_exception()) {
        work->m_exception_ptr = eptr;
    }
    w->m_exception_ptr = eptr;
}

inline void Executor::_push_global(Work* val) {
    std::size_t const size = m_buffers.size();
    std::size_t const b = detail::mulhi64(
        reinterpret_cast<std::uintptr_t>(val) * 11400714819323198485ULL,
        size
        );

    // Fast-Path
    for (std::size_t curr_b = b; curr_b < size; ++curr_b) {
        auto& buf = m_buffers[curr_b];
        if (buf.mutex.try_lock()) {
            buf.queue.push(val);
            buf.mutex.unlock();
            return;
        }
    }
    for (std::size_t curr_b = 0; curr_b < b; ++curr_b) {
        auto& buf = m_buffers[curr_b];
        if (buf.mutex.try_lock()) {
            buf.queue.push(val);
            buf.mutex.unlock();
            return;
        }
    }

    // 直接对目标队列加锁并推送，如果被占用则当前线程阻塞等待
    std::lock_guard<std::mutex> lock(m_buffers[b].mutex);
    m_buffers[b].queue.push(val);
}

template <std::random_access_iterator Iterator>
    requires std::convertible_to<std::iter_reference_t<Iterator>, Work*>
inline void Executor::_push_global(Iterator first, std::size_t n) {
    std::size_t const size = m_buffers.size();
    std::size_t const b = detail::mulhi64(
        reinterpret_cast<std::uintptr_t>(*first) * 11400714819323198485ULL,
        size
        );

    // Fast-Path
    for (std::size_t curr_b = b; curr_b < size; ++curr_b) {
        auto& buf = m_buffers[curr_b];
        if (buf.mutex.try_lock()) {
            buf.queue.push(first, n);
            buf.mutex.unlock();
            return;
        }
    }
    for (std::size_t curr_b = 0; curr_b < b; ++curr_b) {
        auto& buf = m_buffers[curr_b];
        if (buf.mutex.try_lock()) {
            buf.queue.push(first, n);
            buf.mutex.unlock();
            return;
        }
    }

    // 直接对目标队列加锁并批量推送
    std::lock_guard<std::mutex> lock(m_buffers[b].mutex);
    m_buffers[b].queue.push(first, n);
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
        _push_global(remaining, count);
    });

    m_notifier.notify_n(n);
}

template <std::random_access_iterator Iterator>
inline void Executor::_schedule(Iterator first, std::size_t n) {
    if (n == 0) [[unlikely]] {
        return;
    }
    _push_global(first, n);
    m_notifier.notify_n(n);
}

inline void Executor::_schedule(Worker& wr, Work* w) {
    wr.m_wslq.push(w, [&]() {
        _push_global(w);
    });
    m_notifier.notify_one();
}

inline void Executor::_schedule(Work* w) {
    _push_global(w);
    m_notifier.notify_one();
}

/// @brief 父任务完成处理（PREEMPTED 机制）
///
/// @par 设计
/// 当子任务全部完成时：
/// - 普通父任务：等待后续调度
/// - PREEMPTED 父任务：直接插入 cache 继续执行（抢占执行权）
TFL_FORCE_INLINE void Executor::_schedule_parent(Work* parent, Worker& wr, Work*& cache) {
    auto ops = parent->m_options;
    if (parent->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (ops & Work::Option::PREEMPTED) {
            if (cache) {
                _schedule(wr, cache);
            }
            cache = parent;
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
inline void Executor::_corun_until(Worker& wr, Pred&& pred) {
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
            : m_buffers[vtm - nw].queue.steal();;

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


inline void Executor::_corun_graph(Worker& wr, Graph& g, Work* parent) {
    auto num_srcs = _set_up_graph(g, parent->m_topology, parent);
    if(num_srcs == 0) {
        return;
    }
    parent->m_join_counter.store(num_srcs, std::memory_order_relaxed);
    _schedule(wr, g.begin(), num_srcs);

    _corun_until(wr, [parent]() noexcept { return parent->m_join_counter.load(std::memory_order_acquire) == 0; });
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
        w->m_invoke(w, *this, wr, cache);
        w = cache; // 链式执行
    } while (w);
}


// ============================================================================
//  实现部分：AsyncTask 与各类 Work 子类的 Invoke 具体派发机制
// ============================================================================

template <std::input_iterator I, std::sentinel_for<I> S>
    requires std::same_as<std::iter_value_t<I>, AsyncTask>
inline AsyncTask& AsyncTask::start(I first, S last) {
    auto* topo = m_work->m_topology;
    auto& exec = topo->m_executor;

    if (topo->_is_finished()) {
        throw Exception("AsyncTask Error: Cannot start a finished task.");
    }

    auto expected = Topology::State::Idle;
    if (!topo->m_state.compare_exchange_strong(expected, Topology::State::Running,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
        throw Exception("AsyncTask Error: Task is already running.");
    }

    topo->_incref();

    std::size_t num_predecessors = static_cast<std::size_t>(std::ranges::distance(first, last));
    m_work->_set_up(num_predecessors);

    exec._set_up_dep_async_task(m_work, first, last, num_predecessors);

    if (num_predecessors == 0) {
        if (Worker* w = exec._this_worker()) {
            exec._schedule(*w, m_work);
        } else {
            exec._schedule(m_work);
        }
    }

    return *this;
}

#define TFL_SEM_SCHEDULER [&exe, &wr](Work* t) {               \
auto& target_exec = t->m_topology->m_executor;              \
    if (&target_exec == &exe) {   \
        exe._schedule(wr, t);                                   \
} else {                                                    \
        target_exec._schedule(t);                               \
}                                                           \
}


// ============================================================================
//  invoke 实现 — 同步任务
// ============================================================================

template <typename F, typename... Args>
inline void BasicWork<F, Args...>::invoke(Work* self, Executor& exe, Worker& wr, Work*& cache) {
    auto* w = static_cast<BasicWork*>(self);
    if (w->_should_abort()) [[unlikely]] {
        exe._schedule_parent(w->m_parent, wr, cache);
        return;
    }

    if (!w->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
        return;
    }

    w->_notify_before(wr);

    if constexpr (sizeof...(Args) == 0) {
        try {
            std::invoke(w->m_func);
        } catch (...) {
            exe._process_exception(w);
        }
    } else {
        try {
            std::apply([&w](auto&&... a) noexcept(noexcept(std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))...))) {
                std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))...);
            }, w->m_args);
        } catch (...) {
            exe._process_exception(w);
        }
    }

    w->_notify_after(wr);
    w->_release_semaphores(TFL_SEM_SCHEDULER);
    exe._tear_down_task(w, wr, cache);
}

template <typename F, typename... Args>
inline void BranchWork<F, Args...>::invoke(Work* self,Executor& exe, Worker& wr, Work*& cache) {
    auto* w = static_cast<BranchWork*>(self);
    if (w->_should_abort()) [[unlikely]] {
        exe._schedule_parent(w->m_parent, wr, cache);
        return;
    }

    if (!w->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
        return;
    }

    w->_notify_before(wr);

    Branch branch(*w);
    if constexpr (sizeof...(Args) == 0) {
        try {
            std::invoke(w->m_func, branch);
        } catch (...) {
            exe._process_exception(w);
        }
    } else {
        try {
            std::apply([&w, &branch](auto&&... a) noexcept(noexcept(std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., branch))) {
                std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., branch);
            }, w->m_args);
        } catch (...) {
            exe._process_exception(w);
        }
    }

    if (auto target = branch.m_target) {
        target->m_join_counter.fetch_sub(1, std::memory_order_relaxed);
    }

    w->_notify_after(wr);
    w->_release_semaphores(TFL_SEM_SCHEDULER);
    exe._tear_down_task(w, wr, cache);
}

template <typename F, typename... Args>
inline void MultiBranchWork<F, Args...>::invoke(Work* self,Executor& exe, Worker& wr, Work*& cache) {
    auto* w = static_cast<MultiBranchWork*>(self);
    if (w->_should_abort()) [[unlikely]] {
        exe._schedule_parent(w->m_parent, wr, cache);
        return;
    }

    if (!w->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
        return;
    }

    w->_notify_before(wr);

    MultiBranch branch(*w);
    if constexpr (sizeof...(Args) == 0) {
        try {
            std::invoke(w->m_func, branch);
        } catch (...) {
            exe._process_exception(w);
        }
    } else {
        try {
            std::apply([&w, &branch](auto&&... a) noexcept(noexcept(std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., branch))) {
                std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., branch);
            }, w->m_args);
        } catch (...) {
            exe._process_exception(w);
        }
    }

    for (auto* target : branch.m_targets) {
        target->m_join_counter.fetch_sub(1, std::memory_order_relaxed);
    }

    w->_notify_after(wr);
    w->_release_semaphores(TFL_SEM_SCHEDULER);
    exe._tear_down_task(w, wr, cache);
}

template <typename F, typename... Args>
inline void JumpWork<F, Args...>::invoke(Work* self,Executor& exe, Worker& wr, Work*& cache) {
    auto* w = static_cast<JumpWork*>(self);
    if (w->_should_abort()) [[unlikely]] {
        exe._schedule_parent(w->m_parent, wr, cache);
        return;
    }

    if (!w->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
        return;
    }

    w->_notify_before(wr);

    Jump jmp{*w};
    if constexpr (sizeof...(Args) == 0) {
        try {
            std::invoke(w->m_func, jmp);
        } catch (...) {
            exe._process_exception(w);
        }
    } else {
        try {
            std::apply([&w, &jmp](auto&&... a) noexcept(noexcept(std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., jmp))) {
                std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., jmp);
            }, w->m_args);
        } catch (...) {
            exe._process_exception(w);
        }
    }

    w->_notify_after(wr);
    w->_release_semaphores(TFL_SEM_SCHEDULER);
    exe._tear_down_jump_task(w, wr, cache, jmp.m_target);
}

template <typename F, typename... Args>
inline void MultiJumpWork<F, Args...>::invoke(Work* self,Executor& exe, Worker& wr, Work*& cache) {
    auto* w = static_cast<MultiJumpWork*>(self);
    if (w->_should_abort()) [[unlikely]] {
        exe._schedule_parent(w->m_parent, wr, cache);
        return;
    }

    if (!w->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
        return;
    }

    w->_notify_before(wr);

    MultiJump jmp{*w};
    if constexpr (sizeof...(Args) == 0) {
        try {
            std::invoke(w->m_func, jmp);
        } catch (...) {
            exe._process_exception(w);
        }
    } else {
        try {
            std::apply([&w, &jmp](auto&&... a) noexcept(noexcept(std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., jmp))) {
                std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., jmp);
            }, w->m_args);
        } catch (...) {
            exe._process_exception(w);
        }
    }

    w->_notify_after(wr);
    w->_release_semaphores(TFL_SEM_SCHEDULER);
    exe._tear_down_multi_jump_task(w, wr, cache, jmp.m_targets);
}

template <typename F, typename... Args>
inline void RuntimeWork<F, Args...>::invoke(Work* self,Executor& exe, Worker& wr, Work*& cache) {
    auto* w = static_cast<RuntimeWork*>(self);
    if (w->_should_abort()) [[unlikely]] {
        exe._schedule_parent(w->m_parent, wr, cache);
        return;
    }

    if (!w->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
        return;
    }

    w->_notify_before(wr);

    Runtime rt(*w, wr, *w->m_topology, exe);
    if constexpr (sizeof...(Args) == 0) {
        try {
            std::invoke(w->m_func, rt);
        } catch (...) {
            exe._process_exception(w);
        }
    } else {
        try {
            std::apply([&w, &rt](auto&&... a) noexcept(noexcept(std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt))) {
                std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt);
            }, w->m_args);
        } catch (...) {
            exe._process_exception(w);
        }
    }

    w->_notify_after(wr);
    w->_release_semaphores(TFL_SEM_SCHEDULER);
    exe._tear_down_task(w, wr, cache);
}

// ============================================================================
//  invoke 实现 — Subflow
// ============================================================================

template <typename FlowStore, typename P>
inline void SubflowWork<FlowStore, P>::invoke(Work* self,Executor& exe, Worker& wr, Work*& cache) {
    auto* w = static_cast<SubflowWork*>(self);
    auto& graph = detail::unwrap(w->m_flow_store).m_graph;

    // ── 状态分流：m_num_sources > 0 表示子图刚跑完的重入，否则为首次进入 ──
    if (w->m_num_sources > 0) {
        // 重入：子图已执行完毕，通知观察者"本节点一轮工作结束"
        w->_notify_after(wr);
    } else {
        // 首次进入：竞争获取信号量，失败则让出执行权等待唤醒
        if (!w->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
            return;
        }
        // 初始化子图拓扑：重置各节点 join_counter、清除异常残留、
        // 将零入度源节点 swap 到 graph 前端，返回源节点数量
        w->m_num_sources = exe._set_up_graph(graph, w->m_topology, w);
    }

    // ── 终止判定：三条件任一成立即结束循环 ──
    //   pred == true    → 用户谓词决定停止迭代
    //   num_sources == 0 → 子图为空或所有节点均有前驱（无法启动）
    //   _should_abort()   → 拓扑层面已被异常终止
    if (bool pred = std::invoke_r<bool>(w->m_pred); pred || w->m_num_sources == 0 || w->_should_abort()) {
        w->m_num_sources = 0;                              // 归零，为下次图复用做好准备
        w->_release_semaphores(TFL_SEM_SCHEDULER);         // 归还信号量配额，唤醒等待者
        exe._tear_down_task(w, wr, cache);                 // 走静态图依赖传播，通知后继节点
    } else {
        // ── 启动子图：设置屏障并批量调度源节点 ──
        w->_notify_before(wr);                             // 通知观察者"本节点即将开始新一轮"
        w->m_join_counter.store(w->m_num_sources, std::memory_order_relaxed); // 倒计时屏障
        exe._schedule(wr, graph.begin(), w->m_num_sources);// 批量投递源节点进调度队列
    }
}

// ============================================================================
//  invoke 实现 — 独立异步任务
// ============================================================================

template <typename F, typename... Args>
inline void AsyncBasicWork<F, Args...>::invoke(Work* self,Executor& exe, [[maybe_unused]] Worker& wr, [[maybe_unused]] Work*& cache) {
    auto* w = static_cast<AsyncBasicWork*>(self);
    if constexpr (sizeof...(Args) == 0) {
        try {
            std::invoke(w->m_func);
        } catch (...) {}
    } else {
        try {
            std::apply([&w](auto&&... a) noexcept(noexcept(std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))...))) {
                std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))...);
            }, w->m_args);
        } catch (...) {}
    }

    exe._decrement_topology();
    Work::destroy(w, w->m_topology);
}

template <typename F, typename... Args>
inline void AsyncRuntimeWork<F, Args...>::invoke(Work* self,Executor& exe, Worker& wr, [[maybe_unused]] Work*& cache) {
    auto* w = static_cast<AsyncRuntimeWork*>(self);
    Runtime rt(*w, wr, *w->m_topology, exe);
    if constexpr (sizeof...(Args) == 0) {
        try {
            std::invoke(w->m_func, rt);
        } catch (...) {}
    } else {
        try {
            std::apply([&w, &rt](auto&&... a) noexcept(noexcept(std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt))) {
                std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt);
            }, w->m_args);
        } catch (...) {}
    }

    exe._decrement_topology();
    Work::destroy(w, w->m_topology);
}

// ============================================================================
//  invoke 实现 — Promise 异步任务
// ============================================================================

template <typename F, typename R, typename... Args>
inline void AsyncBasicPromiseWork<F, R, Args...>::invoke(Work* self,Executor& exe, [[maybe_unused]] Worker& wr, [[maybe_unused]] Work*& cache) {
    auto* w = static_cast<AsyncBasicPromiseWork*>(self);
    try {
        if constexpr (std::is_void_v<R>) {
            if constexpr (sizeof...(Args) == 0) {
                std::invoke(w->m_func);
            } else {
                std::apply([&w](auto&&... a) noexcept(noexcept(std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))...))) {
                    std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))...);
                }, w->m_args);
            }
            w->m_promise.set_value();
        } else {
            if constexpr (sizeof...(Args) == 0) {
                w->m_promise.set_value(std::invoke(w->m_func));
            } else {
                w->m_promise.set_value(std::apply([&w](auto&&... a) noexcept(noexcept(std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))...))) {
                    return std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))...);
                }, w->m_args));
            }
        }

        exe._decrement_topology();
        Work::destroy(w, w->m_topology);
    } catch (...) {
        w->m_promise.set_exception(std::current_exception());
        exe._decrement_topology();
        Work::destroy(w, w->m_topology);
    }
}

template <typename F, typename R, typename... Args>
inline void AsyncRuntimePromiseWork<F, R, Args...>::invoke(Work* self,Executor& exe, Worker& wr, [[maybe_unused]] Work*& cache) {
    auto* w = static_cast<AsyncRuntimePromiseWork*>(self);
    Runtime rt(*w, wr, *w->m_topology, exe);
    try {
        if constexpr (std::is_void_v<R>) {
            if constexpr (sizeof...(Args) == 0) {
                std::invoke(w->m_func, rt);
            } else {
                std::apply([&w, &rt](auto&&... a) noexcept(noexcept(std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt))) {
                    std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt);
                }, w->m_args);
            }
            w->m_promise.set_value();
        } else {
            if constexpr (sizeof...(Args) == 0) {
                w->m_promise.set_value(std::invoke(w->m_func, rt));
            } else {
                w->m_promise.set_value(std::apply([&w, &rt](auto&&... a) noexcept(noexcept(std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt))) {
                    return std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt);
                }, w->m_args));
            }
        }

        exe._decrement_topology();
        Work::destroy(w, w->m_topology);
    } catch (...) {
        w->m_promise.set_exception(std::current_exception());
        exe._decrement_topology();
        Work::destroy(w, w->m_topology);
    }
}

// ============================================================================
//  invoke 实现 — 有依赖的异步任务
// ============================================================================

template <typename F, typename... Args>
inline void DepAsyncBasicWork<F, Args...>::invoke(Work* self,Executor& exe, Worker& wr, Work*& cache) {
    auto* w = static_cast<DepAsyncBasicWork*>(self);
    if (!w->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
        return;
    }

    w->_notify_before(wr);

    if constexpr (sizeof...(Args) == 0) {
        try {
            std::invoke(w->m_func);
        } catch (...) {
            exe._process_exception(w);
        }
    } else {
        try {
            std::apply([&w](auto&&... a) noexcept(noexcept(std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))...))) {
                std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))...);
            }, w->m_args);
        } catch (...) {
            exe._process_exception(w);
        }
    }

    w->_notify_after(wr);
    w->_release_semaphores(TFL_SEM_SCHEDULER);
    exe._tear_down_dep_async_task(w, wr, cache);
}

template <typename F, typename... Args>
inline void DepAsyncRuntimeWork<F, Args...>::invoke(Work* self,Executor& exe, Worker& wr, Work*& cache) {
    auto* w = static_cast<DepAsyncRuntimeWork*>(self);
    if (!w->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
        return;
    }

    w->_notify_before(wr);

    Runtime rt(*w, wr, *w->m_topology, exe);
    if constexpr (sizeof...(Args) == 0) {
        try {
            std::invoke(w->m_func, rt);
        } catch (...) {
            exe._process_exception(w);
        }
    } else {
        try {
            std::apply([&w, &rt](auto&&... a) noexcept(noexcept(std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt))) {
                std::invoke(w->m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt);
            }, w->m_args);
        } catch (...) {
            exe._process_exception(w);
        }
    }

    w->_notify_after(wr);
    w->_release_semaphores(TFL_SEM_SCHEDULER);
    exe._tear_down_dep_async_task(w, wr, cache);
}

// ============================================================================
//  invoke 实现 — 有依赖的子流程
// ============================================================================

/// @brief DepFlowWork 的核心调度入口 — 条件循环驱动子图执行。
///
/// @details
/// **重入式状态机设计**：利用 m_num_sources 兼任"是否已启动"的判据，
/// 首次进入时 m_num_sources == 0，触发信号量获取与
/// 子图拓扑初始化；子图跑完后 PREEMPTED 机制将控制权交还本节点，
/// 此时 m_num_sources > 0 标识重入，仅执行观察者回调。
template <typename FlowStore, typename P, typename C>
inline void DepFlowWork<FlowStore, P, C>::invoke(Work* self,Executor& exe, Worker& wr, Work*& cache) {
    auto* w = static_cast<DepFlowWork*>(self);
    auto& graph = detail::unwrap(w->m_flow_store).m_graph;

    // ── 状态分流：m_num_sources > 0 表示子图刚跑完的重入，否则为首次进入 ──
    if (w->m_num_sources > 0) {
        // 重入：子图已执行完毕，通知观察者"本节点一轮工作结束"
        w->_notify_after(wr);
    } else {
        // 首次进入：竞争获取信号量，失败则让出执行权等待唤醒
        if (!w->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
            return;
        }
        // 初始化子图拓扑：重置各节点 join_counter、清除异常残留、
        // 将零入度源节点 swap 到 graph 前端，返回源节点数量
        w->m_num_sources = exe._set_up_graph(graph, w->m_topology, w);
    }

    // ── 终止判定：三条件任一成立即结束循环 ──
    //   pred == true  → 用户谓词决定停止迭代
    //   num_sources == 0 → 子图为空或所有节点均有前驱（无法启动）
    //   _should_abort() → 拓扑层面已被异常终止
    if (bool pred = std::invoke_r<bool>(w->m_pred); pred || w->m_num_sources == 0 || w->_should_abort()) {
        w->m_num_sources = 0;                              // 归零，为下次 start() 重入做好准备
        w->_release_semaphores(TFL_SEM_SCHEDULER);         // 归还信号量配额，唤醒等待者
        std::invoke(w->m_callback);                        // 用户终止回调（生命周期落幕通知）
        exe._tear_down_dep_async_task(w, wr, cache);       // 拆除异步依赖链，传播完成信号给下游
    } else {
        // ── 启动子图：设置屏障并批量调度源节点 ──
        w->_notify_before(wr);                             // 通知观察者"本节点即将开始新一轮"
        w->m_join_counter.store(w->m_num_sources, std::memory_order_relaxed); // 倒计时屏障
        exe._schedule(wr, graph.begin(), w->m_num_sources);// 批量投递源节点进调度队列
    }
}
#undef TFL_SEM_SCHEDULER

// ============================================================================
//  实现部分：Runtime 路由适配器
// ============================================================================

template <typename F>
    requires flow_type<F>
inline AsyncTask Runtime::submit(F&& flow) {
    return submit(std::forward<F>(flow), 1ULL);
}

template <typename F, typename C>
    requires (capturable<C> && flow_type<F> && callback<C>)
inline AsyncTask Runtime::submit(F&& flow, C&& callback) {
    return submit(std::forward<F>(flow), 1ULL, std::forward<C>(callback));
}

template <typename F>
    requires flow_type<F>
inline AsyncTask Runtime::submit(F&& flow, std::uint64_t num) {
    return submit(std::forward<F>(flow), num, []() noexcept {});
}

template <typename F, typename C>
    requires (capturable<C> && flow_type<F> && callback<C>)
inline AsyncTask Runtime::submit(F&& flow, std::uint64_t num, C&& callback) {
    return submit(std::forward<F>(flow)
                  ,[num]() mutable noexcept { return num-- == 0; }
                  ,std::forward<C>(callback));
}

template <typename F, typename P>
    requires (capturable<P> && flow_type<F> && predicate<P>)
inline AsyncTask Runtime::submit(F&& flow, P&& pred) {
    return submit(std::forward<F>(flow)
                  ,std::forward<P>(pred)
                  ,[]() noexcept {});
}

template <typename F, typename P, typename C>
    requires (capturable<P, C> && flow_type<F> && predicate<P> && callback<C>)
inline AsyncTask Runtime::submit(F&& flow, P&& pred, C&& callback) {
    constexpr auto options = Work::Option::ANCHORED | Work::Option::PREEMPTED;
    Work* work = Work::make_dep_flow(m_executor, options,
                                     std::forward<F>(flow), std::forward<P>(pred), std::forward<C>(callback));
    return AsyncTask{work};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
inline AsyncTask Runtime::submit(T&& task, Args&&... args) {
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_dep_async_basic(m_executor, options,
                                            std::forward<T>(task), std::forward<Args>(args)...);
    return AsyncTask{work};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
inline AsyncTask Runtime::submit(T&& task, Args&&... args) {
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_dep_async_runtime(m_executor, options,
                                              std::forward<T>(task), std::forward<Args>(args)...);
    return AsyncTask{work};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
inline void Runtime::silent_async(T&& task, Args&&... args) {
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_async_basic(m_executor, options,
                                        std::forward<T>(task), std::forward<Args>(args)...);
    m_executor._increment_topology();
    m_executor._schedule(m_worker, work);
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
inline void Runtime::silent_async(T&& task, Args&&... args) {
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_async_runtime(m_executor, options,
                                          std::forward<T>(task), std::forward<Args>(args)...);
    m_executor._increment_topology();
    m_executor._schedule(m_worker, work);
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
inline auto Runtime::async(T&& task, Args&&... args) -> std::future<basic_return_t<T, Args...>> {
    using R = basic_return_t<T, Args...>;
    std::promise<R> promise;
    auto future = promise.get_future();
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_async_basic(m_executor, options,
                                        std::forward<T>(task), std::move(promise), std::forward<Args>(args)...);
    m_executor._increment_topology();
    m_executor._schedule(m_worker, work);
    return future;
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
inline auto Runtime::async(T&& task, Args&&... args) -> std::future<runtime_return_t<T, Args...>> {
    using R = runtime_return_t<T, Args...>;
    std::promise<R> promise;
    auto future = promise.get_future();
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_async_runtime(m_executor, options,
                                          std::forward<T>(task), std::move(promise), std::forward<Args>(args)...);
    m_executor._increment_topology();
    m_executor._schedule(m_worker, work);
    return future;
}

inline void Runtime::run(Task task) {
    auto* w = task.m_work;

    // Why: 约束校验，强制要求利用此途径插入的动态任务必须存在于统一的隔离生命周期内。
    if (w->m_parent != m_work.m_parent) {
        throw Exception("Task does not share the same parent as this Runtime.");
    }

    w->m_join_counter.store(0, std::memory_order_relaxed);
    w->m_parent->m_join_counter.fetch_add(1, std::memory_order_acq_rel);

    Work* cache{nullptr};
    w->m_invoke(w, m_executor, m_worker, cache);
    if (cache) {
        m_executor._schedule(m_worker, cache);
    }
}

inline void Runtime::run(Flow& flow) {
    constexpr auto options = Work::Option::ANCHORED;
    NullWork parent{m_work.m_topology, options};
    m_executor._corun_graph(m_worker, flow.m_graph, &parent);

    parent._rethrow_exception();
}

template <predicate Pred>
inline void Runtime::wait_until(Pred&& pred) {
    m_executor._corun_until(m_worker, std::forward<Pred>(pred));
}


} // namespace tfl
