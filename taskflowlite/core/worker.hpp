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

/// @brief 定义 Worker 线程生命周期和未处理异常策略的扩展接口。
///
/// `Executor` 在对应 Worker 线程上调用启动、停止和异常钩子。通过构造函数
/// 传入的 WorkerHandler 始终由调用方拥有，Executor 仅在自身生命周期内借用。
///
/// @warning 传入 Executor 的处理器必须比 Executor 及其全部 Worker 线程存活更久。
/// @warning 同一处理器实例可能服务多个 Worker，派生类必须自行同步共享状态；
///          所有钩子均不得抛出异常。
class WorkerHandler {
public:
    virtual ~WorkerHandler() = default;

    /// @brief 在线程创建后、进入调度循环前触发。
    /// @param worker 即将在当前 OS 线程上运行的 Worker。
    /// @note 适合设置 CPU 亲和性、线程名称和线程局部状态。
    virtual void on_start(Worker& worker) noexcept = 0;

    /// @brief 在调度循环退出后、线程函数返回前触发。
    /// @param worker 即将停止的 Worker。
    /// @note 适合清理线程局部状态和输出统计信息。
    virtual void on_stop(Worker& worker) noexcept = 0;

    /// @brief 处理未进入任务结果通道的调度路径异常。
    /// @param worker 捕获异常的 Worker。
    /// @param eptr 当前异常指针。
    /// @return true 表示异常已处理并继续调度；false 表示终止该 Worker 循环。
    /// @warning 本函数为 noexcept；派生实现抛出异常会导致程序终止。
    virtual bool on_exception(Worker& worker, std::exception_ptr eptr) noexcept = 0;
};

/// @brief 将未进入任务结果通道的异常视为已处理并继续当前 Worker 调度。
///
/// 启动和停止钩子为空操作；该策略不会记录、重抛或终止异常。
class ResumeAlways : public WorkerHandler {
public:
    /// @brief Worker 启动时不执行附加操作。
    void on_start(Worker&) noexcept override {}

    /// @brief Worker 停止时不执行附加操作。
    void on_stop(Worker&) noexcept override {}

    /// @brief 将未处理异常视为已处理。
    /// @return 恒为 true，使 Worker 继续调度。
    bool on_exception(Worker&, std::exception_ptr) noexcept override final {
        return true; // 消费异常，继续调度
    }
};

/// @brief 在出现未进入任务结果通道的异常时终止当前 Worker 调度循环。
///
/// 启动和停止钩子为空操作；该策略只影响发生异常的 Worker，不负责保存异常结果。
class ResumeNever : public WorkerHandler {
public:
    /// @brief Worker 启动时不执行附加操作。
    void on_start(Worker&) noexcept override {}

    /// @brief Worker 停止时不执行附加操作。
    void on_stop(Worker&) noexcept override {}

    /// @brief 将未处理异常视为终止信号。
    /// @return 恒为 false，使当前 Worker 退出调度循环。
    bool on_exception(Worker&, std::exception_ptr) noexcept override final {
        return false; // 异常触发 worker 退出
    }
};

}  // namespace tfl
