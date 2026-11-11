/// @file worker.hpp
/// @brief 工作线程实体 Worker、只读视图 WorkerView 和生命周期钩子 WorkerHandler。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "macros.hpp"
#include "forward.hpp"
#include "bounded_queue.hpp"
#include "stack.hpp"
#include "random.hpp"
#include "utility.hpp"

namespace tfl {

/// @brief 表示 Executor 管理的一条工作线程及其线程本地调度资源。
///
/// 每个 Worker 保存系统线程、本地工作窃取队列、随机状态和空闲 Stacklet 缓存。
/// 对象由 Executor 创建和销毁，不支持独立复制或移动。
/// Owner 操作由所属线程执行，其他线程只窃取队列任务。
///
/// 每个 Coro 独占活动栈段链，用于分配该执行链上的 Coroutine Frame。
/// 根 Frame 首次分配或 Coro 栈段空间不足时，优先向当前 Worker 的 m_stack 获取空闲 Stacklet；
/// Worker 没有合适的空闲栈段时创建新段。
/// Coro 的活动栈段在挂起或跨 Worker 恢复期间始终由 Coro 持有；
/// 扩展栈段退空时立即归还；根栈段在本轮执行收尾时归还当前 Worker。
/// 默认大小栈段进入缓存，其他大小的栈段立即释放。
///
/// @warning m_stack 是空闲栈段缓存，不允许存放仍存活的 Coroutine Frame。
/// @warning m_stack 只能由当前 Worker 线程访问，不允许并发操作。
/// @warning 跨 Worker 恢复前必须完成 Coro 的执行权交接和同步。
class Worker : public Immovable<Worker> {
    friend class Executor;
    friend class Runtime;
    friend class SubFlow;
    friend class WorkerView;
    friend class Work;
    friend class Coro;

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

    /// @brief 当前 Worker 的空闲 Stacklet 缓存。
    ///
    /// 默认创建一个 4096 字节的空 Stacklet；后续从 Coro 回收空栈段。
    /// Coro 分配空间不足时调用 acquire(size) 获取合适的栈段，
    /// 如果缓存中没有匹配的栈段则自动分配新块。
    /// Coro 的活动 Frame 始终位于其独占的栈段链中，不会存放在这里。
    ///
    /// @warning 仅允许当前 Worker 线程操作本成员。
    Stack m_stack;

    SplitMix64      m_rng;              ///< 随机数生成器（每个 Worker 独立序列）。
    std::uint32_t   m_vtm{0};           ///< 上次成功窃取的队列索引。
    std::uint32_t   m_max_steals{0};    ///< 进入 yield 阶段前允许连续执行的窃取次数。
    std::uint32_t   m_max_yields{0};    ///< 进入阻塞等待前允许连续执行的 yield 窃取次数。
    std::uint32_t   m_id{0};            ///< 全局唯一 ID。

    /// @brief 跨线程终止信号。
    ///
    /// 独立缓存行对齐，以降低与 Worker 本地热数据之间的伪共享概率。
    alignas(TFL_CACHE_LINE_SIZE) std::atomic_flag m_terminate = ATOMIC_FLAG_INIT;

    std::thread m_thread;
};

/// @brief 提供对一个既有 Worker 的非拥有只读观察接口。
///
/// 视图保存 const Worker&，仅暴露标识、线程和队列快照，不允许控制线程或修改
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
    [[nodiscard]] std::size_t id() const noexcept {
        return m_worker.m_id;
    }

    /// @brief 获取本地 Work-Stealing 队列的当前任务数。
    [[nodiscard]] std::size_t queue_size() const noexcept {
        return m_worker.m_wslq.size();
    }

    /// @brief 获取本地 BoundedQueue 的最大容量。
    [[nodiscard]] std::size_t queue_capacity() const noexcept {
        return static_cast<std::size_t>(m_worker.m_wslq.capacity());
    }

    /// @brief 获取底层绑定的 OS 线程对象的只读引用。
    [[nodiscard]] const std::thread& thread() const noexcept {
        return m_worker.m_thread;
    }

private:
    /// @brief 由框架为指定 Worker 构造只读视图。
    explicit WorkerView(const Worker& wr) noexcept : m_worker{wr} {}

    /// @brief 复制视图；副本继续借用同一个 Worker。
    explicit WorkerView(const WorkerView&) = default;

    const Worker& m_worker;
};

/// @brief 定义 Worker 线程生命周期处理接口。
///
/// Executor 在对应 Worker 线程上调用启动和停止钩子。
/// 通过构造函数传入的 WorkerHandler 始终由调用方拥有，
/// Executor 仅在自身生命周期内借用。
///
/// 普通任务 callable 抛出的异常由 Work 执行路径捕获，
/// 不会通过 Worker 生命周期接口传播。
///
/// @warning 处理器必须比 Executor 及全部 Worker 线程存活更久。
/// @warning 同一处理器可能被多个 Worker 并发调用。
/// @warning 所有生命周期钩子均为 noexcept。
class WorkerHandler {
public:
    /// @brief 虚析构函数，确保通过基类正确销毁派生处理器。
    virtual ~WorkerHandler() noexcept = default;

    /// @brief Worker 线程启动后、进入调度循环前触发。
    /// @param worker 当前启动的 Worker。
    virtual void on_start(Worker& worker) noexcept = 0;

    /// @brief Worker 结束调度循环后、线程函数返回前触发。
    virtual void on_stop(Worker& worker) noexcept = 0;

protected:
    /// @brief 允许派生类型构造基类部分。
    WorkerHandler() = default;

    /// @brief 允许派生类型复制基类部分。
    WorkerHandler(const WorkerHandler&) = default;

    /// @brief 允许派生类型移动基类部分。
    WorkerHandler(WorkerHandler&&) = default;

    /// @brief 允许派生类型复制赋值基类部分。
    WorkerHandler& operator=(const WorkerHandler&) & = default;

    /// @brief 允许派生类型移动赋值基类部分。
    WorkerHandler& operator=(WorkerHandler&&) & = default;
};

} // namespace tfl
