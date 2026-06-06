/// @file  executor.hpp
/// @brief 任务调度器 —— Work-Stealing 并行执行引擎，框架唯一的 OS 线程持有者。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
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
/// 持有 N 个 Worker 线程、共享队列和 Notifier。所有 Flow / AsyncTask / Runtime
/// 最终通过 Executor 落地执行。不可拷贝/移动，析构时等待所有在飞任务完成。
///
/// 调度算法：本地 LIFO pop → 随机 FIFO steal → Notifier 两阶段 park。
/// 提交路径自适应：当前线程是 worker 则推本地队列，否则推共享队列。
///
/// @note 构造完成后所有 public API 可从任意线程并发调用。
class Executor : public Immovable<Executor> {
    friend class Runtime;
    template <typename> friend class AsyncTask;
    TFL_WORK_SUBCLASS_FRIENDS;

public:
    // ========================================================================
    //  构造与析构
    // ========================================================================

    /// @brief 创建调度器并启动工作线程（使用默认 DefaultWorkerHandler）。
    /// @param num_workers 工作线程数量，默认值为 std::thread::hardware_concurrency()。
    /// @throw Exception 若 num_workers == 0。
    explicit Executor(std::size_t num_workers = std::thread::hardware_concurrency());

    /// @brief 创建调度器并启动工作线程（使用用户自定义 handler）。
    ///
    /// @tparam H 满足 worker_handle concept，支持以下传递方式：
    ///           - 值传递或右值 MyHandler{} / std::move(h) → 拷贝/移动到堆，
    ///             Executor 拥有生命周期；
    ///           - std::ref(h) / std::cref(h) → 借用语义，调用方保证 h 生命周期。
    /// @param handler   用户自定义的 WorkerHandler 派生对象。
    /// @param num_workers 工作线程数量，默认值为 std::thread::hardware_concurrency()。
    /// @throw Exception 若 num_workers == 0。
    template <worker_handle H>
    explicit Executor(H&& handler, std::size_t num_workers = std::thread::hardware_concurrency());

    /// @brief 等待所有已提交任务完成、停止工作线程并释放资源。
    ///
    /// 内部调用 wait_for_all() 阻塞至 m_num_topologies 归零，然后设置每
    /// 个 worker 的 terminate flag（release 语义），notify_all 唤醒所有
    /// 等待中的线程，最后 join 回收。
    ~Executor() noexcept;

    // ========================================================================
    //  任务派发 API —— 接受游离态的 AsyncTask，在本执行器内启动
    // ========================================================================

    /// @brief 将已构造的 AsyncTask 派发到本执行器，并指定上游依赖。
    ///
    /// 派发前检查任务内部 Work 节点状态：nonrepeat 模式要求 Idle→Running CAS，
    /// repeat 模式允许 Finished→Running 转换。依赖列表中的每个 AsyncTask 完成
    /// 后，通过动态依赖协议（Topology::State Locking CAS）递减当前任务的
    /// join_counter。
    ///
    /// @tparam T    任意 AsyncTask<Mode>（可为 lvalue 或 rvalue 引用）。
    /// @tparam Deps 必须为 AsyncTask<nonrepeat_t> 或其引用：仅 nonrepeat 任务
    ///              可作为依赖，因为可重复任务的状态机不保证确定性的完成语义。
    /// @param task  待派发的任务句柄。内部 Work 节点必须处于 Idle 状态（nonrepeat）
    ///              或非 Running 状态（repeat）。
    /// @param deps  上游依赖列表。task 将在所有 deps 完成后被调度。
    /// @return      若 task 为 lvalue 引用则返回该引用，否则返回值类型（支持链式）。
    /// @throw Exception 若 task 为空、已处于 Running 状态，或不处于合法状态。
    ///
    /// @note 派发后 topology->m_executor 被填入 *this，任务由本执行器独占调度。
    /// @note deps 中包含空任务或 self 时，对应的依赖边被跳过（直接 decrement counter）。
    template <typename T, typename... Deps>
        requires (any_async_task<T> && (nonrepeat_async_task<Deps> && ...))
    auto submit(T&& task, Deps&&... deps) -> std::conditional_t<std::is_lvalue_reference_v<T>, std::remove_reference_t<T>&, std::remove_cvref_t<T>>;

