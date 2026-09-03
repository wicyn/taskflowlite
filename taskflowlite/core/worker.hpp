/// @file worker.hpp
/// @brief 工作线程实体 Worker、只读视图 WorkerView 和生命周期钩子 WorkerHandler。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cstddef>
#include <new>
#include <thread>

#include "macros.hpp"
#include "forward.hpp"
#include "bounded_queue.hpp"
#include "random.hpp"

namespace tfl {

/// @brief 表示 Executor 管理的一条工作线程及其线程本地调度资源。
///
/// 每个 Worker 保存系统线程、本地工作窃取队列、随机状态和可选内存池；对象由
/// `Executor` 创建和销毁，不支持独立复制或移动。Owner 操作由所属线程执行，其他线程只窃取队列任务。
///
/// @warning 启用本地池时，分配、释放和池重置必须在所属 Worker 线程按同一路径完成。
class Worker : public Immovable<Worker> {
    friend class Executor;
    friend class Runtime;
    friend class SubFlow;
    friend class WorkerView;
    friend class Work;

    TFL_WORK_SUBCLASS_FRIENDS;

public:
    /// @brief 获取 Worker 的唯一标识符。
    /// @return 0-based 索引，与 Executor::m_workers 数组索引一致。
    [[nodiscard]] std::size_t id() const noexcept {
        return m_id;
    }

    /// @brief 获取本地 Work-Stealing 队列任务数的近似快照。
    /// @note 并发窃取或推入后结果可能立即过时。
    [[nodiscard]] std::size_t queue_size() const noexcept {
        return m_wslq.size();
    }

    /// @brief 获取本地队列的最大容量。
    [[nodiscard]] std::size_t queue_capacity() const noexcept {
        return static_cast<std::size_t>(m_wslq.capacity());
    }

    /// @brief 获取底层绑定的系统线程对象。
    /// @warning Executor 拥有该线程；调用方不得 join、silent_async、移动或替换它。
    [[nodiscard]] std::thread& thread() noexcept {
        return m_thread;
    }

    /// @brief 获取底层绑定的系统线程对象。
    /// @warning Executor 拥有该线程；调用方不得 join、silent_async、移动或替换它。
    [[nodiscard]] const std::thread& thread() const noexcept {
        return m_thread;
    }

private:

    /// @brief Worker 本地任务队列。
    /// @note Owner 按 LIFO 顺序访问，Stealer 按 FIFO 顺序窃取。
    BoundedQueue<Work*, TFL_DEFAULT_QUEUE_SIZE> m_wslq;

    SplitMix64    m_rng;                ///< 随机数生成器（每个 Worker 独立序列）。
    std::size_t   m_vtm{0};             ///< 上次成功窃取的队列索引。
    std::uint32_t m_adaptive_factor{4}; ///< 动态退避系数（窃取失败阈值调整）。
    std::uint32_t m_max_steals{0};      ///< 单轮最大窃取尝试次数。
    std::size_t   m_id{0};              ///< 全局唯一 ID。

    /// @brief 跨线程终止信号。
    ///
    /// 独立缓存行对齐，以降低与 Worker 本地热数据之间的伪共享概率。
    alignas(2 * cache_line_size) std::atomic_flag m_terminate = ATOMIC_FLAG_INIT;

