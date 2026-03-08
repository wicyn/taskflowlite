/// @file semaphore.hpp
/// @brief 提供任务级别的信号量同步原语，用于限制特定任务组的并发执行数量。
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include <cstddef>
#include <vector>
#include <algorithm>
#include <cassert>
#include <mutex>
#include "exception.hpp"

namespace tfl {

/// @brief 任务级并发控制信号量。
///
/// 与传统的 OS 级信号量不同，本组件专为异步任务图设计。
/// 当任务尝试获取信号量失败时，不会导致底层 Worker 线程阻塞，
/// 而是将该任务挂起推入内部的等待队列中，释放 Worker 线程去执行其他就绪任务。
class Semaphore {
    friend class Work;
    friend class Executor;
    friend class Task;

public:
    /// @brief 构造函数，初始化信号量的最大并发容量。
    /// @param max_value 允许的最大并发数量。初始可用计数也等同于此值。
    Semaphore(std::size_t max_value) noexcept
        : m_max_value{max_value}
        , m_value{max_value} {}

    /// @brief 构造函数，分别指定最大容量与当前初始可用容量。
    /// @param max_value 允许的最大并发数量。
    /// @param current_value 初始时刻的可用计数。
    /// @note 内部会自动将初始可用计数裁剪至不超过最大容量。
    Semaphore(std::size_t max_value, std::size_t current_value) noexcept
        : m_max_value{max_value}
        , m_value{(std::min)(current_value, max_value)} {}

    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    /// @brief 线程安全地获取当前剩余可用计数。
    [[nodiscard]] std::size_t value() const noexcept {
        std::lock_guard lk{m_lock};
        return m_value;
    }

    /// @brief 获取信号量的最大设计容量。
    [[nodiscard]] std::size_t max_value() const noexcept {
        return m_max_value;
    }

    /// @brief 重置信号量的容量并完全恢复可用计数。
    /// @param max_value 新的最大并发数量。
    /// @pre 必须确保当前没有任何任务在此信号量上处于等待挂起状态。
    /// @exception Exception 如果内部等待队列非空，抛出异常。
    void reset(std::size_t max_value) {
        std::lock_guard lk{m_lock};
        if (!m_waiters.empty()) {
            throw Exception("cannot reset while waiters exist.");
        }
        m_max_value = max_value;
        m_value = m_max_value;
    }

    /// @brief 重置信号量容量，并显式指定当前可用计数。
    /// @param max_value 新的最大并发数量。
    /// @param current_value 新的可用计数。
    /// @pre 必须确保当前没有任何任务在此信号量上处于等待挂起状态。
    /// @exception Exception 如果内部等待队列非空，抛出异常。
    void reset(std::size_t max_value, std::size_t current_value) {
        std::lock_guard lk{m_lock};
        if (!m_waiters.empty()) {
            throw Exception("cannot reset while waiters exist.");
        }
        m_max_value = max_value;
        m_value = (std::min)(current_value, max_value);
    }

private:
    mutable std::mutex m_lock;       ///< 保护内部状态的互斥锁
    std::size_t m_max_value{0};      ///< 允许的最大并发边界
    std::size_t m_value{0};          ///< 运行时动态追踪的当前可用授权数
    std::vector<Work*> m_waiters;    ///< 因获取失败而被迫挂起的拦截任务队列

    /// @brief 框架内部调用：尝试非阻塞地获取一个授权配额。
    /// @param w 发起尝试的任务节点指针。
    /// @return 成功扣减计数返回 true；否则自动将该任务压入等待队列并返回 false。
    [[nodiscard]] bool _try_acquire(Work* w) {
        std::lock_guard lk{m_lock};

        if (m_value > 0) {
            --m_value;
            return true;
        }

        // Why: 当配额耗尽时，直接将任务记录在案，随后返回 false。
        // 这指导底层的 Worker 线程立即放弃此任务并投身于窃取网络，彻底杜绝了并发死锁与 CPU 空转。
        m_waiters.push_back(w);
        return false;
    }

    /// @brief 框架内部调用：释放一个授权配额并唤醒所有等待者。
    /// @param on_wake 唤醒回调闭包，由调用方负责将解封的任务重新排入调度引擎。
    template <typename F>
        requires std::invocable<F&, Work*>
    void _release(F&& on_wake) {
        std::vector<Work*> batch;
        {
            std::lock_guard lk{m_lock};
            if (m_value < m_max_value) {
                ++m_value;
                // Why: 使用 std::vector::swap 是一种经典的锁粒度优化（"惊群"批处理）。
                // 一次性将挂起的队列移交到局部栈上，使得互斥锁能够被极速释放，防止因唤醒过程过长拖累并发度。
                batch.swap(m_waiters);
            }
        }

        // Why: 临界区外执行回调机制，将所有积压的饥饿任务重新推回调度池。
        // 注意：这会导致重新竞争（Re-competition），唯有最先被调度的那个能拿到刚刚释放的名额，其余的将再次失败并挂起。
        for (auto* w : batch) {
            std::invoke(on_wake, w);
        }
    }
};

}  // namespace tfl