    /// @brief 将已构造的 AsyncTask 派发到本执行器，使用迭代器范围指定上游依赖。
    ///
    /// @tparam T 任意 AsyncTask<Mode>。
    /// @tparam I 输入迭代器，其 value_type 必须为 AsyncTask<nonrepeat_t>。
    /// @tparam S I 的 sentinel 类型。
    /// @param task   待派发的任务句柄。
    /// @param first  依赖列表起始迭代器。
    /// @param last   依赖列表结束哨兵。
    /// @return       若 task 为 lvalue 引用则返回该引用，否则返回值类型。
    /// @throw Exception 若 task 为空或不处于合法状态。
    template <typename T, std::input_iterator I, std::sentinel_for<I> S>
        requires (any_async_task<T> && nonrepeat_async_task<std::iter_value_t<I>>)
    auto submit(T&& task, I first, S last) -> std::conditional_t<std::is_lvalue_reference_v<T>, std::remove_reference_t<T>&, std::remove_cvref_t<T>>;

    // ========================================================================
    //  即发即弃 API —— 提交任务并立即返回，不提供结果获取途径
    // ========================================================================

    /// @brief 提交任务图执行一次（即发即弃）。
    /// @tparam Gh 满足 graph_holder concept 的图持有者类型。
    /// @param gh  任务图持有者（Flow 或其他 graph_holder）。
    template <typename Gh>
        requires graph_holder<Gh>
    void detach(Gh&& gh);

    /// @brief 提交任务图执行一次，完成后执行回调（即发即弃）。
    /// @tparam Gh 满足 graph_holder concept。
    /// @tparam C  满足 callback concept 的可调用对象。
    /// @param gh  任务图持有者。
    /// @param cb  完成回调。若任务抛异常，cb 仍会被调用。
    template <typename Gh, typename C>
        requires (capturable<C> && graph_holder<Gh> && callback<C>)
    void detach(Gh&& gh, C&& cb);

    /// @brief 提交任务图循环执行指定次数（即发即弃）。
    /// @tparam Gh  满足 graph_holder concept。
    /// @param gh   任务图持有者。
    /// @param num  循环次数。内部转换为谓词 [num]() mutable { return num-- == 0; }。
    template <typename Gh>
        requires graph_holder<Gh>
    void detach(Gh&& gh, std::uint64_t num);

    /// @brief 提交任务图循环执行指定次数，完成后执行回调（即发即弃）。
    /// @tparam Gh  满足 graph_holder concept。
    /// @tparam C   满足 callback concept。
    /// @param gh   任务图持有者。
    /// @param num  循环次数。
    /// @param cb   完成回调。
    template <typename Gh, typename C>
        requires (capturable<C> && graph_holder<Gh> && callback<C>)
    void detach(Gh&& gh, std::uint64_t num, C&& cb);

    /// @brief 提交任务图按谓词条件循环执行（即发即弃）。
    /// @tparam Gh   满足 graph_holder concept。
    /// @tparam P    满足 predicate concept 的可调用对象。
    /// @param gh    任务图持有者。
    /// @param pred  循环谓词。返回 true 时停止，返回 false 时继续下一轮。
    template <typename Gh, typename P>
        requires (capturable<P> && graph_holder<Gh> && predicate<P>)
    void detach(Gh&& gh, P&& pred);

    /// @brief 提交任务图按谓词循环执行，完成后执行回调（即发即弃）。
    /// @tparam Gh   满足 graph_holder concept。
    /// @tparam P    满足 predicate concept。
    /// @tparam C    满足 callback concept。
    /// @param gh    任务图持有者。
    /// @param pred  循环谓词。
    /// @param cb    完成回调。
    template <typename Gh, typename P, typename C>
        requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
    void detach(Gh&& gh, P&& pred, C&& cb);

    /// @brief 即发即弃执行单个可调用任务。
    /// @tparam T    满足 basic_invocable_plain concept 的任务体。
    /// @tparam Args 任务参数类型包。
    /// @param task  可调用对象。
    /// @param args  任务参数。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable_plain<T, Args...>)
    void detach(T&& task, Args&&... args);

    /// @brief 即发即弃执行单个运行时任务（可通过 Runtime 动态操作图）。
    /// @tparam T    满足 runtime_invocable_plain concept。
    /// @tparam Args 任务参数类型包。
    /// @param task  可调用对象（签名为 void(Runtime&, Args...)）。
    /// @param args  任务参数。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable_plain<T, Args...>)
    void detach(T&& task, Args&&... args);


    // ========================================================================
    //  异步返回 API —— 提交任务并返回 Future<R>，可阻塞等待结果
    // ========================================================================

    /// @brief 异步执行任务图一次，返回 Future<void> 用于等待完成。
    /// @tparam Gh 满足 graph_holder concept。
    /// @param gh  任务图持有者。
    /// @return    Future<void>。调用 future.get() 可阻塞等待完成并传播异常。
    template <graph_holder Gh>
    [[nodiscard]] Future<void> async(Gh&& gh);

