/// @file executor.hpp
/// @brief Executor —— Worker 线程、任务队列、拓扑生命周期与 work-stealing 调度器。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <array>

#include "graph.hpp"
#include "traits.hpp"
#include "async_future.hpp"
#include "work_factory_fwd.hpp"
#include "worker.hpp"
#include "random.hpp"
#include "notifier.hpp"
#include "unbounded_queue.hpp"

namespace tfl {

/// @brief 拥有 Worker 线程、调度队列和通知设施，并负责执行任务图及异步任务。
///
/// Executor 构造时创建固定数量的 Worker，并通过 Worker 本地 work-stealing 队列、
/// 分片共享队列和 Notifier 协同完成任务发布、窃取、休眠与唤醒。
///
/// 每个顶层 silent_async / async / run 执行链通过 `m_num_topologies` 参与 Executor
/// 生命周期管理；析构会先等待该计数归零，再通知 Worker 退出并回收线程。
///
/// Executor 不拥有以借用方式提交的外部对象，例如左值 Graph/Flow 和 WorkerHandler；
/// 调用方必须保证这些对象在框架仍可能访问期间保持有效。
///
/// @note 不同顶层任务可由多个线程并发提交；同一图结构的修改、同一 AsyncTask 的启动
///       以及其他具有独占配置语义的操作仍须遵守各自接口契约。
/// @warning `Executor` 不可复制或移动，且不得在其 Worker 或回调仍可能访问它时销毁。
class Executor : public Immovable<Executor> {
    friend class Context;
    friend class Runtime;
    friend class SubFlow;
    friend class Work;
    friend class TaskGroup;

    template <typename> friend class AsyncTask;
    TFL_WORK_SUBCLASS_FRIENDS;

public:
    // ============================================================================
    // 构造与析构
    // ============================================================================

    /// @brief 创建不绑定 WorkerHandler 的 Executor 并立即启动工作线程。
    /// @param num_workers Worker 数量，默认使用 `std::thread::hardware_concurrency()`。
    /// @throws Exception num_workers 为 0 或达到 / 超过 Notifier 可表示的容量上限。
    explicit Executor(std::size_t num_workers = std::thread::hardware_concurrency());

    /// @brief 创建 Executor、启动工作线程，并为所有 Worker 借用同一个 WorkerHandler。
    ///
    /// Executor 不取得 @p handler 的所有权。Worker 启动、停止以及兜底异常路径
    /// 均可能访问该对象，因此其生命周期必须覆盖整个 Executor 生命周期。
    ///
    /// @param handler 外部拥有并由 Executor 借用的 WorkerHandler。
    /// @param num_workers Worker 数量，默认使用 `std::thread::hardware_concurrency()`。
    /// @throws Exception num_workers 为 0 或达到 / 超过 Notifier 可表示的容量上限。
    explicit Executor(WorkerHandler& handler, std::size_t num_workers = std::thread::hardware_concurrency());

    /// @brief 等待当前活跃顶层拓扑完成，停止所有 Worker 并回收线程资源。
    ///
    /// 析构首先通过 `wait_for_all()` 等待 `m_num_topologies` 归零，随后以 release
    /// 语义设置每个 Worker 的终止标志，唤醒可能阻塞在 Notifier 上的线程，
    /// 最后逐一 join。
    ///
    /// @note 析构开始后不得再从其他线程向该 Executor 提交新任务。
    ~Executor() noexcept;

    // ============================================================================
    // 任务派发 API —— 接受游离态 AsyncTask，在本执行器内启动
    // ============================================================================

    /// @brief 在当前 Executor 中启动一个尚未执行的 AsyncTask，并可附加动态前置依赖。
    ///
    /// `task` 通过其 Topology 控制字竞争一次性的 Idle -> Running 启动权。
    /// 对每个仍未完成的有效依赖，框架把 `task` 注册到该前驱的动态后继表；
    /// `task.m_join_counter` 记录尚未满足的依赖数，归零后才进入调度队列。
    ///
    /// 空依赖以及 `task` 自身不会建立动态边，而是立即抵消对应的初始 join 计数；
    /// 已经 Finished 的依赖同样视为已经满足。
    ///
    /// @tparam T AsyncTask 句柄类型，保留调用实参的值类别。
    /// @tparam Deps 前置 AsyncFuture / AsyncTask 依赖类型包。
    /// @param task 待启动句柄，必须非空且尚未成功启动过。
    /// @param deps 前置依赖列表。
    /// @return task 为左值时返回原引用；为右值时按值返回移动后的句柄。
    /// @throws Exception task 为空或已经离开 Idle 状态。
    ///
    /// @note 启动成功后 task 所属 Topology 的 Executor 被绑定为当前 Executor。
    template <async_task T, async_future... Deps>
    auto run(T&& task, Deps&&... deps) -> forward_return_t<T>;

    // ============================================================================
    // 即发即弃 API —— 提交任务并立即返回，不提供结果获取途径
    // ============================================================================

    /// @brief 即发即弃执行单个可调用任务。
    /// @tparam T 满足 basic_invocable concept 的任务体。
    /// @param task 可调用对象。
    template <typename T>
        requires (basic_invocable<T> && capturable<T>)
    void silent_async(T&& task);

    /// @brief 即发即弃执行单个运行时任务（可通过 Runtime 动态操作图）。
    /// @tparam T 满足 runtime_invocable concept。
    /// @param task 可调用对象；框架在调用时注入栈绑定 `Runtime&`。
    template <typename T>
        requires (runtime_invocable<T> && capturable<T>)
    void silent_async(T&& task);

    /// @brief 即发即弃执行可接收 `SubFlow&` 的动态子图 callable。
    /// @tparam T 满足 subflow_invocable concept 的任务体。
    /// @param task 要执行的 callable；框架在调用时注入栈绑定 `SubFlow&`。
    /// @warning callable 不得保存框架注入的 `SubFlow&`。
    template <typename T>
        requires (subflow_invocable<T> && capturable<T>)
    void silent_async(T&& task);

    /// @brief 提交任务图执行一次（即发即弃）；给了 @p cb 则完成后执行。
    /// @tparam Gh 满足 graph_holder concept 的图持有者类型。
    /// @tparam C  完成回调类型，默认 noop_callback（无回调）。
    /// @param gh  任务图持有者（Flow 或其他 graph_holder）。
    /// @param cb  完成回调，省略则不回调。若任务抛异常，cb 仍会被调用。
    template <graph_holder Gh, callback C = noop_callback>
        requires capturable<C>
    void silent_async(Gh&& gh, C&& cb = C{});

    /// @brief 提交任务图循环执行 @p num 次（即发即弃）；给了 @p cb 则完成后执行。
    /// @tparam Gh  满足 graph_holder concept。
    /// @tparam C   完成回调类型，默认 noop_callback（无回调）。
    /// @param gh   任务图持有者。
    /// @param num  循环次数。0 表示不执行；内部谓词不会对无符号计数产生回绕。
    /// @param cb   完成回调，省略则不回调。
    template <graph_holder Gh, callback C = noop_callback>
        requires capturable<C>
    void silent_async(Gh&& gh, std::uint64_t num, C&& cb = C{});

    /// @brief 提交任务图按谓词条件循环执行（即发即弃）；给了 @p cb 则完成后执行。
    /// @tparam Gh   满足 graph_holder concept。
    /// @tparam P    满足 predicate concept 的可调用对象。
    /// @tparam C    完成回调类型，默认 noop_callback（无回调）。
    /// @param gh    任务图持有者。
    /// @param pred  循环谓词。返回 true 时停止，返回 false 时继续下一轮。
    /// @param cb    完成回调，省略则不回调。
    template <graph_holder Gh, predicate P, callback C = noop_callback>
        requires capturable<P, C>
    void silent_async(Gh&& gh, P&& pred, C&& cb = C{});

    // ============================================================================
    // 异步返回 API —— 提交任务并返回 AsyncFuture<R>，可附加动态前置依赖
    // ============================================================================

    /// @brief 异步执行单个可调用任务，并在全部前置依赖完成后开始执行。
    ///
    /// @tparam T 满足 basic_invocable concept 的任务体。
    /// @tparam Deps 前置 AsyncFuture / AsyncTask 依赖类型包。
    /// @param task 可调用对象。
    /// @param deps 前置依赖列表；已完成依赖立即视为满足。
    /// @return AsyncFuture<basic_return_t<T>>。调用 future.get()
    ///         阻塞等待任务完成并返回结果；若任务抛异常则 future.get() 重抛。
    ///
    /// @note 返回的 AsyncFuture 通过 Work 强引用保持任务状态存活，
    ///       `request_stop()` 直接作用于该任务的独立 Topology。
    template <typename T, async_future... Deps>
        requires (basic_invocable<T> && capturable<T>)
    [[nodiscard]] auto async(T&& task, Deps&&... deps) -> AsyncFuture<basic_return_t<T>>;

