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

/// @brief 任务调度器——Work-Stealing 并行执行引擎,框架唯一的 OS 线程持有者。
///
/// @details
/// `Executor` 拥有 worker 线程池、共享提交队列和 Notifier。`Flow`、`Task`、
/// `Runtime`、`AsyncTask` 等所有用户层对象最终都通过 `Executor` 落地执行。
/// 不可拷贝/不可移动——析构函数会阻塞等待所有在飞任务完成,实例应长寿命。
///
/// ============================================================================
///  调度拓扑
/// ============================================================================
/// @code
///   Flow / AsyncTask / Runtime ──submit──► Executor ──dispatch──► Worker×N
///                                                            │   BoundedQueue (LIFO)
///                                                            └── 溢出 ──► SharedBuffer (UnboundedQueue)
/// @endcode
///
/// ============================================================================
///  工作窃取三阶段
/// ============================================================================
/// 1. 本地队列优先：owner 从自己的 `BoundedQueue` 尾部 LIFO pop,缓存友好；
/// 2. 随机窃取：空闲 worker 用 `SplitMix64` 选 victim,从头部 FIFO steal；
/// 3. 阻塞等待：多轮失败后通过 `Notifier` 两阶段 park,防止 lost wake-up。
///
/// `m_shared_buffers` 是对数级分片的共享队列集合。`_push_shared` 用任务地址
/// 哈希选 buffer,try_lock 失败后线性探测,最后才阻塞锁。
///
/// ============================================================================
///  提交 API
/// ============================================================================
/// | API               | 返回值                | 启动方式        | 语义           |
/// |-------------------|-----------------------|-----------------|----------------|
/// | `detach`    | `void`                | 立即            | 即发即弃       |
/// | `async`           | `Future<R>`           | 立即            | 异步返回值     |
/// | `lazy_async`  | `AsyncTask        `   | 手动 `start()`  | 启动前可配置   |
///
/// 每组 API 按 `graph_holder`、`basic_invocable`、`runtime_invocable` 等 concept
/// 分发,最终收敛到 Work 工厂、依赖初始化和 `_schedule` 调度入口。
///
/// ============================================================================
///  上下文感知调度
/// ============================================================================
/// - 当前线程是 worker → 推入该 worker 本地队列(最快路径)
/// - 当前线程非 worker → 推入 shared buffer
/// - 本地队列满 → 溢出到 shared buffer
///
/// ============================================================================
///  Topology 生命周期
/// ============================================================================
/// `m_num_topologies` 仅统计顶层提交实例——嵌套任务由父节点 `m_join_counter` 管理。
/// `wait_for_all()` 等待的是用户提交的根任务,而非内部派生的每个子任务。
///
/// ============================================================================
///  内存序约定
/// ============================================================================
/// | 变量                  | 内存序           | 用途                         |
/// |-----------------------|------------------|------------------------------|
/// | `m_num_topologies`    | acquire / notify | `wait_for_all()` 同步        |
/// | `m_join_counter`      | acq_rel          | 后继就绪判断,前驱结果可见性   |
/// | `m_terminate`         | relaxed          | 终止轮询                     |
/// | `Topology::m_state`   | CAS (acq_rel)    | 动态依赖边插入的 Locking 协议 |
///
/// @par 线程安全
/// 构造完成后,所有 public API 可从任意线程并发调用。内部由原子变量、work-stealing
/// 队列、`Notifier` 和分片 mutex 保护。正在执行的 `Flow` 不能并发 emplace/erase/clear。
///
/// @see Worker      工作线程实体
/// @see Notifier    两阶段 park 唤醒机制
/// @see Topology    执行实例与引用计数
/// @see Flow        用户侧 DAG 构建器
class Executor : public Immovable<Executor> {
    friend class Runtime;
    template <typename> friend class AsyncTask;
    TFL_WORK_SUBCLASS_FRIENDS;

public:
    /// @brief 创建调度器并启动工作线程（使用默认 handler）
    /// @param num_workers 工作线程数量(默认 CPU 核心数)
    explicit Executor(std::size_t num_workers = std::thread::hardware_concurrency());