    /// @brief 异步执行任务图一次，完成后回调，返回 Future<void>。
    ///
    /// @tparam Gh 满足 graph_holder concept。
    /// @tparam C  满足 callback concept。
    /// @param gh  任务图持有者。
    /// @param cb  完成回调。回调中的异常会被归档到 Future，通过 future.get() 可见。
    /// @return    Future<void>。
    template <graph_holder Gh, typename C>
        requires (capturable<C> && callback<C>)
    [[nodiscard]] Future<void> async(Gh&& gh, C&& cb);

    /// @brief 异步执行任务图指定次数，返回 Future<void>。
    /// @tparam Gh  满足 graph_holder concept。
    /// @param gh   任务图持有者。
    /// @param num  循环次数。
    /// @return     Future<void>。
    template <graph_holder Gh>
    [[nodiscard]] Future<void> async(Gh&& gh, std::uint64_t num);

    /// @brief 异步执行任务图指定次数，完成后回调，返回 Future<void>。
    /// @tparam Gh  满足 graph_holder concept。
    /// @tparam C   满足 callback concept。
    /// @param gh   任务图持有者。
    /// @param num  循环次数。
    /// @param cb   完成回调。
    /// @return     Future<void>。
    template <graph_holder Gh, typename C>
        requires (capturable<C> && callback<C>)
    [[nodiscard]] Future<void> async(Gh&& gh, std::uint64_t num, C&& cb);

    /// @brief 异步执行任务图按谓词条件循环，返回 Future<void>。
    /// @tparam Gh   满足 graph_holder concept。
    /// @tparam P    满足 predicate concept。
    /// @param gh    任务图持有者。
    /// @param pred  循环谓词。
    /// @return      Future<void>。
    template <graph_holder Gh, typename P>
        requires (capturable<P> && predicate<P>)
    [[nodiscard]] Future<void> async(Gh&& gh, P&& pred);

    /// @brief 异步执行任务图谓词循环 + 回调，返回 Future<void>。
    /// @tparam Gh   满足 graph_holder concept。
    /// @tparam P    满足 predicate concept。
    /// @tparam C    满足 callback concept。
    /// @param gh    任务图持有者。
    /// @param pred  循环谓词。
    /// @param cb    完成回调。
    /// @return      Future<void>。
    template <graph_holder Gh, typename P, typename C>
        requires (capturable<P, C> && predicate<P> && callback<C>)
    [[nodiscard]] Future<void> async(Gh&& gh, P&& pred, C&& cb);


    /// @brief 异步执行单个可调用任务，返回 Future<R>。
    ///
    /// @tparam T    满足 basic_invocable concept 的任务体。
    /// @tparam Args 任务参数类型包。
    /// @param task  可调用对象。
    /// @param args  任务参数。
    /// @return      Future<basic_return_t<T, Args...>>。调用 future.get()
    ///              阻塞等待任务完成并返回结果；若任务抛异常则 future.get() 重抛。
    ///
    /// @note 返回的 Future 持有 stop_source 的共享副本，确保 topology 析构后
    ///       request_stop() 仍然合法。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    [[nodiscard]] auto async(T&& task, Args&&... args) -> Future<basic_return_t<T, Args...>>;

    /// @brief 异步执行单个运行时任务，返回 Future<R>。
    ///
    /// @tparam T    满足 runtime_invocable concept 的任务体。
    /// @tparam Args 任务参数类型包。
    /// @param task  可调用对象（签名为 R(Runtime&, Args...)）。
    /// @param args  任务参数。
    /// @return      Future<runtime_return_t<T, Args...>>。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    [[nodiscard]] auto async(T&& task, Args&&... args) -> Future<runtime_return_t<T, Args...>>;

    // ========================================================================
    //  同步与状态查询
    // ========================================================================

    /// @brief 阻塞等待所有顶层任务完成。
    ///
    /// 使用 m_num_topologies 原子变量的 acquire 加载 + wait 原语实现。
    /// 底层 OS 挂起机制（Linux futex / Windows WaitOnAddress）保证低 CPU 占用。
    void wait_for_all() const noexcept;

    /// @brief 返回构造时指定的工作线程数量，运行时恒定不变。
    [[nodiscard]] std::size_t num_workers() const noexcept;

    /// @brief 返回当前因无任务可执行而阻塞在 Notifier 上的 worker 线程数（瞬时快照）。
    [[nodiscard]] std::size_t num_waiters() const noexcept;

    /// @brief 返回偷取阶段可探测的队列总数。
    /// @return num_workers() + bit_width(num_workers)，等于 worker 本地队列数加共享分片数。
    [[nodiscard]] std::size_t num_queues() const noexcept;