    /// @brief 异步执行单个运行时任务，并在全部前置依赖完成后开始执行。
    ///
    /// @tparam T 满足 runtime_invocable concept 的任务体。
    /// @tparam Deps 前置 AsyncFuture / AsyncTask 依赖类型包。
    /// @param task 可调用对象；框架在调用时注入栈绑定 `Runtime&`。
    /// @param deps 前置依赖列表；已完成依赖立即视为满足。
    /// @return AsyncFuture<runtime_return_t<T>>。
    template <typename T, async_future... Deps>
        requires (runtime_invocable<T> && capturable<T>)
    [[nodiscard]] auto async(T&& task, Deps&&... deps) -> AsyncFuture<runtime_return_t<T>>;

    /// @brief 异步执行可接收 `SubFlow&` 的 callable，并在全部前置依赖完成后开始执行。
    ///
    /// @tparam T 满足 subflow_invocable concept 的任务体。
    /// @tparam Deps 前置 AsyncFuture / AsyncTask 依赖类型包。
    /// @param task 要执行的 callable；框架在调用时注入栈绑定 `SubFlow&`。
    /// @param deps 前置依赖列表；已完成依赖立即视为满足。
    /// @return `AsyncFuture<R>`，其中 `R = subflow_return_t<T>`。
    /// @warning callable 不得保存框架注入的 `SubFlow&`。
    template <typename T, async_future... Deps>
        requires (subflow_invocable<T> && capturable<T>)
    [[nodiscard]] auto async(T&& task, Deps&&... deps) -> AsyncFuture<subflow_return_t<T>>;

    /// @brief 异步执行任务图一次，并在全部前置依赖完成后开始执行。
    /// @tparam Gh 满足 graph_holder concept。
    /// @tparam Deps 前置 AsyncFuture / AsyncTask 依赖类型包。
    /// @param gh 任务图持有者。
    /// @param deps 前置依赖列表。
    /// @return AsyncFuture<void>。
    template <graph_holder Gh, async_future... Deps>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, Deps&&... deps);

    /// @brief 异步执行任务图一次，并在完成后执行回调。
    /// @tparam Gh 满足 graph_holder concept。
    /// @tparam C 完成回调类型。
    /// @tparam Deps 前置 AsyncFuture / AsyncTask 依赖类型包。
    /// @param gh 任务图持有者。
    /// @param cb 完成回调；回调异常会归档到 AsyncFuture。
    /// @param deps 前置依赖列表。
    /// @return AsyncFuture<void>。
    template <graph_holder Gh, callback C, async_future... Deps>
        requires capturable<C>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, C&& cb, Deps&&... deps);

    /// @brief 异步执行任务图 @p num 次，并在全部前置依赖完成后开始执行。
    /// @tparam Gh 满足 graph_holder concept。
    /// @tparam Deps 前置 AsyncFuture / AsyncTask 依赖类型包。
    /// @param gh 任务图持有者。
    /// @param num 循环次数。
    /// @param deps 前置依赖列表。
    /// @return AsyncFuture<void>。
    template <graph_holder Gh, async_future... Deps>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, std::uint64_t num, Deps&&... deps);

    /// @brief 异步执行任务图 @p num 次，并在完成后执行回调。
    /// @tparam Gh 满足 graph_holder concept。
    /// @tparam C 完成回调类型。
    /// @tparam Deps 前置 AsyncFuture / AsyncTask 依赖类型包。
    /// @param gh 任务图持有者。
    /// @param num 循环次数。
    /// @param cb 完成回调。
    /// @param deps 前置依赖列表。
    /// @return AsyncFuture<void>。
    template <graph_holder Gh, callback C, async_future... Deps>
        requires capturable<C>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, std::uint64_t num, C&& cb, Deps&&... deps);

    /// @brief 异步执行任务图按谓词条件循环，并在全部前置依赖完成后开始执行。
    /// @tparam Gh 满足 graph_holder concept。
    /// @tparam P 满足 predicate concept。
    /// @tparam Deps 前置 AsyncFuture / AsyncTask 依赖类型包。
    /// @param gh 任务图持有者。
    /// @param pred 循环谓词；返回 true 时停止，返回 false 时继续。
    /// @param deps 前置依赖列表。
    /// @return AsyncFuture<void>。
    template <graph_holder Gh, predicate P, async_future... Deps>
        requires capturable<P>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, P&& pred, Deps&&... deps);

    /// @brief 异步执行任务图按谓词条件循环，并在完成后执行回调。
    /// @tparam Gh 满足 graph_holder concept。
    /// @tparam P 满足 predicate concept。
    /// @tparam C 完成回调类型。
    /// @tparam Deps 前置 AsyncFuture / AsyncTask 依赖类型包。
    /// @param gh 任务图持有者。
    /// @param pred 循环谓词；返回 true 时停止，返回 false 时继续。
    /// @param cb 完成回调。
    /// @param deps 前置依赖列表。
    /// @return AsyncFuture<void>。
    template <graph_holder Gh, predicate P, callback C, async_future... Deps>
        requires capturable<P, C>
    [[nodiscard]] AsyncFuture<void> async(Gh&& gh, P&& pred, C&& cb, Deps&&... deps);


    // ============================================================================
    // 同步与状态查询
    // ============================================================================

    /// @brief 阻塞等待当前观察到的所有活跃顶层拓扑完成。
    ///
    /// 通过 `m_num_topologies` 的 acquire 加载和 `std::atomic::wait` 等待计数归零。
    /// 最后一个顶层拓扑完成时 `_decrement_topology()` 负责 `notify_all()`。
    ///
    /// @note 本函数不是“禁止提交”的全局栅栏。若其他线程与返回边界并发提交新任务，
    ///       新任务可能发生在本次等待观察范围之外。
    void wait_for_all() const noexcept;

    /// @brief 返回构造时指定的工作线程数量。
    /// @return Worker 数组的固定长度，Executor 生命周期内不变。
    [[nodiscard]] std::size_t num_workers() const noexcept;

    /// @brief 返回当前因无任务可执行而阻塞在 Notifier 上的 Worker 数量。
    /// @return 调用瞬间的等待者计数；调度并发会使结果立即过时。
    [[nodiscard]] std::size_t num_waiters() const noexcept;

    /// @brief 返回 Worker 在窃取阶段可选择的队列总数。
    /// @return Worker 本地队列数量与共享分片数量之和，即
    ///         `num_workers() + bit_width(num_workers())`。
    [[nodiscard]] std::size_t num_queues() const noexcept;

    /// @brief 返回当前活跃顶层拓扑计数的瞬时值。
    /// @return relaxed 加载得到的 `m_num_topologies`。
    /// @note 该接口仅用于状态观察，不建立任何同步关系；返回后计数可能立即变化。
    [[nodiscard]] std::size_t num_topologies() const noexcept;