    /// @brief 创建调度器并启动工作线程
    /// @tparam H 见 worker_handle concept,支持以下形态:
    ///         - 值/右值: `MyHandler{...}` / `std::move(h)`  → 拷贝/移动到堆,Executor 拥有
    ///         - std::ref(h) / std::cref(h)                 → 借用,调用方保证 h 生命周期
    /// @param handler 用户自定义的 WorkerHandler 派生对象
    /// @param num_workers 工作线程数量(默认 CPU 核心数)
    template <worker_handle H>
    explicit Executor(H&& handler, std::size_t num_workers = std::thread::hardware_concurrency());

    /// @brief 等待所有已提交任务完成、停止工作线程并释放资源
    ~Executor() noexcept;

    /// ====================================================================
    // 任务派发 API —— 接受游离态的 AsyncTask,在本执行器内启动
    // ====================================================================
    /// @brief 派发任务到本执行器(节点必须 Idle)。
    /// @tparam Deps 必须为 AsyncTask<nonrepeat_t> 或其引用。
    /// @param task 待派发的任务句柄,内部 Work 节点必须处于 Idle 状态。
    /// @param deps 上游依赖列表,task 将在所有 deps 完成后调度。
    /// @return task 的引用,支持链式。
    /// @throw Exception 若 task 为空、已 submitted、或处于非 Idle 状态。
    /// @note 派发后 topology->m_executor 被填入 *this,任务由本执行器独占调度。
    template <typename T, typename... Deps>
        requires (any_async_task<T> && (nonrepeat_async_task<Deps> && ...))
    auto submit(T&& task, Deps&&... deps) -> std::conditional_t<std::is_lvalue_reference_v<T>, std::remove_reference_t<T>&, std::remove_cvref_t<T>>;

    template <typename T, std::input_iterator I, std::sentinel_for<I> S>
        requires (any_async_task<T> && nonrepeat_async_task<std::iter_value_t<I>>)
    auto submit(T&& task, I first, S last) -> std::conditional_t<std::is_lvalue_reference_v<T>, std::remove_reference_t<T>&, std::remove_cvref_t<T>>;
    // ========================================================================
    //  任务提交 API
    // ========================================================================

    /// @brief 提交任务图执行一次
    template <typename Gh>
        requires graph_holder<Gh>
    void detach(Gh&& gh);

    /// @brief 提交任务图执行一次，完成后执行回调
    template <typename Gh, typename C>
        requires (capturable<C> && graph_holder<Gh> && callback<C>)
    void detach(Gh&& gh, C&& cb);

    /// @brief 提交任务图执行指定次数
    template <typename Gh>
        requires graph_holder<Gh>
    void detach(Gh&& gh, std::uint64_t num);

    /// @brief 提交任务图循环执行指定次数，完成后执行回调
    template <typename Gh, typename C>
        requires (capturable<C> && graph_holder<Gh> && callback<C>)
    void detach(Gh&& gh, std::uint64_t num, C&& cb);

    /// @brief 提交任务图条件循环执行
    template <typename Gh, typename P>
        requires (capturable<P> && graph_holder<Gh> && predicate<P>)
    void detach(Gh&& gh, P&& pred);

    /// @brief 提交任务图条件循环执行，完成后执行回调
    template <typename Gh, typename P, typename C>
        requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
    void detach(Gh&& gh, P&& pred, C&& cb);

    /// @brief 即发即弃异步执行
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable_plain<T, Args...>)
    void detach(T&& task, Args&&... args);

    /// @brief 即发即弃运行时任务
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable_plain<T, Args...>)
    void detach(T&& task, Args&&... args);


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
    /// @brief 返回当前活跃的 worker 线程数。
    [[nodiscard]] std::size_t num_workers() const noexcept;
    /// @brief 返回当前正在等待的 worker 线程数。
    [[nodiscard]] std::size_t num_waiters() const noexcept;
    /// @brief 返回共享队列分片数。
    [[nodiscard]] std::size_t num_queues() const noexcept;
    /// @brief 返回当前活跃的拓扑实例数。
    [[nodiscard]] std::size_t num_topologies() const noexcept;