    /// @brief 返回调用时 m_num_topologies 的 relaxed 瞬时值，反映未完成的 async/submit/detach 拓扑数量。
    ///
    /// @note relaxed 加载无同步语义，返回值可能包含已实际完成但尚未调用 _decrement_topology 的拓扑。
    [[nodiscard]] std::size_t num_topologies() const noexcept;

private:
    /// @brief 单字段 handler 存储：unique_ptr + 函数指针 deleter。
    ///
    /// deleter 编码"借用 vs 拥有"语义：
    /// - 借用（lvalue handler）：noop deleter，不释放；
    /// - 拥有（rvalue handler）：按真实类型 delete。
    using WorkerHandlerPtr = std::unique_ptr<WorkerHandler, void(*)(WorkerHandler*)>;

    /// @brief 共享队列分片：互斥锁 + 无界队列。
    ///
    /// 按 2x cache line 对齐（通常 128 字节）防止伪共享。
    /// 分片数量 = bit_width(num_workers)，随 worker 数对数增长。
    struct alignas(2 * cache_line_size) Buffer {
        std::mutex mutex;
        UnboundedQueue<Work*> queue{2 * TFL_DEFAULT_QUEUE_SIZE};
    };

    // 2x cache line 对齐：防止多线程修改 m_num_topologies 时产生伪共享
    alignas(2 * cache_line_size) std::atomic<std::size_t> m_num_topologies{0};

    std::vector<Worker>                             m_workers;          ///< Worker 线程实体数组
    std::vector<Buffer>                             m_shared_buffers;   ///< 分片共享队列，溢出目标
    Notifier                                        m_notifier;         ///< 两阶段 park 唤醒器
    WorkerHandlerPtr                                m_handler;          ///< WorkerHandler 生命周期管理
    unordered_dense::map<std::thread::id, Worker*>  m_worker_by_tid;    ///< thread_id → Worker* 快速查找


    /// @brief 真实初始化入口：字段构造 + _spawn 启动 worker。
    ///
    /// 两个 public 构造均委托至此。m_shared_buffers 数量按 bit_width 计算。
    explicit Executor(WorkerHandlerPtr handler, std::size_t num_workers);

    /// @brief 根据 H 的值类别构造 WorkerHandlerPtr。
    ///
    /// - lvalue 引用 → 借用，deleter 为空操作；
    /// - rvalue → 拥有，heap-allocate + 按真实类型 delete。
    ///
    /// @tparam H 满足 worker_handle concept。
    template <worker_handle H>
    static auto _make_handler_ptr(H&& handler) -> WorkerHandlerPtr;

    /// @brief 创建并启动 num_workers 个工作线程，同时填充 m_worker_by_tid 映射表供 _this_worker() 查询。
    /// @param num_workers 要创建的线程数，已由构造函数保证 >= 1。
    void _spawn(std::size_t num_workers);

    /// @brief 关闭：等待全部任务 -> 设置终止标志 -> 唤醒 -> join 线程。
    void _shutdown() noexcept;

    /// @brief 任务执行入口：通过 cache 接力实现无调度开销的串行链。
    ///
    /// 任务 Work::invoke 完成后可能产出 ready 的后继（放入 cache）。
    /// 本函数在 do-while 中连续执行 cache 链，避免反复入队/出队。
    void _invoke(Worker& wr, Work* w);

    /// @brief 自适应三阶段 work-stealing：自旋窃取 → 退避让出 → 阻塞等待。
    ///
    /// @param wr 当前 worker 的上下文。
    /// @return   获取到的 Work*，或 nullptr（仅终止时）。
    [[nodiscard]] Work* _wait_for_work(Worker& wr) noexcept;

    /// @brief 重置子图所有节点到下一轮执行的初始状态。
    ///
    /// 对每个节点：注入 topology/parent 上下文 → 清除 EXCEPTION+CAUGHT 标记位
    /// → 重算 join_weight 并初始化 join_counter → 零入度节点 swap 到 data 前段。
    ///
    /// 使用单次 fetch_and 同时清位 + 读旧值，比 load + fetch_and 少一次 RMW。
    ///
    /// @return 零入度源节点数量。调用方按此值批量调度 data[0..n) 区间。
    [[nodiscard]] std::size_t _set_up_graph(Graph& g, Topology* topo, Work* parent) noexcept;

    /// @brief 任务完成后的依赖传播 —— 通过 cache 接力实现零调度开销的串行链。
    ///
        /// w 在被调度时已占 parent 的 1 个 slot。本函数退出前必须二选一：
    /// 1) 把这个 slot 平移给某个就绪后继（放入 cache 接力执行）；
    /// 2) 通过 _schedule_parent 把 slot 归还给 parent。
    void _tear_down_task(Work* w, Worker& wr, Work*& cache);

