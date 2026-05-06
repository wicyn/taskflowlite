/// @file worker.hpp
/// @brief 工作线程实体 Worker / 安全视图 WorkerView / 钩子接口 WorkerHandler
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <thread>

#include "forward.hpp"
#include "bounded_queue.hpp"
#include "random.hpp"
#include "notifier.hpp"

namespace tfl {

/// @brief 工作线程运行时容器 —— 1:1 映射到 1 个 OS 线程的执行单元。
///
/// @details
/// `Worker` 持有一个 OS 线程 + 该线程独占的所有调度上下文（本地队列、随机数
/// 引擎、终止标志、统计量）。`Executor` 在构造时 `_spawn` 创建固定数量的 Worker，
/// 此后这些 Worker 与底层线程在 Executor 生命周期内死锁绑定。
///
/// ============================================================================
///  内存布局 —— 减少伪共享（False Sharing）
/// ============================================================================
/// 字段顺序按"竞争性"分层：
///
/// | 区段                | 字段                          | 访问模式                  |
/// |---------------------|------------------------------|--------------------------|
/// | 头部（多线程竞争）   | `m_wslq`（BoundedQueue）     | owner LIFO + stealer FIFO |
/// | 中部（单线程独占）   | rng / vtm / adaptive_factor  | owner 线程读写             |
/// | 独立 cache line     | `m_terminate`                | shutdown 时跨线程写        |
/// | 尾部                | thread                       | 仅生命周期端点              |
///
/// `m_terminate` 显式 `alignas(2 * cache_line_size)` 隔离 —— 否则 owner 线程
/// 频繁修改本地队列字段会拖累 shutdown 路径的 atomic_flag 性能。
///
/// ============================================================================
///  核心字段语义
/// ============================================================================
/// - **`m_wslq`**：BoundedQueue<Work*> 本地任务队列。owner LIFO 操作（push/pop）
///   保证缓存温度，stealer 从另一端 FIFO 偷（粒度大、争用少）；
/// - **`m_rng`**：SplitMix64 引擎，独立种子（hash(thread_id)）—— 防多 worker
///   同时偷同一队列；
/// - **`m_vtm`**：上次成功窃取的目标队列索引 —— 局部性优化，下次先试这个；
/// - **`m_adaptive_factor`**：窃取失败次数的退避系数（1~8），动态调整 yield 频率；
/// - **`m_terminate`**：shutdown 信号位，relaxed 读 / relaxed 写。
///
/// ============================================================================
///  对外暴露
/// ============================================================================
/// `Worker` 的 public 接口非常薄 —— 只读查询（id / queue_size / capacity / thread）。
/// 写操作全部走 friend（Executor / Runtime / Work / WorkerView），这是分层访问
/// 控制：调度器有充分修改权，用户态只能读。
///
/// @see WorkerView         给 TaskObserver 用的安全切片
/// @see WorkerHandler      生命周期钩子
/// @see Executor::_spawn   Worker 的创建与启动
/// @see BoundedQueue       本地队列实现
class Worker : public Immovable<Worker> {
    friend class Executor;
    friend class Runtime;
    friend class WorkerView;
    friend class Work;

    TFL_WORK_SUBCLASS_FRIENDS;

public:
    /// @brief 获取 Worker 的唯一标识符
    /// @return 0-based 索引，与 Executor::m_workers 数组索引一致
    [[nodiscard]] std::size_t id() const noexcept { return m_id; }

    /// @brief 获取本地 Work-Stealing 队列的当前任务数
    [[nodiscard]] std::size_t queue_size() const noexcept { return m_wslq.size(); }

    /// @brief 获取本地队列的最大容量
    [[nodiscard]] std::size_t queue_capacity() const noexcept {
        return static_cast<size_t>(m_wslq.capacity());
    }

    /// @brief 获取底层绑定的系统线程对象
    [[nodiscard]] std::thread& thread() noexcept { return m_thread; }
    [[nodiscard]] const std::thread& thread() const noexcept { return m_thread; }

private:
    // 核心任务队列：Owner 线程 LIFO 存取，Stealer 线程 FIFO 窃取
    BoundedQueue<Work*, TFL_DEFAULT_QUEUE_SIZE> m_wslq;

    SplitMix64          m_rng;                     ///< 随机数生成器（每个 Worker 独立序列）
    std::size_t         m_vtm{0};                ///< 上次成功窃取的队列索引（局部性优化）
    std::uint32_t       m_adaptive_factor{4};  ///< 动态退避系数（窃取失败阈值调整）
    std::uint32_t       m_max_steals{0};       ///< 单轮最大窃取尝试次数
    std::size_t         m_id{0};                 ///< 全局唯一 ID

    // ---- 独立 cache line：跨线程终止信号 ----
    alignas(2 * cache_line_size)
        std::atomic_flag m_terminate = ATOMIC_FLAG_INIT;