private:
    /// @brief 保存一条由互斥锁串行访问的共享调度队列分片。
    ///
    /// 分片拥有队列存储但不拥有其中的 `Work` 节点；用于接收跨线程提交
    /// 和 Worker 本地队列溢出的任务。
    struct alignas(2 * cache_line_size) Buffer {
        std::mutex mutex;
        UnboundedQueue<Work*> queue{2LL * TFL_DEFAULT_QUEUE_SIZE};
    };

    // 2 倍缓存行对齐，使高频修改的拓扑计数尽量独占缓存区域，降低与相邻成员的伪共享。
    alignas(2 * cache_line_size) std::atomic<std::size_t> m_num_topologies{0};

    std::vector<Worker>                             m_workers;          ///< Worker 线程实体数组。
    std::vector<Buffer>                             m_shared_buffers;   ///< 分片共享队列，接收跨线程提交和本地溢出任务。
    Notifier                                        m_notifier;         ///< Worker 两阶段 park / wake 通知器。
    WorkerHandler*                                  m_handler{nullptr}; ///< 外部拥有的 WorkerHandler；nullptr 表示未绑定处理器。
    std::unordered_map<std::thread::id, Worker*>    m_tid_to_worker;    ///< Worker thread_id 到稳定 Worker 地址的只读运行期映射。

    /// @brief 统一构造入口，完成成员初始化后创建并启动全部 Worker。
    ///
    /// @param handler 可选的外部 WorkerHandler；nullptr 表示不启用生命周期和异常钩子。
    /// @param num_workers Worker 数量，进入本构造函数前尚未校验。
    ///
    /// @note Executor 不拥有 handler；非空 handler 的生命周期由调用方负责。
    Executor(WorkerHandler* handler, std::size_t num_workers);

    /// @brief 校验 Worker 数量并原样返回合法值，供成员初始化列表直接使用。
    /// @param n 请求创建的 Worker 数量。
    /// @return 校验通过后的 n。
    /// @throws Exception n 为 0 或 `n >= Notifier::capacity()`。
    static std::size_t _check_worker_count(std::size_t n);

    /// @brief 初始化 Worker 调度参数、启动线程，并建立 thread_id 到 Worker 的映射。
    /// @param num_workers 要启动的 Worker 数量。
    /// @pre num_workers 已通过 `_check_worker_count()` 校验。
    void _spawn(std::size_t num_workers);

    /// @brief 执行 Executor 关闭序列：等待顶层拓扑归零、请求终止、统一唤醒并 join Worker。
    void _shutdown() noexcept;

    /// @brief 发布一个顶层 SilentAsync Work，并维护 Executor 活跃 topology 计数.
    ///
    /// 发布前先增加 `m_num_topologies`；若调度在 Work 尚未成功发布前抛出异常，
    /// 则撤销 topology 计数并销毁该 SilentAsync Work.
    ///
    /// @param work 已完成构造、尚未发布的 SilentAsync Work.
    /// @pre work 非空，且 `_schedule()` 抛异常时保证 work 尚未被调度器发布。
    void _launch_silent_async(Work* work);


    /// @brief 发布一个顶层 Async Work，并按动态前置依赖决定是否立即调度。
    ///
    /// 返回的 Future 持有一份外部强引用；任务执行期间额外持有一份执行强引用。
    /// 存在未完成依赖时，Work 暂不进入调度队列，由最后一个完成的前驱负责发布。
    ///
    /// @tparam R 任务结果类型。
    /// @tparam Deps 前置 AsyncFuture / AsyncTask 依赖类型包。
    /// @param work 已完成构造、尚未发布的 Async Work。
    /// @param result 与 work 绑定的结果槽。
    /// @param deps 前置依赖列表。
    /// @return 关联该顶层任务的 AsyncFuture。
    template <typename R, async_future... Deps>
    [[nodiscard]] AsyncFuture<R> _launch_async(Work* work, ResultSlot<R>* result, const Deps&... deps);

    /// @brief 执行一个 Work，并沿 `cache` 接力链连续执行后续就绪任务。
    ///
    /// `Work::invoke()` 可以把一个立即就绪且适合当前线程继续执行的节点写入 cache。
    /// 本函数直接在当前调用栈中消费该节点，直到没有新的 cache，从而减少队列 push/pop、
    /// Notifier 唤醒以及再次窃取的调度成本。
    ///
    /// @param wr 当前执行 Worker。
    /// @param w 首个待执行 Work；必须非空。
    void _invoke(Worker& wr, Work* w);

    /// @brief 在当前 Worker 无本地任务时执行自适应 work-stealing 与阻塞等待。
    ///
    /// 调度分为三阶段：持续探测可窃取队列；超过快速窃取阈值后通过 yield 退避；
    /// 达到本轮上限后进入 Notifier 的 prepare / recheck / commit 两阶段等待协议，
    /// 以避免任务发布与休眠之间发生 lost wake-up。
    ///
    /// @param wr 当前 Worker。
    /// @return 获取到的 Work；仅观察到终止请求时返回 nullptr。
    [[nodiscard]] Work* _wait_for_work(Worker& wr) noexcept;

    /// @brief 为一次 Graph 执行建立节点上下文、静态 join weight、运行期计数和 source 分区。
    ///
    /// 每个节点都会重新绑定到 @p parent 及其 Topology，清除上一轮异常传播状态，
    /// 根据 strong predecessor 重新计算静态 join weight，并为非 source 节点初始化
    /// `m_join_counter`。零物理入度节点被原地交换到 `m_works` 前段。
    ///
    /// source 的判定依据物理前驱数量，而不是 strong join weight：即使某个前驱属于
    /// weak dependency，只要物理边存在，该节点仍不能作为本轮初始 source。
    ///
    /// 异常状态通过一次 `fetch_and` 同时完成清位和旧值探测；只有上一轮真正持有
    /// `EXCEPTION_CAUGHT` 的归档节点需要释放 `m_exception_ptr`。
    ///
    /// @param g 要建立运行期状态的 Graph。
    /// @param parent 本次 Graph 执行所属父 Work。
    /// @return 零物理入度 source 数量；这些节点位于 `m_works[0, n)`。
    /// @pre 本轮执行尚未发布，不存在 Worker 并发执行 g 内节点。
    [[nodiscard]] std::size_t _set_up_graph(Graph& g, Work& parent) noexcept;

    /// @brief 在 Graph 再次执行前，把非 source 节点的运行期 join_counter 恢复为静态权重。
    ///
    /// `_set_up_graph()` 已把零物理入度 source 聚集到 `[0, num_sources)`；
    /// source 不通过 predecessor join 进入本轮调度，因此这里只检查其后的节点。
    ///
    /// Branch / MultiBranch 等控制流可能使某些路径在上一轮未真正执行，但其
    /// `m_join_counter` 已被部分前驱递减。本函数以 `_join_weight()` 为基准纠正
    /// 这些残留计数，避免下一轮继承上一轮的部分到达状态。
    ///
    /// @param g 要恢复运行期依赖状态的 Graph。
    /// @param num_sources `_set_up_graph()` 得到的 source 数量。
    /// @pre 上一轮 Graph 已完全结束，不存在 Worker 仍在访问 g 内节点。
    void _reset_graph_join_counters(Graph& g, std::size_t num_sources) noexcept;

    /// @brief 完成普通静态节点的依赖传播，并把一个就绪后继通过 cache 接力执行。
    ///
    /// @p w 被调度时已经占用 parent 的一个 join slot。tear-down 后该 slot 必须保持守恒：
    /// 若存在 ready 后继，第一个 ready 节点直接继承该 slot；额外 ready 后继在发布前
    /// 各自为 parent 增加一个 slot；若没有任何 ready 后继，则把原 slot 归还给 parent。
    ///
    /// 对具有 strong predecessor 的节点，执行结束后先通过 fetch_add 恢复自身静态
    /// join weight，再传播后继，以支持循环路径在当前 tear-down 尚未结束时提前到达。
    ///
    /// @param w 已完成执行的普通静态 Work。
    /// @param wr 当前 Worker。
    /// @param cache 当前 cache 接力槽，可为空。
    void _tear_down_task(Work& w, Worker& wr, Work*& cache);

    /// @brief 完成 Branch 节点，并仅向本次选中的 target 传播一次 strong dependency 到达。
    ///
    /// Branch 先恢复自身静态 join weight；无 target 或异常时直接归还 parent slot。
    /// 有效 target 通过 `fetch_sub(1)` 参与统一 join 协议，最后一个到达者使其 ready，
    /// 并让 target 通过 cache 继承当前 slot。
    ///
    /// @param w 已完成的 Branch Work。
    /// @param wr 当前 Worker。
    /// @param cache cache 接力槽。
    /// @param target 本次分支选择的目标；nullptr 表示没有后续目标。
    void _tear_down_branch_task(Work& w, Worker& wr, Work*& cache, Work* target);

    /// @brief 完成 MultiBranch 节点，并向本次选中的多个 target 传播 strong dependency 到达。
    ///
    /// 第一个 ready target 继承当前 parent slot 并进入 cache；其余 ready target
    /// 原地压缩到 targets 前段，在统一增加 parent slot 后批量调度。
    ///
    /// @param w 已完成的 MultiBranch Work。
    /// @param wr 当前 Worker。
    /// @param cache cache 接力槽。
    /// @param targets 本次分支选择出的目标集合，可为空。
    void _tear_down_multi_branch_task(Work& w, Worker& wr, Work*& cache, SmallVector<Work*>& targets);

    /// @brief 完成 Jump 节点，并通过清零 target join_counter 绕过普通 strong join 屏障。
    ///
    /// Jump 不等待 target 的其余 strong predecessor，而是直接把其运行期 join_counter
    /// 置为 0，使 target 进入可执行状态并继承当前 parent slot。
    ///
    /// @param w 已完成的 Jump Work。
    /// @param wr 当前 Worker。
    /// @param cache cache 接力槽。
    /// @param target 跳转目标；nullptr 表示本次不跳转。
    void _tear_down_jump_task(Work& w, Worker& wr, Work*& cache, Work* target);

    /// @brief 完成 MultiJump 节点，并强制激活本次选择的全部 target。
    ///
    /// 每个 target 的 join_counter 都直接置零。最后一个 target 通过 cache 继承当前
    /// parent slot，其余 target 各增加一个额外 slot 后批量发布。
    ///
    /// @param w 已完成的 MultiJump Work。
    /// @param wr 当前 Worker。
    /// @param cache cache 接力槽。
    /// @param targets 要强制激活的目标集合，可为空。
    void _tear_down_multi_jump_task(Work& w, Worker& wr, Work*& cache, SmallVector<Work*>& targets);

    /// @brief 完成 SilentAsync Work，销毁节点并结束其父 slot 或顶层 topology 生命周期.
    ///
    /// SilentAsync 不持有外部 AsyncFuture 强引用，执行结束后立即 `destroy_work()`；
    /// 若存在 parent 则归还 parent slot，否则递减 Executor 的顶层 topology 计数.
    ///
    /// @param w 已完成的 SilentAsync Work.
    /// @param wr 当前 Worker.
    /// @param cache cache 接力槽.
    void _tear_down_silent_async_task(Work& w, Worker& wr, Work*& cache);

    /// @brief 将一组 AsyncTask 作为 @p w 的动态前驱，并修正尚未满足的依赖数量。
    ///
    /// 对每个有效且非自身的前驱，通过其 Topology `LOCKED` 位与完成路径串行化：
    /// 若前驱已经 Finished，则直接递减 w 的 join_counter；否则在持锁期间把 w
    /// 追加到前驱动态后继表。空前驱和 w 自身同样视为无需等待。
    ///
    /// @tparam I 前驱区间迭代器类型。
    /// @tparam S 前驱区间哨兵类型。
    /// @param w 正在建立动态依赖的目标 Work。
    /// @param first 前驱区间起点。
    /// @param last 前驱区间终点。
    /// @param num_predecessors 输入为初始依赖数；输出为处理期间观察到的剩余依赖数。
    template <std::forward_iterator I, std::sentinel_for<I> S>
        requires std::convertible_to<std::iter_reference_t<I>, Work*>
    void _link_predecessors(Work* w, I first, S last, std::size_t& num_predecessors);

    /// @brief 完成 AsyncTask AsyncTask，冻结动态后继表并向所有后继传播完成信号。
    ///
    /// 完成路径通过 CAS 与 `_link_predecessors()` 的 LOCKED 插边协议竞争，只有在
    /// 未锁定 Running 状态下才能发布 Finished。成功后动态边表被冻结，随后逐个
    /// 递减后继 join_counter；ready 后继按其所属 Executor 在本地 cache / 队列
    /// 或跨 Executor 共享调度路径中发布。
    ///
    /// 最后释放执行期间持有的 Work 强引用，并归还 parent slot 或顶层 topology 计数。
    ///
    /// @param w 已完成的 AsyncTask Work。
    /// @param wr 当前 Worker。
    /// @param cache cache 接力槽。
    void _tear_down_async_task(Work& w, Worker& wr, Work*& cache);

    /// @brief 将单个 Work 发布到共享分片队列。
    ///
    /// 以 Work 地址哈希得到首选分片，从该位置循环线性探测 `try_lock()`；
    /// 若全部分片均忙，则阻塞获取首选分片互斥锁后入队。
    ///
    /// @param val 待发布的 Work，必须非空。
    void _push_shared(Work* val);

    /// @brief 将 `[first, first + n)` 的 Work 批量发布到同一个共享分片。
    ///
    /// 使用首个 Work 地址选择起始分片，并采用与单任务版本相同的 try_lock
    /// 线性探测和最终阻塞回退策略。
    ///
    /// @tparam Iterator 随机访问迭代器类型。
    /// @param first 待发布区间起点。
    /// @param n 区间元素数量，必须非 0。
    template <std::random_access_iterator Iterator>
        requires std::convertible_to<std::iter_reference_t<Iterator>, Work*>
    void _push_shared(Iterator first, std::size_t n);

    /// @brief 从已知 Worker 上下文批量调度任务，优先发布到该 Worker 本地队列。
    ///
    /// 本地队列无法容纳的剩余任务由回调溢出到共享分片；发布完成后按 n 通知等待者。
    template <std::random_access_iterator Iterator>
    void _schedule(Worker& wr, Iterator first, std::size_t n);

    /// @brief 从非 Worker 上下文批量调度任务，直接发布到共享分片并通知等待者。
    template <std::random_access_iterator Iterator>
    void _schedule(Iterator first, std::size_t n);

    /// @brief 从已知 Worker 上下文调度单个任务，优先进入本地队列，满时溢出到共享分片。
    void _schedule(Worker& wr, Work* w);

    /// @brief 从非 Worker 上下文调度单个任务，直接发布到共享分片。
    void _schedule(Work* w);

    /// @brief 归还一个 parent join slot，并在 PREEMPTED 父节点归零时恢复其执行。
    ///
    /// 当前子链完成时通过 `fetch_sub(1)` 归还一个 slot。若该操作使 parent 归零：
    /// 普通父节点仅完成其等待条件，不在这里重新调度；PREEMPTED 父节点需要恢复
    /// 被子任务挂起的执行，因此接管 cache。若 cache 已被占用，原 cache 先入队。
    ///
    /// @param parent 要归还 slot 的父 Work，必须非空。
    /// @param wr 当前 Worker。
    /// @param cache cache 接力槽。
    void _schedule_parent(Work* parent, Worker& wr, Work*& cache);

    /// @brief 将 Semaphore 唤醒或回滚得到的 waiter 重新发布到各自所属 Executor。
    ///
    /// waiter 与当前 Executor 相同时走当前 Worker 的本地调度快路径；跨 Executor
    /// waiter 直接进入目标 Executor 的共享调度入口。
    ///
    /// @param w 当前 Worker。
    /// @param waiters 已具备继续执行条件的 Work 集合。
    void _schedule_from_semaphore(Worker& w, SmallVector<Work*>& waiters);

    /// @brief 在当前 Worker 上协作执行其他任务，直到 @p pred 返回 true。
    ///
    /// 该等待不会阻塞 Worker：优先消费本地队列，本地为空时持续从全部可见队列
    /// 窃取，并在连续失败后通过 yield 退避。每轮窃取过程中都会重新检查结束谓词。
    ///
    /// @tparam Pred 满足 predicate concept 的结束谓词类型。
    /// @param worker 当前 Worker。
    /// @param pred 返回 true 时结束协作等待的谓词。
    template <predicate Pred>
    void _corun_until(Worker& worker, Pred&& pred);

    /// @brief 在当前 Worker 上启动一个 Graph，并协作执行直到该 Graph 占用的 parent slot 全部归还。
    ///
    /// 首先通过 `_set_up_graph()` 建立节点运行期状态并取得 source；若没有 source
    /// 则直接返回。否则为每个 source 增加一个 parent join slot，批量发布 source，
    /// 再通过 `_corun_until()` 持续执行可用任务直到 parent join_counter 归零。
    ///
    /// @param worker 当前 Worker。
    /// @param graph 要执行的 Graph。
    /// @param parent Graph 所属父 Work。
    void _corun_graph(Graph& g, Work& parent, Worker& wr);

    /// @brief 为一个新启动的顶层执行链增加 Executor 活跃 topology 计数。
    ///
    /// 增量本身只负责生命周期计数，因此使用 relaxed；完成侧通过 release 序列发布结束。
    void _increment_topology() noexcept;

    /// @brief 结束一个顶层执行链；最后一个 topology 离开时唤醒全部 `wait_for_all()` 等待者。
    ///
    /// `fetch_sub` 使用 acq_rel；当旧值为 1 时新值变为 0，并调用 `notify_all()`。
    void _decrement_topology() noexcept;

    /// @brief 根据当前 `std::thread::id` 查询是否运行在本 Executor 的 Worker 线程上。
    /// @return 当前线程对应的 Worker；非本 Executor Worker 时返回 nullptr。
    /// @note 映射在 Executor 构造阶段建立，正常运行期间只读。
    [[nodiscard]] Worker* _this_worker();

};