    std::thread m_thread;
};

/// @brief 提供对一个既有 `Worker` 的非拥有只读观察接口。
///
/// 视图保存 `const Worker&`，仅暴露标识、线程和队列快照，不允许控制线程或修改
/// 调度状态；复制视图不会延长 Worker 的生命周期。
///
/// @note 队列相关查询是瞬时近似值，可能因并发调度立即过时。
class WorkerView {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;
public:
    /// @brief 获取该 Worker 的唯一标识符。
    /// @return 0-based 索引，与 Executor::m_workers 数组索引一致。
    [[nodiscard]] std::size_t id() const noexcept { return m_worker.m_id; }
    /// @brief 获取本地 Work-Stealing 队列的当前任务数（快照，可能瞬时过时）。
    /// @return 队列中等待执行的 task 近似数量。
    [[nodiscard]] std::size_t queue_size() const noexcept { return m_worker.m_wslq.size(); }
    /// @brief 获取本地 BoundedQueue 的最大容量。
    /// @return 队列满容量，由模板参数 TFL_DEFAULT_QUEUE_SIZE 决定。
    [[nodiscard]] std::size_t queue_capacity() const noexcept {
        return static_cast<size_t>(m_worker.m_wslq.capacity());
    }
    /// @brief 获取底层绑定的 OS 线程对象的只读引用。
    /// @return const std::thread&，仅供查询（如 get_id()），不可 join/silent_async。
    [[nodiscard]] const std::thread& thread() const noexcept { return m_worker.m_thread; }

private:
    /// @brief 由框架为指定 Worker 构造只读视图。
    /// @param wr 被借用的 Worker，必须比视图存活更久。
    explicit WorkerView(const Worker& wr) noexcept : m_worker{wr} {}

    /// @brief 复制视图；副本继续借用同一个 Worker。
    explicit WorkerView(const WorkerView&) = default;

    const Worker& m_worker;
};

/// @brief 定义 Worker 线程生命周期处理接口。
///
/// `Executor` 在对应 Worker 线程上调用启动和停止钩子。通过构造函数传入的
/// `WorkerHandler` 始终由调用方拥有，Executor 仅在自身生命周期内借用。
///
/// `on_start` 在线程进入调度循环前调用；`on_stop` 在线程离开调度循环后调用。
/// Worker 正常停止时 `on_stop` 接收到空异常指针；若调度路径存在未处理异常导致
/// Worker 退出，则接收到对应的 `std::exception_ptr`。
///
/// 普通任务 callable 抛出的异常由 Work 自身捕获、通知和归档，不会作为
/// Worker 停止异常进入本接口。
///
/// @warning 传入 Executor 的处理器必须比 Executor 及其全部 Worker 线程存活更久。
/// @warning 同一处理器实例可能被多个 Worker 并发调用，派生类必须自行同步共享状态。
/// @warning 所有生命周期钩子均不得抛出异常。
class WorkerHandler {
public:
    /// @brief 虚析构函数，确保通过基类正确销毁派生处理器。
    virtual ~WorkerHandler() = default;

    /// @brief Worker 线程启动后、进入调度循环前触发。
    ///
    /// 回调运行在当前 Worker 所属的 OS 线程上，可用于设置线程名称、
    /// CPU 亲和性、线程优先级以及初始化线程局部资源。
    ///
    /// @param worker 当前启动的 Worker。
    /// @warning 派生实现不得抛出异常。
    virtual void on_start(Worker& worker) noexcept = 0;

    /// @brief Worker 离开调度循环后、线程函数返回前触发。
    ///
    /// 正常停止时 @p exception 为空；若 Worker 因未处理的调度路径异常退出，
    /// 则保存导致本次退出的异常。该回调只负责处理停止事件，不影响 Worker
    /// 已经确定的退出行为。
    ///
    /// @param worker 当前即将停止的 Worker。
    /// @param exception 导致 Worker 停止的异常；正常停止时为空。
    /// @warning 派生实现不得抛出异常。
    virtual void on_stop(Worker& worker, const std::exception_ptr& exception) noexcept = 0;

protected:
    /// @brief 允许派生类型构造基类部分。
    WorkerHandler() = default;

    /// @brief 允许派生类型复制基类部分。
    WorkerHandler(const WorkerHandler&) = default;

    /// @brief 允许派生类型移动基类部分。
    WorkerHandler(WorkerHandler&&) = default;

    /// @brief 允许派生类型复制赋值基类部分。
    /// @return `*this`。
    WorkerHandler& operator=(const WorkerHandler&) & = default;

    /// @brief 允许派生类型移动赋值基类部分。
    /// @return `*this`。
    WorkerHandler& operator=(WorkerHandler&&) & = default;
};

}  // namespace tfl