    /// @brief Branch 任务完成：将缓存的任务接力给 target 后继。
    ///
    /// 逻辑与 _tear_down_task 类似，但后继固定为单个 target（condition
    /// 节点的选中分支）。优先走"target 单前驱"快路径。
    void _tear_down_branch_task(Work* w, Worker& wr, Work*& cache, Work* target);

    /// @brief MultiBranch 任务完成：向多个 target 传播完成信号。
    ///
    /// n==1 时等价于 _tear_down_branch_task。n>=2 时第一个 ready 的 target
    /// 继承 slot，其余各占新 slot。
    void _tear_down_multi_branch_task(Work* w, Worker& wr, Work*& cache, const SmallVector<Work*>& targets);

    /// @brief Jump 任务完成：store(0) 强制清零 target 的 join_counter，
    ///        绕过正常计数协议。
    ///
    /// store(0) 等价于"所有前驱都到齐"。slot 平移 w→target，parent counter 不动。
    void _tear_down_jump_task(Work* w, Worker& wr, Work*& cache, Work* target);

    /// @brief MultiJump 任务完成：对每个 target store(0) 强制触发。
    ///
    /// 不变量：最后一个留在 cache 的 target 继承 w 的 parent slot，
    /// 其余被挤出者各自 fetch_add 占新 slot 后入队。
    void _tear_down_multi_jump_task(Work* w, Worker& wr, Work*& cache, const SmallVector<Work*>& targets);

    /// @brief 动态任务（无依赖）完成：归还 parent slot 或 decrement topology，
    ///        然后销毁 Work 节点。
    void _tear_down_async_task(Work* w, Worker& wr, Work*& cache);

    /// @brief 动态任务依赖设置：将 first..last 中的每个 AsyncTask 注册为 w 的前驱。
    ///
        /// 使用 Topology::State 的 CAS 状态机防止数据竞争：
    /// - Running  → Locking（当前线程加锁成功，可安全插入依赖边）
    /// - Locking  → 自旋等待（被其他线程持有，不触发 CAS 减少总线竞争）
    /// - Finished → 目标已完成，跳过（直接 decrement counter）
    /// - Locking  → Running（释放锁，恢复原状态）
    template <typename I, typename S>
        requires std::sentinel_for<S, I>
    void _process_dependent(Work* w, I first, S last, std::size_t& num_predecessors);

    /// @brief 动态依赖任务完成：Running→Finished 状态转移 → 唤醒等待者 →
    ///        传播完成信号给所有后继 → 引用计数管理。
    ///
    /// 支持跨调度器传播：若后继的 executor 不同于当前 executor，
    /// 则通过目标 executor 的 _schedule 推送。
    void _tear_down_dep_async_task(Work* w, Worker& wr, Work*& cache);

    /// @brief 将一个 Work* 压入哈希选中的共享分片队列，try_lock 线性探测，所有分片均忙时阻塞等锁。
    /// @param val 待入队的 Work 节点指针。
    void _push_shared(Work* val);

    /// @brief 批量将 [first, first+n) 区间的 Work* 压入同一共享分片队列，与单元素版相同 hash+try_lock 策略。
    /// @param first 起始迭代器，指向待入队的 Work* 序列。
    /// @param n     待入队元素数量。
    template <std::random_access_iterator Iterator>
        requires std::convertible_to<std::iter_reference_t<Iterator>, Work*>
    void _push_shared(Iterator first, std::size_t n);

    /// @brief 调度入口（worker 感知）：推入 worker 本地队列，溢出到共享队列。
    template <std::random_access_iterator Iterator>
    void _schedule(Worker& wr, Iterator first, std::size_t n);

    /// @brief 调度入口（非 worker）：直接推入共享队列。
    template <std::random_access_iterator Iterator>
    void _schedule(Iterator first, std::size_t n);

    /// @brief 调度入口（worker 感知，单任务）：推入 worker 本地队列。
    void _schedule(Worker& wr, Work* w);

    /// @brief 调度入口（非 worker，单任务）：直接推入共享队列。
    void _schedule(Work* w);

    /// @brief 父任务完成处理（PREEMPTED 机制）。
    ///
    /// 当子任务全部完成、父节点 join_counter 归零时：
    /// - 普通父任务：等待后续调度（不在此处入队）。
    /// - PREEMPTED 父任务：直接占据 cache 继续执行（抢占执行权，
    ///   避免刚完成的子任务 worker 的 cache 上下文丢失）。
    void _schedule_parent(Work* parent, Worker& wr, Work*& cache);