// ============================================================================
// 内联实现
// ============================================================================
inline std::size_t Executor::_check_worker_count(std::size_t n) {
    if (n == 0) {
        throw Exception("Executor must define at least one worker.");
    }

    if (n >= Notifier::capacity()) {
        throw Exception("Executor worker count exceeds Notifier 16-bit capacity (max 65534).");
    }

    return n;
}

inline Executor::Executor(WorkerHandler* handler, std::size_t num_workers)
    : m_workers{_check_worker_count(num_workers)}
    , m_shared_buffers{static_cast<std::size_t>(std::bit_width(num_workers))}
    , m_notifier{num_workers}
    , m_handler{handler}
{
    try {
        _spawn(num_workers);
    } catch (...) {
        _shutdown();
        throw;
    }
}

inline Executor::Executor(std::size_t num_workers)
    : Executor(nullptr, num_workers) {}

inline Executor::Executor(WorkerHandler& handler, std::size_t num_workers)
    : Executor(std::addressof(handler), num_workers) {}

inline Executor::~Executor() noexcept {
    _shutdown();
}

// ============================================================================
// Executor::run(AsyncTask)
// ============================================================================
template <async_task T, async_future... Deps>
inline auto Executor::run(T&& task, Deps&&... deps) -> forward_return_t<T> {
    task._start(*this, std::forward<Deps>(deps)...);
    return std::forward<T>(task);
}