private:
    /// @brief 单字段 handler 存储:函数指针 deleter 编码"借用 vs 拥有"语义
    using WorkerHandlerPtr = std::unique_ptr<WorkerHandler, void(*)(WorkerHandler*)>;

    /// @brief 共享队列分片 —— 互斥锁 + 无界队列，按 2×cache line 对齐防伪共享。
    struct alignas(2 * cache_line_size) Buffer {
        std::mutex mutex;
        UnboundedQueue<Work*> queue{2 * TFL_DEFAULT_QUEUE_SIZE};
    };

    // 64 字节对齐：防止多线程修改 m_num_topologies 时产生伪共享
    alignas(2 * cache_line_size) std::atomic<std::size_t> m_num_topologies{0};

    std::vector<Worker>                             m_workers;
    std::vector<Buffer>                             m_shared_buffers; ///< 队列缓冲区数组
    Notifier                                        m_notifier;
    WorkerHandlerPtr                                m_handler;
    unordered_dense::map<std::thread::id, Worker*>  m_worker_by_tid;


    /// @brief 真实初始化入口 —— 两个 public 构造均委托至此
    explicit Executor(WorkerHandlerPtr handler, std::size_t num_workers);

    /// @brief 根据 H 的形态构造 WorkerHandlerPtr(借用/拥有 二选一)
    template <worker_handle H>
    static auto _make_handler_ptr(H&& handler) -> WorkerHandlerPtr;

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
    void _process_dependent(Work* w, I first, S last, std::size_t& num_predecessors);

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
/// @brief 默认 handler 构造 —— 委托到核心构造,借用进程级单例
inline Executor::Executor(std::size_t num_workers)
    : Executor(DefaultWorkerHandler{}, num_workers) {}

/// @brief 用户 handler 构造 —— 委托到核心构造,handler 形态由 helper 编码
template <worker_handle H>
inline Executor::Executor(H&& handler, std::size_t num_workers)
    : Executor(_make_handler_ptr(std::forward<H>(handler)), num_workers) {}

/// @brief Helper:按 H 的值类别构造 WorkerHandlerPtr
///        - lvalue → 借用,noop deleter
///        - rvalue → 拥有,按真实类型 delete
template <worker_handle H>
inline auto Executor::_make_handler_ptr(H&& handler) -> WorkerHandlerPtr {
    using Raw = std::remove_cvref_t<H>;

    if constexpr (std::is_lvalue_reference_v<H>) {
        // ─── 借用语义:lvalue,调用方保证生命周期 ───
        return WorkerHandlerPtr{
            std::addressof(handler),
            +[](WorkerHandler*) noexcept {}
        };
    } else {
        // ─── 拥有语义:rvalue,拷贝/移动到堆,按真实类型 delete ───
        return WorkerHandlerPtr{
            new Raw(std::forward<H>(handler)),
            +[](WorkerHandler* p) noexcept {
                delete static_cast<Raw*>(p);
            }
        };
    }
}