    /// @brief 将信号量释放/回滚产生的 waiters 批量重新入队。
    ///
    /// 按 waiter 归属的 executor 分派：
    /// - 同本 executor：走 worker-local deque 快路径（_schedule(worker, work)）。
    /// - 跨 executor：推送到目标 executor 全局队列（target->_schedule(work)）。
    void _schedule_from_semaphore(Worker& w, SmallVector<Work*>& waiters);

    /// @brief 协作式等待：等待谓词满足期间继续执行其他任务。
    ///
    /// 用于避免线程阻塞导致死锁（如 Runtime::wait_until）。
    /// 内部重用 work-stealing 逻辑：本地队列优先 → 窃取 → 退避。
    ///
    /// @tparam Pred 满足 predicate concept 的可调用对象。
    template <predicate Pred>
    void _cowait_until(Worker& wr, Pred&& pred);

    /// @brief 展开执行子图并协作式等待其完成。
    ///
    /// 用于 Runtime 内嵌子图的执行。设置子图节点、增加 parent counter、
    /// 调度零入度节点，然后 _cowait_until parent counter 归零。
    void _cowait_graph(Worker& wr, Graph& g, Work* parent);

    /// @brief 将 m_num_topologies 原子递增 1，使用 relaxed 序因为仅用作 wait_for_all 的计数信号，不传递其他数据。
    void _increment_topology() noexcept;

    /// @brief 活跃拓扑计数 -1：归零时 notify_all 唤醒 wait_for_all。
    void _decrement_topology() noexcept;

    /// @brief 通过 thread_id 查找当前线程对应的 Worker*。
    /// @return 若当前线程是 worker 则返回其 Worker*，否则返回 nullptr。
    [[nodiscard]] Worker* _this_worker();

};

// ============================================================================
//  内联实现
// ============================================================================

// 默认 handler 构造：委托到核心构造，借用进程级单例
inline Executor::Executor(std::size_t num_workers)
    : Executor(DefaultWorkerHandler{}, num_workers) {}

// 用户 handler 构造：委托到核心构造，handler 形态由 helper 编码
template <worker_handle H>
inline Executor::Executor(H&& handler, std::size_t num_workers)
    : Executor(_make_handler_ptr(std::forward<H>(handler)), num_workers) {}

// Helper：按 H 的值类别构造 WorkerHandlerPtr
// - lvalue → 借用，noop deleter，调用方保证生命周期
// - rvalue → 拥有，heap-allocate + 按真实类型 delete
template <worker_handle H>
inline auto Executor::_make_handler_ptr(H&& handler) -> WorkerHandlerPtr {
    using Raw = std::remove_cvref_t<H>;

    if constexpr (std::is_lvalue_reference_v<H>) {
        // 借用语义：lvalue，调用方保证生命周期
        return WorkerHandlerPtr{
            std::addressof(handler),
            +[](WorkerHandler*) noexcept {}
        };
    } else {
        // 拥有语义：rvalue，拷贝/移动到堆，按真实类型 delete
        return WorkerHandlerPtr{
            new Raw(std::forward<H>(handler)),
            +[](WorkerHandler* p) noexcept {
                delete static_cast<Raw*>(p);
            }
        };
    }
}

// 真实构造体：字段初始化 + _spawn 启动 worker
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

/// @brief 阻塞等待所有拓扑任务完成。
///
/// 使用原子 wait 原语，底层由 OS 挂起机制（Linux futex / Windows WaitOnAddress）
/// 实现，不占用 CPU 周期。
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