inline void Executor::_launch_silent_async(Work* work) {
    TFL_ASSERT(work);

    _increment_topology();
    if (Worker* worker = _this_worker()) {
        _schedule(*worker, work);
    } else {
        _schedule(work);
    }
}

template <typename R, async_future... Deps>
inline AsyncFuture<R> Executor::_launch_async(Work* work, ResultSlot<R>* result, const Deps&... deps) {
    TFL_ASSERT(work);
    TFL_ASSERT(result);

    AsyncFuture<R> future{work, result};

    auto& control = work->m_topology->m_control;
    auto current = control.load(std::memory_order_relaxed);
    control.store(Topology::Control::set_status(current, Topology::Control::Status::Running), std::memory_order_relaxed);

    // 执行生命周期额外持有一份强引用，由 Async tear-down 释放。
    work->_increment_ref();
    _increment_topology();

    if constexpr (sizeof...(Deps) != 0) {
        std::array<Work*, sizeof...(Deps)> predecessors{deps.m_work...};
        std::size_t num_predecessors = sizeof...(Deps);

        work->m_join_counter.store(num_predecessors, std::memory_order_relaxed);
        _link_predecessors(work, predecessors.begin(), predecessors.end(), num_predecessors);

        if (num_predecessors != 0) {
            return future;
        }
    }

    if (Worker* worker = _this_worker()) {
        _schedule(*worker, work);
    } else {
        _schedule(work);
    }

    return future;
}

// ============================================================================
// Executor：独立顶层 fire-and-forget
// ============================================================================
template <typename T>
    requires (basic_invocable<T> && capturable<T>)
inline void Executor::silent_async(T&& task) {
    Work* work = make_silent_async_basic(*this, nullptr, nullptr, std::forward<T>(task));
    _launch_silent_async(work);
}

template <typename T>
    requires (runtime_invocable<T> && capturable<T>)
inline void Executor::silent_async(T&& task) {
    Work* work = make_silent_async_runtime(*this, nullptr, nullptr, std::forward<T>(task));
    _launch_silent_async(work);
}

template <typename T>
    requires (subflow_invocable<T> && capturable<T>)
inline void Executor::silent_async(T&& task) {
    Work* work = make_silent_async_subflow(*this, nullptr, nullptr, std::forward<T>(task));
    _launch_silent_async(work);
}

template <graph_holder Gh, callback C>
    requires capturable<C>
inline void Executor::silent_async(Gh&& gh, C&& cb) {
    silent_async(std::forward<Gh>(gh), 1ULL, std::forward<C>(cb));
}

template <graph_holder Gh, callback C>
    requires capturable<C>
inline void Executor::silent_async(Gh&& gh, std::uint64_t num, C&& cb) {
    silent_async(std::forward<Gh>(gh), [num]() mutable noexcept  -> bool { return num-- == 0; }, std::forward<C>(cb));
}

template <graph_holder Gh, predicate P, callback C>
    requires capturable<P, C>
inline void Executor::silent_async(Gh&& gh, P&& pred, C&& cb) {
    Work* work = make_silent_async_module(*this, nullptr, nullptr, std::forward<Gh>(gh), std::forward<P>(pred), std::forward<C>(cb));
    _launch_silent_async(work);
}


// ============================================================================
// Executor：独立顶层带结果任务
// ============================================================================

template <typename T, async_future... Deps>
    requires (basic_invocable<T> && capturable<T>)
inline auto Executor::async(T&& task, Deps&&... deps) -> AsyncFuture<basic_return_t<T>> {
    auto [work, result] = make_async_basic(*this, nullptr, nullptr, std::forward<T>(task));
    return _launch_async(work, result, std::forward<Deps>(deps)...);
}

template <typename T, async_future... Deps>
    requires (runtime_invocable<T> && capturable<T>)
inline auto Executor::async(T&& task, Deps&&... deps) -> AsyncFuture<runtime_return_t<T>> {
    auto [work, result] = make_async_runtime(*this, nullptr, nullptr, std::forward<T>(task));
    return _launch_async(work, result, std::forward<Deps>(deps)...);
}

template <typename T, async_future... Deps>
    requires (subflow_invocable<T> && capturable<T>)
inline auto Executor::async(T&& task, Deps&&... deps) -> AsyncFuture<subflow_return_t<T>> {
    auto [work, result] = make_async_subflow(*this, nullptr, nullptr, std::forward<T>(task));
    return _launch_async(work, result, std::forward<Deps>(deps)...);
}

template <graph_holder Gh, async_future... Deps>
inline AsyncFuture<void> Executor::async(Gh&& gh, Deps&&... deps) {
    return async(std::forward<Gh>(gh), std::uint64_t{1}, noop_callback{}, std::forward<Deps>(deps)...);
}

template <graph_holder Gh, callback C, async_future... Deps>
    requires capturable<C>
inline AsyncFuture<void> Executor::async(Gh&& gh, C&& cb, Deps&&... deps) {
    return async(std::forward<Gh>(gh), std::uint64_t{1}, std::forward<C>(cb), std::forward<Deps>(deps)...);
}

template <graph_holder Gh, async_future... Deps>
inline AsyncFuture<void> Executor::async(Gh&& gh, std::uint64_t num, Deps&&... deps) {
    return async(std::forward<Gh>(gh),
                 [num]() mutable noexcept -> bool { return num-- == 0; },
                 noop_callback{},
                 std::forward<Deps>(deps)...);
}

template <graph_holder Gh, callback C, async_future... Deps>
    requires capturable<C>
inline AsyncFuture<void> Executor::async(Gh&& gh, std::uint64_t num, C&& cb, Deps&&... deps) {
    return async(std::forward<Gh>(gh),
                 [num]() mutable noexcept -> bool { return num-- == 0; },
                 std::forward<C>(cb),
                 std::forward<Deps>(deps)...);
}

template <graph_holder Gh, predicate P, async_future... Deps>
    requires capturable<P>
inline AsyncFuture<void> Executor::async(Gh&& gh, P&& pred, Deps&&... deps) {
    return async(std::forward<Gh>(gh),
                 std::forward<P>(pred),
                 noop_callback{},
                 std::forward<Deps>(deps)...);
}

template <graph_holder Gh, predicate P, callback C, async_future... Deps>
    requires capturable<P, C>
inline AsyncFuture<void> Executor::async(Gh&& gh, P&& pred, C&& cb, Deps&&... deps) {
    auto [work, result] = make_async_module(*this,
                                               nullptr,
                                               nullptr,
                                               std::forward<Gh>(gh),
                                               std::forward<P>(pred),
                                               std::forward<C>(cb));

    return _launch_async(work, result, std::forward<Deps>(deps)...);
}

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

