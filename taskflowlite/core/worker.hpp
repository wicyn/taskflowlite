/// @file worker.hpp
/// @brief 工作线程运行时状态容器 - Work-Stealing 调度器的执行单元
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include <thread>

#include "forward.hpp"
#include "bounded_queue.hpp"
#include "random.hpp"
#include "notifier.hpp"

namespace tfl {

/// @brief 工作线程运行时状态容器
///
/// @details
/// 每个 Worker 对象对应一个底层 OS 线程，持有 Work-Stealing 调度所需的全部上下文数据。
/// Worker 由 Executor 统一拥有和初始化，在调度器生命周期内与底层线程绑定。
///
/// @par 内存布局设计
/// 多线程竞争的队列（m_wslq）位于头部，紧随其后的是单线程独占的热点数据。
/// 此布局最大化缓存命中率并减少伪共享（False Sharing）。
class Worker {
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

    Xoshiro m_rng;                        ///< 随机数生成器（每个 Worker 独立序列）
    uniform_uint64_distribution m_dist;   ///< 均匀分布器（选择 victim 队列）

    std::size_t m_vtm{0};                ///< 上次成功窃取的队列索引（局部性优化）
    std::uint32_t m_adaptive_factor{4};  ///< 动态退避系数（窃取失败阈值调整）
    std::uint32_t m_max_steals{0};       ///< 单轮最大窃取尝试次数
    std::size_t m_id{0};                 ///< 全局唯一 ID

    std::atomic_flag m_terminate = ATOMIC_FLAG_INIT; ///< 一次性终止信号
    std::thread m_thread;                           ///< OS 线程句柄
};

/// @brief Worker 的只读安全视图代理
///
/// @details
/// 专为 TaskObserver 设计。通过向用户空间传递 WorkerView 而非 Worker 引用，
/// 从编译器层面彻底杜绝用户态代码篡改内部调度状态。
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
    inline explicit WorkerView(const Worker& wr) noexcept : m_worker{wr} {}
    inline explicit WorkerView(const WorkerView&) = default;

    const Worker& m_worker;
};

/// @brief 工作线程生命周期钩子（策略模式接口）
///
/// @details
/// 允许用户在三个关键生命周期节点注入自定义逻辑：
/// - on_start: 线程启动后、进入调度循环前
/// - on_stop: 线程退出调度循环前
/// - on_exception: 发生未捕获异常时
///
/// 所有回调标记为 noexcept，用户扩展代码必须自行处理异常。
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