/// @brief 优雅关闭：等待任务完成 → 设置终止标志 → 唤醒等待线程 → join 回收。
///
/// 对每个 worker 设置 terminate flag 使用 test_and_set(release)，
/// 保证 worker 端的 acquire load 能看到终止信号（ARM/PowerPC 等弱一致性架构安全）。
inline void Executor::_shutdown() noexcept {
    wait_for_all();

    for (auto& wr : m_workers) {
        // release 保证 worker 端 acquire/relaxed load 可见 terminate flag
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
//  Work-Stealing 调度核心
// ============================================================================

/// @brief 工作线程启动入口：初始化随机种子 → on_start 回调 → 主调度循环。
///
/// 1. 执行 cache 链（Work::invoke 产出的就绪后继原地接力执行）。
/// 2. 本地队列 pop（LIFO，缓存友好）。
/// 3. 窃取阶段（_wait_for_work 三阶段算法）。
/// 4. 检查终止标志（acquire load）。
///
/// @note 异常处理：invoke 捕获的异常交给 handler->on_exception 裁决。
///       若 handler 返回 false，线程退出循环（goto exit → on_stop）。
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
            for (;;) {
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

/// @brief 自适应三阶段 work-stealing 算法。
///
/// 1. 自旋窃取：SplitMix64 随机选 victim（worker 或 shared buffer），FIFO steal；
///    成功则 adaptive_factor 递增（最多 8），尽快返回。
/// 2. 退避让出：num_steals > max_steals 时 yield；超过 yield_limit 递减
///    adaptive_factor（最少 1）并跳出。
/// 3. 阻塞等待：prepare_wait → 二次确认（避免 lost wake-up）→ commit_wait；
///    若确认有任务则 cancel_wait 并跳回 explore 重试。
///
/// @note 终止检查：每轮窃取后和阻塞前后均检查 terminate flag（acquire），
///       保证及时响应 _shutdown。
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

    // 二次确认：消除 lost wake-up（在 prepare_wait 和 commit_wait 之间
    // 可能已有任务入队并 notify）
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

TFL_FORCE_INLINE void Executor::_tear_down_task(Work* w, Worker& wr, Work*& cache) {
    // 还原 w 自身 counter：供循环子图 / Jump 目标复用，counter 簿记不可省
    w->m_join_counter.fetch_add(w->m_join_weight, std::memory_order_relaxed);

    auto* const parent = w->m_parent;
    const std::size_t sz = w->m_num_successors;

    // 无后继 或 异常：不向后传播，直接归还 w 占用的 parent slot
    if (sz == 0 || w->_has_exception()) [[unlikely]] {
        _schedule_parent(parent, wr, cache);
        return;
    }

    // 单后继路径
    // 1) 若 suc 静态入度为 1，则当前 w 是其唯一前驱。
    //    w 完成后 suc 必然 ready，可直接 store(0) + cache 接力。
    // 2) 若 suc 静态入度 > 1，则必须走正常 fetch_sub 计数协议。
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

    // 通用 fan-out
    // 不变量：第一个 ready 的 suc 继承 w 的 parent slot；
    //          后续 ready 的 suc 走 _schedule，各自占新 slot。
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

    // ready == 0：没人接班，w 占用的 parent slot 必须归还
    if (!cache) [[unlikely]] {
        _schedule_parent(parent, wr, cache);
    }
}

TFL_FORCE_INLINE void Executor::_tear_down_branch_task(Work* w, Worker& wr, Work*& cache, Work* target) {
    // 还原 w 自身 counter：为下一轮触发（循环子图 / 再调度）做准备
    w->m_join_counter.fetch_add(w->m_join_weight, std::memory_order_relaxed);
    auto* parent = w->m_parent;
    // 异常 或 无 target
    if (target == nullptr || w->_has_exception()) [[unlikely]] {
        _schedule_parent(parent, wr, cache);
        return;
    }

    // Branch 串行接力快路径：
    // target 只有当前 w 一个前驱，命中后必然 ready
    if (target->_num_predecessors() == 1) [[unlikely]] {
        target->m_join_counter.store(0, std::memory_order_relaxed);
        cache = target;
        return;
    }

    // 通用路径：target 可能还有其他前驱，必须走计数协议
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
    // 无目标 或 异常：归还 slot
    if (n == 0 || w->_has_exception()) [[unlikely]] {
        _schedule_parent(parent, wr, cache);
        return;
    }

    // 单目标快路径
    // MultiBranch 本次只命中 1 个 target，且 target 静态入度为 1，
    // 说明这个 target 只有当前 w 一个前驱，本次必然 ready。
    if (n == 1) [[unlikely]] {
        Work* const target = targets[0];

        if (target->_num_predecessors() == 1) [[unlikely]] {
            // target 入度为 1 → 本次必然归零
            // 直接 store(0)，省一次 acq_rel 原子递减。
            target->m_join_counter.store(0, std::memory_order_relaxed);
            cache = target;
            return;
        }

        // 单目标但 target 还有其他前驱，必须走正常计数协议。
        if(target->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            cache = target;
            return;
        }

        _schedule_parent(parent, wr, cache);
        return;
    }

    // 多目标通用路径
    // 不变量：第一个 ready 的 target 继承 w 的 parent slot；
    //          后续 ready 的 target 走 _schedule，各自占新 slot。
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

    // ready == 0，没人接班，w 占的 slot 必须归还
    if (!cache) [[unlikely]] {
        _schedule_parent(parent, wr, cache);
    }
}

TFL_FORCE_INLINE void Executor::_tear_down_jump_task(Work* w, Worker& wr, Work*& cache, Work* target) {
    // 还原 w 自身 counter：为下一轮触发（循环子图 / 再调度）做准备
    w->m_join_counter.fetch_add(w->m_join_weight, std::memory_order_relaxed);

    // 异常 或 无 target
    if (target == nullptr || w->_has_exception()) [[unlikely]] {
        _schedule_parent(w->m_parent, wr, cache);
        return;
    }

    // 强制触发：store(0) 等价于"所有前驱都到齐"，绕过 fetch_sub 协议
    // slot 平移 w → target，parent counter 不动，本 worker 通过 cache 接力执行
    target->m_join_counter.store(0, std::memory_order_relaxed);
    cache = target;
}


TFL_FORCE_INLINE void Executor::_tear_down_multi_jump_task(Work* w, Worker& wr, Work*& cache, const SmallVector<Work*>& targets) {
    w->m_join_counter.fetch_add(w->m_join_weight, std::memory_order_relaxed);
    auto* parent = w->m_parent;

    // 无目标 或 异常：归还 slot
    if (targets.empty() || w->_has_exception()) [[unlikely]] {
        _schedule_parent(parent, wr, cache);
        return;
    }

    // 通用路径（覆盖 n == 1 和 n >= 2）
    // 不变量：最后留在 cache 的 target 继承 w 的 parent slot；
    //          其余被挤出 cache 的 target 走 _schedule，各自占新 slot
    // - n == 1: cache 入口为 nullptr，if (cache) 跳过，直接 cache = target
    //           等价于"slot 平移"，parent counter 不动
    // - n >= 2: 前 n-1 次 fetch_add 占新 slot，最后一次平移 w 的 slot
    //           parent counter 净增 (n-1)，严格变大，不会归零
    for (auto* target : targets) {
        target->m_join_counter.store(0, std::memory_order_relaxed);
        if (cache) {
            // 之前的 cache 让位推队列，为它新占一个 parent slot
            parent->m_join_counter.fetch_add(1, std::memory_order_relaxed);
            _schedule(wr, target);
        } else {
            cache = target;
        }

    }
    // n >= 1 时循环至少进 1 次，cache 必非空，slot 已平移（n==1）或累计 (n-1) 个新 slot
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
                                            std::memory_order_acquire)) [[likely]] {
                // 加锁成功！当前线程独占修改权限
                work->m_edges.push_back(w);
                ++work->m_num_successors;

                // 解锁并恢复为原状态（target 中存的是替换前的 Idle 或 Running）
                state.store(target, std::memory_order_release);
                break;
            }
            // 5. 如果加锁失败，说明恰好有其他线程抢先了。
            // 循环会回到开头，重新 load 最新状态，完美闭环！
        }
    }
}

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