inline void Executor::_shutdown() noexcept {
    wait_for_all();

    for (auto& wr : m_workers) {
        // release 发布终止请求；Worker 在调度循环中通过 acquire test() 观察该状态。
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
// Work-Stealing 调度循环
// ============================================================================

inline void Executor::_spawn(std::size_t num_workers) {
    const std::size_t num_queues = this->num_queues();

    m_tid_to_worker.reserve(num_workers);

    for (std::size_t id = 0; id < num_workers; ++id) {
        auto& wr = m_workers[id];
        wr.m_id = id;
        wr.m_vtm = (id + 1) % num_queues;
        wr.m_max_steals = static_cast<std::uint32_t>((std::min)(num_queues * 2, std::size_t{64}));
        wr.m_max_yields = 256;

        wr.m_thread = std::thread([this, &wr, num_queues]() noexcept {
            wr.m_rng.seed(std::hash<std::thread::id>{}(std::this_thread::get_id()), static_cast<std::uint32_t>(num_queues));

            if (m_handler) {
                m_handler->on_start(wr);
            }

            std::exception_ptr exception;

            try {
                Work* w = nullptr;

                for (;;) {
                    while (w) {
                        _invoke(wr, w);
                        w = wr.m_wslq.pop();
                    }

                    if ((w = _wait_for_work(wr)) == nullptr) [[unlikely]] {
                        break;
                    }
                }
            } catch (...) {
                exception = std::current_exception();
            }

            if (m_handler) {
                m_handler->on_stop(wr, exception);
            }
        });

        m_tid_to_worker.emplace(wr.m_thread.get_id(), std::addressof(wr));
    }
}

inline Work* Executor::_wait_for_work(Worker& wr) noexcept {
    const std::size_t nw = m_workers.size();
    const std::size_t nb = m_shared_buffers.size();
    const std::size_t id = wr.m_id;

explore:
    std::size_t vtm = wr.m_vtm;
    std::uint32_t num_steals = 0;
    std::uint32_t num_yields = 0;

    // 阶段一、二：
    // 先在 m_max_steals 预算内持续快速窃取；
    // 超过快速窃取预算后进入 steal + yield 阶段；
    // 持续 yield 达到 m_max_yields 后才准备进入阻塞等待。
    for (;;) {
        Work* w = (vtm < nw)
        ? m_workers[vtm].m_wslq.steal()
        : m_shared_buffers[vtm - nw].queue.steal();

        if (w) {
            wr.m_vtm = vtm;
            return w;
        }

        if (++num_steals > wr.m_max_steals) {
            std::this_thread::yield();

            if (++num_yields >= wr.m_max_yields) {
                break;
            }
        }

        if (wr.m_terminate.test(std::memory_order_acquire)) [[unlikely]] {
            return nullptr;
        }

        vtm = wr.m_rng();
    }

    // 阶段三：进入 Notifier 两阶段等待；prepare 后必须重新检查所有可见队列。
    m_notifier.prepare_wait(id);

    // 二次确认：prepare_wait 与 commit_wait 之间可能已有任务入队并 notify；
    // 此处重新扫描共享队列和其他 Worker 本地队列，发现工作则 cancel_wait，
    // 从而避免在已有可执行任务时错误进入休眠。
    for (std::size_t i = 0; i < nb; ++i) {
        if (!m_shared_buffers[i].queue.empty()) {
            m_notifier.cancel_wait(id);
            wr.m_vtm = i + nw;
            goto explore;
        }
    }

    for (std::size_t i = 0; i < id; ++i) {
        if (!m_workers[i].m_wslq.empty()) {
            m_notifier.cancel_wait(id);
            wr.m_vtm = i;
            goto explore;
        }
    }

    for (std::size_t i = id + 1; i < nw; ++i) {
        if (!m_workers[i].m_wslq.empty()) {
            m_notifier.cancel_wait(id);
            wr.m_vtm = i;
            goto explore;
        }
    }

    if (wr.m_terminate.test(std::memory_order_acquire)) [[unlikely]] {
        m_notifier.cancel_wait(id);
        return nullptr;
    }

    m_notifier.commit_wait(id);
    goto explore;
}

TFL_FORCE_INLINE std::size_t Executor::_set_up_graph(Graph& g, Work& parent) noexcept {
    Work** const data = g.m_works.data();
    const std::size_t size = g.m_works.size();
    std::size_t n = 0;
    auto* parent_ptr = std::addressof(parent);
    auto* topology = parent.m_topology;

    constexpr auto exc_mask = Work::Control::EXCEPTION | Work::Control::EXCEPTION_CAUGHT;

    for (std::size_t i = 0; i < size; ++i) {
        Work* w = data[i];
        w->m_parent = parent_ptr;
        w->m_topology = topology;

        // 清除上一轮异常传播位，并利用 fetch_and 返回的旧值判断是否持有异常归档。
        // 只有 EXCEPTION_CAUGHT 节点实际保存 m_exception_ptr；仅带 EXCEPTION 的路径节点
        // 只是传播标记，因此无需额外清理 exception_ptr。
        if (w->m_control.fetch_and(~exc_mask, std::memory_order_relaxed) & Work::Control::EXCEPTION_CAUGHT) [[unlikely]] {
            w->m_exception_ptr = nullptr;
        }

        // 重新计算静态 strong predecessor 数量，并写入 Properties 的 join-weight 低位。
        const std::size_t join_weight = w->_compute_join_weight();
        TFL_ASSERT(join_weight <= Work::Properties::JOIN_WEIGHT_MAX);

        w->m_properties = (w->m_properties & Work::Properties::FLAG_MASK) | static_cast<Work::Properties::type>(join_weight);

        // source 按“零物理入度”判定并原地聚集到 [0, n)。
        // weak predecessor 虽不计入 join weight，但仍是物理前驱，因此存在 weak 前驱的节点
        // 不能作为本轮初始 source。
        if (w->_num_predecessors() == 0) {
            std::swap(data[i], data[n++]);
        } else {
            w->m_join_counter.store(join_weight, std::memory_order_relaxed);
        }
    }

    return n;
}

TFL_FORCE_INLINE void Executor::_reset_graph_join_counters(Graph& g, std::size_t num_sources) noexcept {
    Work** const data = g.m_works.data();
    const std::size_t size = g.m_works.size();

    TFL_ASSERT(num_sources <= size);

    for (std::size_t i = num_sources; i < size; ++i) {
        Work* const w = data[i];
        const auto join_weight = w->_join_weight();

        if (w->m_join_counter.load(std::memory_order_relaxed) != join_weight) [[unlikely]] {
            w->m_join_counter.store(join_weight, std::memory_order_relaxed);
        }
    }
}


TFL_FORCE_INLINE void Executor::_tear_down_task(Work& w, Worker& wr, Work*& cache) {
    auto* const parent = w.m_parent;
    const std::size_t sz = w.m_num_successors;
    const auto join_weight = w._join_weight();

    // 恢复当前节点下一次激活所需的 strong dependency join 计数。
    //
    // join_weight == 0：
    //   当前节点没有 strong predecessor，不参与 join_counter 协议。
    //
    // join_weight > 0：
    //   当前节点统一通过 fetch_sub 参与 strong dependency join；
    //   最后一个 strong predecessor 将计数从 1 递减到 0 并获得执行权。
    //   当前节点执行完成后重新加回静态 join weight，为下一次激活恢复计数。
    //
    // 必须使用 fetch_add 而不能 store：循环图中当前节点执行期间，
    // 下一次激活的 strong predecessor 可能已经提前递减 join_counter。
    // fetch_add 能保留这些已经发生的到达，而 store 会覆盖并丢失。
    //
    // 恢复必须发生在传播后继之前，否则后继可能沿循环路径重新激活
    // 当前节点，并发访问尚未恢复的 join_counter。
    if (join_weight != 0) [[likely]] {
        w.m_join_counter.fetch_add(join_weight, std::memory_order_relaxed);
    }

    // 异常路径停止向后传播，归还当前 w 占用的 parent slot。
    if (w._has_exception()) [[unlikely]] {
        _schedule_parent(parent, wr, cache);
        return;
    }

    // 第一个 ready 后继直接继承当前 w 的 parent slot 并进入 cache；
    // 后续 ready 后继原地聚集到 m_edges 前段 [0, num_ready)，
    // 最后统一增加额外 parent slot 并批量调度。
    //
    // 所有具有 strong predecessor 的后继统一通过 join_counter 同步：
    //
    // join_weight == 1：
    //   唯一 strong predecessor 将计数从 1 递减到 0，并直接获得执行权。
    //
    // join_weight > 1：
    //   每个 strong predecessor 递减一次计数，最后一个将计数从 1
    //   递减到 0 的前驱负责激活该后继。
    std::size_t num_ready = 0;

    for (std::size_t i = 0; i < sz; ++i) {
        Work* const suc = w.m_edges[i];

        if (suc->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (cache) {
                // 第一个 ready 已由 cache 接管，因此后续 ready 的聚集位置
                // num_ready 始终位于当前扫描位置 i 之前，不会发生 self-swap。
                std::swap(w.m_edges[i], w.m_edges[num_ready++]);
            } else {
                // 第一个 ready 后继继承当前 w 的 parent slot。
                cache = suc;
            }
        }
    }

    // 没有任何后继 ready，当前 w 占用的 parent slot 无人继承。
    if (!cache) {
        _schedule_parent(parent, wr, cache);
        return;
    }

    // cache 已继承当前 w 的 parent slot，其余 ready 后继各占一个新的 parent slot。
    //
    // 必须先增加 parent 计数，再发布任务，避免后继快速完成导致
    // parent 尚未建立完整计数就提前归零。
    if (num_ready != 0) {
        parent->m_join_counter.fetch_add(num_ready, std::memory_order_relaxed);

        if (num_ready == 1) {
            _schedule(wr, w.m_edges[0]);
        } else {
            _schedule(wr, w.m_edges.begin(), num_ready);
        }
    }
}

TFL_FORCE_INLINE void Executor::_tear_down_branch_task(Work& w, Worker& wr, Work*& cache, Work* target) {
    auto* const parent = w.m_parent;
    const auto join_weight = w._join_weight();

    // 恢复当前节点下一次激活所需的 strong dependency join 计数。
    //
    // join_weight == 0 的节点没有 strong predecessor，不参与 join_counter；
    // join_weight > 0 的节点执行完成后重新加回静态 join weight。
    //
    // 使用 fetch_add 而不能 store，以保留当前节点执行期间可能已经提前
    // 发生的下一次 strong predecessor 到达。
    //
    // 必须在传播 target 之前恢复，否则 target 可能沿循环路径重新激活
    // 当前节点，并发访问尚未恢复的 join_counter。
    if (join_weight != 0) [[likely]] {
        w.m_join_counter.fetch_add(join_weight, std::memory_order_relaxed);
    }

    // 本次 Branch 未选择目标，当前 w 占用的 parent slot 无人继承。
    if (!target) {
        _schedule_parent(parent, wr, cache);
        return;
    }

    // 异常路径停止向后传播。
    if (w._has_exception()) [[unlikely]] {
        _schedule_parent(parent, wr, cache);
        return;
    }

    // target 统一通过 join_counter 参与 strong dependency join：
    //
    // join_weight == 1：
    //   唯一 strong predecessor 将计数从 1 递减到 0 并获得执行权。
    //
    // join_weight > 1：
    //   最后一个将计数从 1 递减到 0 的 strong predecessor
    //   负责激活 target。
    if (target->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        cache = target;
        return;
    }

    // target 尚未 ready，当前 w 的 parent slot 无人继承。
    _schedule_parent(parent, wr, cache);
}

TFL_FORCE_INLINE void Executor::_tear_down_multi_branch_task(Work& w, Worker& wr, Work*& cache, SmallVector<Work*>& targets) {
    auto* const parent = w.m_parent;
    const auto join_weight = w._join_weight();

    // 恢复当前节点下一次激活所需的 strong dependency join 计数。
    //
    // join_weight == 0 的节点没有 strong predecessor，不参与 join_counter；
    // join_weight > 0 的节点执行完成后重新加回静态 join weight。
    //
    // 使用 fetch_add 而不能 store，以保留当前节点执行期间可能已经提前
    // 发生的下一次 strong predecessor 到达。
    //
    // 必须在传播 targets 之前恢复，否则某个 target 可能沿循环路径
    // 重新激活当前节点，并发访问尚未恢复的 join_counter。
    if (join_weight != 0) [[likely]] {
        w.m_join_counter.fetch_add(join_weight, std::memory_order_relaxed);
    }

    // 异常路径停止向后传播。
    if (w._has_exception()) [[unlikely]] {
        _schedule_parent(parent, wr, cache);
        return;
    }

    // 第一个 ready target 继承当前 w 的 parent slot，并直接进入 cache；
    // 后续 ready target 原地压缩到 targets 前段 [0, num_ready)，
    // 最后统一增加额外 parent slot 并调度。
    //
    // 所有具有 strong predecessor 的 target 统一通过 join_counter 同步，
    // 最后一个将计数从 1 递减到 0 的 strong predecessor 获得执行权。
    std::size_t num_ready = 0;

    for (Work* const target : targets) {
        if (target->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (cache) {
                targets[num_ready++] = target;
            } else {
                cache = target;
            }
        }
    }

    // 没有任何 target ready，当前 w 占用的 parent slot 无人继承。
    if (!cache) {
        _schedule_parent(parent, wr, cache);
        return;
    }

    // cache 已继承当前 w 的 parent slot，因此只需为其余 num_ready 个
    // ready target 增加新的 parent slot。
    //
    // 必须先增加 parent 计数，再发布任务，避免 target 快速完成导致
    // parent 尚未建立完整计数就提前归零。
    if (num_ready != 0) {
        parent->m_join_counter.fetch_add(num_ready, std::memory_order_relaxed);

        if (num_ready == 1) {
            _schedule(wr, targets[0]);
        } else {
            _schedule(wr, targets.begin(), num_ready);
        }
    }
}

TFL_FORCE_INLINE void Executor::_tear_down_jump_task(Work& w, Worker& wr, Work*& cache, Work* target) {
    auto* const parent = w.m_parent;
    const auto join_weight = w._join_weight();

    // 恢复当前节点下一次激活所需的 strong dependency join 计数。
    //
    // join_weight == 0 的节点没有 strong predecessor，不参与 join_counter；
    // join_weight > 0 的节点执行完成后重新加回静态 join weight。
    //
    // 使用 fetch_add 而不能 store，以保留当前节点执行期间可能已经提前
    // 发生的下一次 strong predecessor 到达。
    if (join_weight != 0) [[likely]] {
        w.m_join_counter.fetch_add(join_weight, std::memory_order_relaxed);
    }

    // 无目标或异常：不发生跳转，归还当前 parent slot。
    if (target == nullptr || w._has_exception()) [[unlikely]] {
        _schedule_parent(parent, wr, cache);
        return;
    }

    // Jump 绕过 target 的普通 strong dependency join 屏障并强制激活。
    //
    // 普通依赖激活时，最后一个 strong predecessor 会将 join_counter
    // 从 1 递减到 0；Jump 没有执行这次 fetch_sub，因此这里直接置零，
    // 使 target 进入与普通 join 完成后相同的运行期状态。
    //
    // 对 join_weight == 0 的 target，该 relaxed store 只是重复写入零，
    // 不改变节点语义。
    target->m_join_counter.store(0, std::memory_order_relaxed);

    cache = target;
}

TFL_FORCE_INLINE void Executor::_tear_down_multi_jump_task(Work& w, Worker& wr, Work*& cache, SmallVector<Work*>& targets) {
    auto* const parent = w.m_parent;
    std::size_t n = targets.size();
    const auto join_weight = w._join_weight();

    // 恢复当前节点下一次激活所需的 strong dependency join 计数。
    //
    // join_weight == 0 的节点没有 strong predecessor，不参与 join_counter；
    // join_weight > 0 的节点执行完成后重新加回静态 join weight。
    //
    // 使用 fetch_add 而不能 store，以保留当前节点执行期间可能已经提前
    // 发生的下一次 strong predecessor 到达。
    if (join_weight != 0) [[likely]] {
        w.m_join_counter.fetch_add(join_weight, std::memory_order_relaxed);
    }

    // 无目标或异常：不发生跳转，归还当前 parent slot。
    if (n == 0 || w._has_exception()) [[unlikely]] {
        _schedule_parent(parent, wr, cache);
        return;
    }

    // 所有 target 均由 MultiJump 强制激活，不经过普通 strong dependency join。
    //
    // 普通激活时最后一个 strong predecessor 会将 join_counter 递减至零；
    // MultiJump 没有执行这些 fetch_sub，因此统一置零，使每个 target
    // 进入与普通 join 完成后相同的运行期状态。
    for (Work* const target : targets) {
        target->m_join_counter.store(0, std::memory_order_relaxed);
    }

    // 最后一个 target 继承当前 w 已占用的 parent slot，并通过 cache 接力执行。
    cache = targets[--n];

    // 剩余 n 个 target 无法共享原 slot，因此每个 target 各占一个新的 parent slot。
    //
    // 必须先增加 parent 计数，再发布任务，避免 target 快速完成导致
    // parent 尚未建立完整计数就提前归零。
    if (n != 0) {
        parent->m_join_counter.fetch_add(n, std::memory_order_relaxed);

        if (n == 1) {
            _schedule(wr, targets[0]);
        } else {
            _schedule(wr, targets.begin(), n);
        }
    }
}


/// @brief 完成 SilentAsync 异步任务，立即销毁 Work，并结束所属父 slot 或顶层 topology。
///
/// SilentAsync 不向调用方暴露结果句柄，因此执行完成后不需要为外部观察者保留 Work。
/// 函数先缓存 parent，再销毁当前节点；之后若存在 parent 则归还一个 join slot，
/// 否则递减 Executor 的顶层 topology 计数。
TFL_FORCE_INLINE void Executor::_tear_down_silent_async_task(Work& w, Worker& wr, Work*& cache) {
    Work* const parent = w.m_parent;

    // parent 已提前保存；SilentAsync 没有外部强引用，当前执行结束后即可立即回收 Work。
    destroy_work(std::addressof(w));

    if (parent) {
        _schedule_parent(parent, wr, cache);
    } else {
        _decrement_topology();
    }
}


template <std::forward_iterator I, std::sentinel_for<I> S>
    requires std::convertible_to<std::iter_reference_t<I>, Work*>
TFL_FORCE_INLINE void Executor::_link_predecessors(Work* w, I first, S last, std::size_t& num_predecessors) {
    for (; first != last; ++first) {
        Work* const work = *first;

        if (!work || work == w) {
            num_predecessors = w->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) - 1;
            continue;
        }

        auto& control = work->m_topology->m_control;
        auto current = control.load(std::memory_order_acquire);

        for (;;) {
            const auto status = Topology::Control::status(current);

            if (status == Topology::Control::Status::Finished) {
                num_predecessors = w->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) - 1;
                break;
            }

            TFL_ASSERT(status == Topology::Control::Status::Idle || status == Topology::Control::Status::Running);

            // CAS 只允许从未锁定状态获取动态依赖锁。
            current &= ~Topology::Control::LOCKED;

            if (control.compare_exchange_weak(current,
                                              current | Topology::Control::LOCKED,
                                              std::memory_order_acquire,
                                              std::memory_order_acquire)) {
                work->m_edges.push_back(w);
                ++work->m_num_successors;

                control.fetch_and(~Topology::Control::LOCKED, std::memory_order_release);
                break;
            }
        }
    }
}