/// @brief 真实构造体 —— 字段初始化 + 启动 worker
inline Executor::Executor(WorkerHandlerPtr handler, std::size_t num_workers)
    : m_workers{num_workers}
    , m_shared_buffers{static_cast<std::size_t>(std::bit_width(num_workers))}
    , m_notifier{num_workers}
    , m_handler{std::move(handler)}
{
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
            m_handler->on_start(wr);

            Work* w = nullptr;
            while (true) {
                // 本地队列优先：LIFO 顺序，缓存命中率最高
                while (w) {
                    try {
                        _invoke(wr, w);
                    } catch (...) {
                        if (!m_handler->on_exception(wr, std::current_exception())) {
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
            m_handler->on_stop(wr);
        });

        m_worker_by_tid.emplace(wr.m_thread.get_id(), std::addressof(wr));
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
    while (true) {
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
/// 对每个节点:注入 topology/parent 上下文 → 清除 EXCEPTION+CAUGHT 标记位
/// (仅 CAUGHT==1 时释放 m_exception_ptr) → 重算 join_length 并初始化
/// join_counter → 零入度节点 swap 到 data 前段。
///
/// 使用单次 fetch_and 同时清位+读旧值,比 load+fetch_and 省一次 RMW。
/// set_up 在 owner 线程独占阶段执行,cache line 反弹概率低。
///
/// @return 零入度源节点数量,调用方按此值批量调度 data 前段
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

/// @brief Jump 任务完成:store(0) 强制清零 target 的 join_counter,绕过正常计数协议。
///
/// @par 路径
/// - 异常/无 target:归还 parent slot
/// - 正常:store(0) 强制触发 target,slot 平移 w→target,cache 接力执行
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


/// @brief MultiJump 任务完成:对每个 target store(0) 强制触发。
///
/// 不变量:最后一个留在 cache 的 target 继承 w 的 parent slot,
/// 其余被挤出者各自 fetch_add 占新 slot 后入队。n==1 时等价于单 Jump 的 slot 平移。
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
inline void Executor::_process_dependent(Work* w, I first, S last, std::size_t& num_predecessors) {

    if (w->m_parent == nullptr) {
        _increment_topology();
    }

    for (; first != last; ++first) {
        auto* work = first->m_work;
        if (!work || w == work) {
            num_predecessors = w->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) - 1;
            continue;
        }

        auto& state = work->m_topology->m_state;

        while (true) {
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
                                            std::memory_order_acquire)) [[likely]] {
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
                                                std::memory_order_relaxed)) [[unlikely]] {
        target = Topology::State::Running;
    }

    // 唤醒等待者
    topo->m_state.notify_all();

    const std::size_t sz = w->m_num_successors;
    for (std::size_t i = 0; i < sz; ++i) {
        auto* suc = w->m_edges[i];
        if ((suc->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1)) {
            auto suc_exec = suc->m_topology->m_executor;
            if (suc_exec == this) {
                if (cache) {
                    _schedule(wr, suc);
                } else {
                    cache = suc;
                }
            } else {
                // 跨调度器调度
                suc_exec->_schedule(suc);
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
        auto target = t->m_topology->m_executor;
        if (target == this) [[likely]] {
            _schedule(w, t);            // 同 executor 走 worker-local 快路径
        } else {
            target->_schedule(t);        // 跨 executor 走全局慢路径
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
    auto itr = m_worker_by_tid.find(std::this_thread::get_id());
    return itr == m_worker_by_tid.end() ? nullptr : itr->second;
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




template <typename Gh>
    requires graph_holder<Gh>
inline void Executor::detach(Gh&& gh) {
    return detach(std::forward<Gh>(gh), 1ULL);
}

template <typename Gh, typename C>
    requires (capturable<C> && graph_holder<Gh> && callback<C>)
inline void Executor::detach(Gh&& gh, C&& cb) {
    return detach(std::forward<Gh>(gh), 1ULL, std::forward<C>(cb));
}

template <typename Gh>
    requires graph_holder<Gh>
inline void Executor::detach(Gh&& gh, std::uint64_t num) {
    return detach(std::forward<Gh>(gh), num, []() noexcept {});
}

template <typename Gh, typename C>
    requires (capturable<C> && graph_holder<Gh> && callback<C>)
inline void Executor::detach(Gh&& gh, std::uint64_t num, C&& cb) {
    // 将次数转换为谓词
    return detach(std::forward<Gh>(gh)
                        ,[num]() mutable noexcept { return num-- == 0; }
                        ,std::forward<C>(cb));
}

template <typename Gh, typename P>
    requires (capturable<P> && graph_holder<Gh> && predicate<P>)
inline void Executor::detach(Gh&& gh, P&& pred) {
    return detach(std::forward<Gh>(gh)
                        ,std::forward<P>(pred)
                        ,[]() noexcept {});
}

template <typename Gh, typename P, typename C>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
inline void Executor::detach(Gh&& gh, P&& pred, C&& cb) {
    Work* work = make_detached_flow<anchor::explicit_t>(
        /*parent=*/nullptr,
        std::forward<Gh>(gh),
        std::forward<P>(pred),
        std::forward<C>(cb));

    work->m_topology->m_executor = this;
    _increment_topology();

    // 线程本地提交：直接放入本地队列（零队列操作）
    if (Worker* wr = _this_worker(); wr) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }
}


template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable_plain<T, Args...>)
inline void Executor::detach(T&& task, Args&&... args) {
    Work* work = make_detached_basic<anchor::explicit_t>(
        this,
        /*parent=*/nullptr,
        std::forward<T>(task),
        std::forward<Args>(args)...);

    _increment_topology();

    // 线程本地提交：直接放入本地队列（零队列操作）
    if (Worker* wr = _this_worker(); wr) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable_plain<T, Args...>)
inline void Executor::detach(T&& task, Args&&... args) {
    Work* work = make_detached_runtime<anchor::explicit_t>(
        this,
        /*parent=*/nullptr,
        std::forward<T>(task),
        std::forward<Args>(args)...);

    _increment_topology();

    if (Worker* wr = _this_worker(); wr) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }
}



// ============================================================================
//  Executor::async ── 任务图提交，返回 Future<void>
// ============================================================================

template <graph_holder Gh>
inline Future<void> Executor::async(Gh&& gh) {
    return async(std::forward<Gh>(gh), 1ULL);
}

template <graph_holder Gh, typename C>
    requires (capturable<C> && callback<C>)
inline Future<void> Executor::async(Gh&& gh, C&& cb) {
    return async(std::forward<Gh>(gh), 1ULL, std::forward<C>(cb));
}

template <graph_holder Gh>
inline Future<void> Executor::async(Gh&& gh, std::uint64_t num) {
    return async(std::forward<Gh>(gh), num, []() noexcept {});
}

template <graph_holder Gh, typename C>
    requires (capturable<C> && callback<C>)
inline Future<void> Executor::async(Gh&& gh, std::uint64_t num, C&& cb) {
    return async(std::forward<Gh>(gh),
                 [num]() mutable noexcept { return num-- == 0; },
                 std::forward<C>(cb));
}

template <graph_holder Gh, typename P>
    requires (capturable<P> && predicate<P>)
inline Future<void> Executor::async(Gh&& gh, P&& pred) {
    return async(std::forward<Gh>(gh),
                 std::forward<P>(pred),
                 []() noexcept {});
}

template <graph_holder Gh, typename P, typename C>
    requires (capturable<P, C> && predicate<P> && callback<C>)
inline Future<void> Executor::async(Gh&& gh, P&& pred, C&& cb) {
    std::promise<void> promise;
    std::future<void>  std_future = promise.get_future();

    Work* work = make_promised_flow<anchor::explicit_t>(
        this,
        /*parent=*/nullptr,
        std::forward<Gh>(gh),
        std::forward<P>(pred),
        std::forward<C>(cb),
        std::move(promise));

    // Why: 必须在 _schedule 之前抓拷贝。派发后 work 可能被其它 worker
    //      立即执行并 tear_down，work->m_topology 变 use-after-free。
    //      stop_source 是 shared_ptr 语义，拷贝即共享 control block。
    std::stop_source ss = work->m_topology->m_stop_source;

    _increment_topology();

    // 线程本地提交：worker 内调用走本地队列（零队列操作）
    if (Worker* wr = _this_worker(); wr) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }

    return Future<void>{std::move(std_future), std::move(ss)};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
inline auto Executor::async(T&& task, Args&&... args) -> Future<basic_return_t<T, Args...>> {
    using R = basic_return_t<T, Args...>;

    std::promise<R> promise;
    std::future<R> std_future = promise.get_future();

    Work* work = make_promised_basic<anchor::explicit_t>(
        this,
        /*parent=*/nullptr,
        std::forward<T>(task),
        std::move(promise),
        std::forward<Args>(args)...);

    // Why: 必须在 _schedule 之前抓拷贝。派发后 work 可能被其它 worker
    //      立即执行并 tear_down，work->m_topology 变 use-after-free。
    //      stop_source 是 shared_ptr 语义，拷贝即共享 control block；
    //      Future 端持有令其在 topology 析构后仍可安全调用 request_stop。
    std::stop_source ss = work->m_topology->m_stop_source;

    _increment_topology();
    if (Worker* wr = _this_worker(); wr) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }

    return Future<R>{std::move(std_future), std::move(ss)};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
inline auto Executor::async(T&& task, Args&&... args) -> Future<runtime_return_t<T, Args...>> {
    using R = runtime_return_t<T, Args...>;

    std::promise<R> promise;
    std::future<R> std_future = promise.get_future();

    Work* work = make_promised_runtime<anchor::explicit_t>(
        this,
        /*parent=*/nullptr,
        std::forward<T>(task),
        std::move(promise),
        std::forward<Args>(args)...);

    std::stop_source ss = work->m_topology->m_stop_source;

    _increment_topology();
    if (Worker* wr = _this_worker(); wr) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }

    return Future<R>{std::move(std_future), std::move(ss)};
}

} // namespace tfl
