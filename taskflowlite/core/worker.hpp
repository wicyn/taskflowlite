/// @file  worker.hpp
/// @brief 工作线程实体 Worker / 安全视图 WorkerView / 生命周期钩子 WorkerHandler。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <thread>

#include "forward.hpp"
#include "bounded_queue.hpp"
#include "random.hpp"
#include "notifier.hpp"

namespace tfl {

/// @brief 工作线程运行时容器 —— 1:1 映射到 OS 线程的执行单元。
///
/// 持有一个 OS 线程及其独占的调度上下文：BoundedQueue 本地任务队列、SplitMix64
/// 随机数引擎、窃取局部性优化字段。m_terminate 独立 cache line 对齐防伪共享。
/// public 接口仅提供只读查询，写操作全部走 friend 分层访问控制。
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
    alignas(2 * cache_line_size) std::atomic_flag m_terminate = ATOMIC_FLAG_INIT;

    std::thread         m_thread;
};


/// @brief Worker 的只读安全视图 —— 给 TaskObserver / 用户态回调用。
///
/// 持有 const Worker&，编译期屏蔽全部写操作，把线程安全契约提升到类型层。
/// 构造私有仅 friend 可造，保证用户只能通过调度器内部拿到此视图。
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
    /// @return const std::thread&，仅供查询（如 get_id()），不可 join/detach。
    [[nodiscard]] const std::thread& thread() const noexcept { return m_worker.m_thread; }

private:
    explicit WorkerView(const Worker& wr) noexcept : m_worker{wr} {}
    explicit WorkerView(const WorkerView&) = default;

    const Worker& m_worker;
};


/// @brief 工作线程生命周期钩子 —— 策略模式注入点。
///
/// 三个钩子：on_start（线程创建后）、on_stop（退出调度循环前）、on_exception
/// （节点抛异常时，返回 true 吞掉 / false 终止 worker）。全部 noexcept。
/// 框架内置 ResumeAlways（吞异常继续）和 ResumeNever（遇异常停止）两种策略。
///
class WorkerHandler {
public:
    virtual ~WorkerHandler() = default;

    /// @brief 线程创建后、调度循环启动前触发
    /// @note 适合 CPU 亲和性绑定、线程重命名、TLS 初始化
    virtual void on_start(Worker& worker) noexcept = 0;

    /// @brief 调度循环退出后、线程 join 前触发
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
        return true; // 消费异常，继续调度
    }
};

/// @brief 严格策略：Resume Never
/// @details 任何异常都视为致命错误，停止 Worker 线程
class ResumeNever : public WorkerHandler {
public:
    void on_start(Worker&) noexcept override {}
    void on_stop(Worker&) noexcept override {}

    bool on_exception(Worker&, std::exception_ptr) noexcept override final {
        return false; // 异常触发 worker 退出
    }
};

/// @brief 默认 worker handler —— 所有 hook 空实现，异常发生时让 worker 终止
using DefaultWorkerHandler = ResumeNever;

}  // namespace tfl
