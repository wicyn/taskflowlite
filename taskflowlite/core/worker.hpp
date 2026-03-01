#pragma once

#include <thread>

#include "forward.hpp"
#include "bounded_queue.hpp"
#include "random.hpp"
#include "notifier.hpp"
namespace tfl {
class Worker {

    friend class Executor;
    friend class Runtime;
    friend class WorkerView;
    friend class Work;

public:

    [[nodiscard]] std::size_t id() const noexcept {
        return m_id;
    }

    [[nodiscard]] std::size_t queue_size() const noexcept {
        return m_wslq.size();
    }

    [[nodiscard]] std::size_t queue_capacity() const noexcept {
        return static_cast<size_t>(m_wslq.capacity());
    }

    [[nodiscard]] std::thread& thread() noexcept {
        return m_thread;
    }

    [[nodiscard]] const std::thread& thread() const noexcept {
        return m_thread;
    }

private:

    // ---- 多线程竞争：work-stealing queue ----
    BoundedQueue<Work*, TFL_DEFAULT_QUEUE_SIZE> m_wslq;

    // ---- owner-only 热数据：steal 循环每次迭代都碰 ----
    Xoshiro m_rng;                      // 随机选 victim
    uniform_uint64_distribution m_dist; // 配合 m_rng
    std::size_t m_vtm;                  // 当前 victim
    std::uint32_t m_adaptive_factor;    // 自适应退避
    std::uint32_t m_max_steals;         // steal 上限
    std::size_t m_id;                   // 比较用，只读
    std::atomic_flag m_terminate = ATOMIC_FLAG_INIT;
    std::thread m_thread;
};


class WorkerView {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Runtime;

    // ---- 子类友元 ----
    TFL_WORK_SUBCLASS_FRIENDS;

public:
    [[nodiscard]] std::size_t id() const noexcept {
        return m_worker.m_id;
    }

    [[nodiscard]] std::size_t queue_size() const noexcept {
        return m_worker.m_wslq.size();
    }

    [[nodiscard]] std::size_t queue_capacity() const noexcept {
        return static_cast<size_t>(m_worker.m_wslq.capacity());
    }

    [[nodiscard]] const std::thread& thread() const noexcept {
        return m_worker.m_thread;
    }
private:

    inline explicit WorkerView(const Worker& wr) noexcept : m_worker{wr} {}

    inline explicit WorkerView(const WorkerView&) = default;

    const Worker& m_worker;

};


/**
 * @brief 工作线程生命周期钩子
 *
 * 允许用户自定义 worker 线程的行为，包括：
 * - 线程启动/停止时的初始化/清理
 * - 异常处理策略
 * - CPU 亲和性绑定等
 */
class WorkerHandler {
public:
    virtual ~WorkerHandler() = default;

    /**
     * @brief worker 线程启动后、进入调度循环前调用
     * @param worker 当前 worker 引用
     *
     * 适合用于：
     * - 设置线程名称
     * - 绑定 CPU 亲和性
     * - 初始化线程局部资源
     */
    virtual void on_start(Worker& worker) noexcept = 0;

    /**
     * @brief worker 线程退出调度循环后调用
     * @param worker 当前 worker 引用
     *
     * 适合用于：
     * - 清理线程局部资源
     * - 记录统计信息
     */
    virtual void on_stop(Worker& worker) noexcept = 0;

    /**
     * @brief worker 调度循环中捕获到未处理异常时调用
     * @param worker 当前 worker 引用
     * @param eptr 异常指针
     * @return true 表示异常已处理，继续运行；false 表示终止该 worker
     *
     */
    virtual bool on_exception(Worker& worker, std::exception_ptr eptr) noexcept = 0;

};


// =========================================================
// 策略 1: ResumeAlways (永远恢复)
// 组合特性：静默处理 + 自动恢复 + 允许定制生命周期
// =========================================================
class ResumeAlways : public WorkerHandler {
public:
    // 默认实现为空，允许派生类重写
    void on_start(Worker& worker) noexcept override {}
    void on_stop(Worker& worker) noexcept override {}

    /**
     * @brief 核心策略：捕获异常 -> 记录警告 -> 强制返回 true
     */
    bool on_exception(Worker& worker, std::exception_ptr eptr) noexcept override final {
        return true;
    }
};

// =========================================================
// 策略 2: ResumeNever (绝不恢复)
// 组合特性：严格报错 + 立即终止 + 允许定制生命周期
// =========================================================
class ResumeNever : public WorkerHandler {
public:
    void on_start(Worker& worker) noexcept override {}
    void on_stop(Worker& worker) noexcept override {}

    /**
     * @brief 核心策略：捕获异常 -> 记录致命错误 -> 强制返回 false
     */
    bool on_exception(Worker& worker, std::exception_ptr eptr) noexcept override final {
        return false;
    }
};
}  // end of namespact tfl ------------------------------------------------------