/// @brief 将单个 Work* 压入分片共享队列。
///
/// 哈希选 buffer → try_lock 线性探测 → 兜底阻塞锁。分片策略减少锁竞争。
inline void Executor::_push_shared(Work* val) {
    std::size_t const size = m_shared_buffers.size();
    std::size_t const b = detail::mulhi64(reinterpret_cast<std::uintptr_t>(val) * 11400714819323198485ULL, size);

    // 快路径：从哈希位置开始线性探测 try_lock
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

    // 所有分片均被占用，阻塞等待目标分片
    std::lock_guard<std::mutex> lock(m_shared_buffers[b].mutex);
    m_shared_buffers[b].queue.push(val);
}

template <std::random_access_iterator Iterator>
    requires std::convertible_to<std::iter_reference_t<Iterator>, Work*>
inline void Executor::_push_shared(Iterator first, std::size_t n) {
    std::size_t const size = m_shared_buffers.size();
    std::size_t const b = detail::mulhi64(reinterpret_cast<std::uintptr_t>(*first) * 11400714819323198485ULL, size);

    // 快路径：从哈希位置开始线性探测 try_lock
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

    // 所有分片均被占用，阻塞等待目标分片批量推送
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

/// @brief 任务执行入口（链式执行优化）。
///
/// Work::invoke 执行完毕后可能产出满足条件的后继（放入 cache）。
/// 本函数在 do-while 中连续执行 cache 链，避免后继任务再次入队/出队的开销。
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
    // 将次数转换为谓词：lambda 捕获 num，每次调用递减
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
        this,
        /*parent=*/nullptr,
        std::forward<Gh>(gh),
        std::forward<P>(pred),
        std::forward<C>(cb));

    work->m_topology->m_executor = this;
    _increment_topology();

    // 上下文感知调度：worker 线程内提交走本地队列（零队列操作），
    // 非 worker 线程走共享队列
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
//  Executor::async —— 任务图提交，返回 Future<void>
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

    // 上下文感知调度：worker 线程内提交走本地队列（零队列操作）
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