TFL_FORCE_INLINE void Executor::_tear_down_async_task(Work& w, Worker& wr, Work*& cache) {
    Topology* const topology = w.m_topology;
    Work* const parent = w.m_parent;
    auto& control = topology->m_control;

    auto current = control.load(std::memory_order_acquire);

    // 只能从未锁定的 Running 状态进入 Finished。
    //
    // 如果 _link_predecessors 正持有 LOCKED，CAS 会失败并把实际控制值
    // 写回 current；下一轮清除 expected 中的 LOCKED 后继续等待解锁。
    for (;;) {
        current &= ~Topology::Control::LOCKED;

        TFL_ASSERT(Topology::Control::status(current) == Topology::Control::Status::Running);

        const auto finished = Topology::Control::set_status(current, Topology::Control::Status::Finished);

        if (control.compare_exchange_weak(current,
                                          finished,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
            break;
        }
    }

    // Finished 已通过成功 CAS 的 release 部分发布；此后唤醒等待当前 Topology 的线程。
    control.notify_all();

    // 成功发布 Finished 后，_link_predecessors() 会直接把该前驱视为已完成，
    // 因而不会再向当前 Work 追加动态后继；此刻动态后继前缀已经冻结。
    const std::size_t num_successors = w.m_num_successors;

    for (std::size_t i = 0; i < num_successors; ++i) {
        Work* const successor = w.m_edges[i];

        if (successor->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            Executor* const executor = successor->m_topology->m_executor;

            if (executor == this) {
                if (cache) {
                    _schedule(wr, successor);
                } else {
                    cache = successor;
                }
            } else {
                executor->_schedule(successor);
            }
        }
    }

    // 释放本次执行持有的一份强引用；若没有外部 AsyncTask 句柄继续持有，
    // 当前线程可能成为最后一个引用释放者并立即销毁 Work。
    if (w._decrement_ref()) {
        destroy_work(std::addressof(w));
    }

    // 上一步可能已经销毁 w，因此从这里开始禁止再次访问 w，只使用提前缓存的 parent。
    if (parent) {
        _schedule_parent(parent, wr, cache);
    } else {
        _decrement_topology();
    }
}

inline void Executor::_push_shared(Work* val) {
    std::size_t const size = m_shared_buffers.size();
    std::size_t const b = detail::mulhi64(reinterpret_cast<std::uintptr_t>(val) * 11400714819323198485ULL, size);

    // 快路径：从哈希首选分片开始环形线性探测，优先选择当前可立即取得的互斥锁。
    for (std::size_t curr_b = b; curr_b < size; ++curr_b) {
        auto& buf = m_shared_buffers[curr_b];
        if (buf.mutex.try_lock()) {
            std::lock_guard lock{buf.mutex, std::adopt_lock};
            buf.queue.push(val);
            return;
        }
    }

    for (std::size_t curr_b = 0; curr_b < b; ++curr_b) {
        auto& buf = m_shared_buffers[curr_b];
        if (buf.mutex.try_lock()) {
            std::lock_guard lock{buf.mutex, std::adopt_lock};
            buf.queue.push(val);
            return;
        }
    }

    // 所有分片当前均被占用时，不再继续自旋，阻塞等待最初哈希得到的首选分片。
    std::lock_guard lock(m_shared_buffers[b].mutex);
    m_shared_buffers[b].queue.push(val);
}

template <std::random_access_iterator Iterator>
    requires std::convertible_to<std::iter_reference_t<Iterator>, Work*>
inline void Executor::_push_shared(Iterator first, std::size_t n) {
    TFL_ASSERT(n != 0);

    std::size_t const size = m_shared_buffers.size();
    std::size_t const b = detail::mulhi64(reinterpret_cast<std::uintptr_t>(*first) * 11400714819323198485ULL, size);

    // 快路径：从哈希首选分片开始环形线性探测，优先选择当前可立即取得的互斥锁。
    for (std::size_t curr_b = b; curr_b < size; ++curr_b) {
        auto& buf = m_shared_buffers[curr_b];

        if (buf.mutex.try_lock()) {
            std::lock_guard lock{buf.mutex, std::adopt_lock};
            buf.queue.push(first, n);
            return;
        }
    }

    for (std::size_t curr_b = 0; curr_b < b; ++curr_b) {
        auto& buf = m_shared_buffers[curr_b];

        if (buf.mutex.try_lock()) {
            std::lock_guard lock{buf.mutex, std::adopt_lock};
            buf.queue.push(first, n);
            return;
        }
    }

    // 所有分片当前均被占用时，阻塞等待首选分片并一次性完成批量推送。
    std::lock_guard lock(m_shared_buffers[b].mutex);
    m_shared_buffers[b].queue.push(first, n);
}

// ============================================================================
// 任务调度入口
// ============================================================================

template <std::random_access_iterator Iterator>
inline void Executor::_schedule(Worker& wr, Iterator first, std::size_t n) {
    // Worker 本地队列优先；无法继续容纳的尾部区间整体溢出到共享分片。
    wr.m_wslq.push(first, n, [&](Iterator remaining, std::size_t count) {
        _push_shared(remaining, count);
    });

    m_notifier.notify_n(n);
}

template <std::random_access_iterator Iterator>
inline void Executor::_schedule(Iterator first, std::size_t n) {
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

inline void Executor::_schedule_parent(Work* parent, Worker& wr, Work*& cache) {
    if (parent->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (parent->m_properties & Work::Properties::PREEMPTED) {
            if (cache) {
                _schedule(wr, cache);
            }
            cache = parent;
        }
    }
}

inline void Executor::_schedule_from_semaphore(Worker& wr, SmallVector<Work*>& waiters) {
    for (Work* work : waiters) {
        Executor* const executor = work->m_topology->m_executor;
        TFL_ASSERT(executor);
        if (executor == this) [[likely]] {
            _schedule(wr, work);
        } else {
            executor->_schedule(work);
        }
    }
}

// ============================================================================
// 协作式等待
// ============================================================================
template <predicate Pred>
inline void Executor::_corun_until(Worker& wr, Pred&& pred) {
    const std::size_t nw = m_workers.size();

    while (!std::invoke(pred)) {
        if (auto* w = wr.m_wslq.pop()) [[likely]] {
            _invoke(wr, w);
            continue;
        }

        std::size_t vtm = wr.m_vtm;
        std::uint32_t num_steals = 0;
        std::uint32_t num_yields = 0;

        while (!std::invoke(pred)) {
            Work* w = (vtm < nw) ? m_workers[vtm].m_wslq.steal() : m_shared_buffers[vtm - nw].queue.steal();

            if (w) [[likely]] {
                wr.m_vtm = vtm;
                _invoke(wr, w);
                break;
            }

            if (++num_steals > wr.m_max_steals) [[unlikely]] {
                std::this_thread::yield();

                if (++num_yields >= wr.m_max_yields) [[unlikely]] {
                    break;
                }
            }

            vtm = wr.m_rng();
        }
    }
}

inline void Executor::_corun_graph(Graph& g, Work& parent, Worker& wr) {
    const std::size_t num_sources = _set_up_graph(g, parent);

    if (num_sources == 0) {
        return;
    }

    parent.m_join_counter.fetch_add(num_sources, std::memory_order_relaxed);
    _schedule(wr, g.begin(), num_sources);

    _corun_until(wr, [&parent]() noexcept {
        return parent.m_join_counter.load(std::memory_order_acquire) == 0;
    });
}

// ============================================================================
// 拓扑计数管理
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
    auto itr = m_tid_to_worker.find(std::this_thread::get_id());
    return itr == m_tid_to_worker.end() ? nullptr : itr->second;
}

TFL_FORCE_INLINE void Executor::_invoke(Worker& wr, Work* w) {
    do {
        Work* cache{nullptr};
        w->invoke(wr, *this, cache);
        w = cache;
    } while (w);
}

} // namespace tfl