    std::thread         m_thread;
};


/// @brief Worker 的只读安全视图 —— 给 TaskObserver / 用户态回调用。
///
/// @details
/// `WorkerView` 持有 `const Worker&`，**编译期** 屏蔽全部写操作。它出现的理由
/// 与 `TaskView` 完全平行：把"线程安全契约"提升到类型层 —— 用户在回调里拿到的
/// 是只读切片，写访问编译就过不了。
///
/// 暴露字段：id / queue_size / queue_capacity / thread。
///
/// 构造私有，仅 friend 可造（Work / Flow / Executor / Runtime + Work 子类）。
/// 这保证 WorkerView 实例只能由调度器内部产生再传给用户回调，用户拿不到
/// 任意构造它的能力。
///
/// @see Worker          本视图代理的对象
/// @see TaskObserver    主要消费者
/// @see TaskView        TaskObserver 的另一个安全参数
class WorkerView {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;
public:
    [[nodiscard]] std::size_t id() const noexcept { return m_worker.m_id; }
    [[nodiscard]] std::size_t queue_size() const noexcept { return m_worker.m_wslq.size(); }
    [[nodiscard]] std::size_t queue_capacity() const noexcept {
        return static_cast<size_t>(m_worker.m_wslq.capacity());
    }
    [[nodiscard]] const std::thread& thread() const noexcept { return m_worker.m_thread; }

private:
    explicit WorkerView(const Worker& wr) noexcept : m_worker{wr} {}
    explicit WorkerView(const WorkerView&) = default;

    const Worker& m_worker;
};


/// @brief 工作线程生命周期钩子 —— 策略模式注入点。
///
/// @details
/// `WorkerHandler` 把 Worker 线程的三个关键时刻暴露给用户：启动、停止、异常。
/// `Executor` 在构造时通过引用收纳一个 handler 实例，所有 worker 共享。
///
/// ============================================================================
///  三个钩子的语义
/// ============================================================================
/// - **`on_start(Worker&)`** ：线程刚创建、进入调度循环之前。
///   典型用途：CPU 亲和性绑定（`sched_setaffinity`）、线程重命名（`pthread_setname_np`）、
///   TLS 初始化、tracing 上下文挂载。
///
/// - **`on_stop(Worker&)`**  ：线程退出调度循环之前（终止 / shutdown 路径）。
///   典型用途：TLS 资源回收、累计指标输出、tracing 上下文卸载。
///
/// - **`on_exception(Worker&, exception_ptr)`** ：节点 invoke 抛未捕获异常时。
///   返回值决定线程命运 —— `true` 吞掉继续，`false` 终止本 worker。
///
/// 全部钩子标记 `noexcept` —— 用户实现自己 try-catch；钩子里再抛异常会
/// std::terminate。
///
/// ============================================================================
///  内置策略：ResumeAlways vs ResumeNever
/// ============================================================================
/// 框架给出两种典型实现，覆盖光谱两端：
///
/// | 策略           | 适用场景                                                 |
/// |----------------|---------------------------------------------------------|
/// | `ResumeAlways` | 业务任务，单点失败不应拖垮整体 —— 静默吞异常             |
/// | `ResumeNever`  | 数据处理 pipeline / 事务批处理 —— 任何错误立即停 worker |
///
/// 用户可继承 WorkerHandler 实现自定义策略（如"只允许 std::bad_alloc 通过"）。
///
/// @see Executor::Executor(WorkerHandler&)  注入点
/// @see Executor::_spawn   钩子调用位置
class WorkerHandler {
public:
    virtual ~WorkerHandler() = default;

    /// @brief 线程启动前触发
    /// @note 适合 CPU 亲和性绑定、线程重命名、TLS 初始化
    virtual void on_start(Worker& worker) noexcept = 0;

    /// @brief 线程退出前触发
    /// @note 适合 TLS 资源回收、统计指标输出
    virtual void on_stop(Worker& worker) noexcept = 0;

    /// @brief 发生未捕获异常时触发
    /// @return true: 异常已安抚，线程继续工作 | false: 线程终止
    virtual bool on_exception(Worker& worker, std::exception_ptr eptr) noexcept = 0;
};

/// @brief 容错策略：Resume Always
/// @details 单个任务失败不能拖垮整体系统，静默消费异常并继续调度
class ResumeAlways : public WorkerHandler {
public:
    void on_start(Worker&) noexcept override {}
    void on_stop(Worker&) noexcept override {}

    bool on_exception(Worker&, std::exception_ptr) noexcept override final {
        return true; // 强制吞掉异常，保全整体可用性
    }
};

/// @brief 严格策略：Resume Never
/// @details 任何异常都视为致命错误，停止 Worker 线程
class ResumeNever : public WorkerHandler {
public:
    void on_start(Worker&) noexcept override {}
    void on_stop(Worker&) noexcept override {}

    bool on_exception(Worker&, std::exception_ptr) noexcept override final {
        return false; // 强制终止线程
    }
};

}  // namespace tfl
