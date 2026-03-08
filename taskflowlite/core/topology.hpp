/// @file topology.hpp
/// @brief 执行拓扑 - 任务图的生命周期管理与状态机
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include <atomic>
#include "forward.hpp"

namespace tfl {

/// @brief 任务图执行拓扑 - 生命周期与状态管理
///
/// @details
/// 每次向 Executor 提交任务图时创建，作为执行上下文。
/// 负责统筹整个任务图的生命周期、取消操作、内存引用计数。
///
/// @par 状态机转换
/// @code
///   Idle ──(start)──▶ Running ──(finish)──▶ Finished
///    ▲                   │
///    │                   │(动态添加依赖)
///    └───────────────────┘
///          Locking (瞬态)
/// @endcode
///
/// @par 线程安全
/// 所有状态操作均为原子操作。Running ↔ Locking 转换作为轻量级自旋锁，
/// 用于动态添加任务时的并发保护，无需 std::mutex。
class Topology {

    friend class Work;
    friend class Task;
    friend class AsyncTask;
    friend class Graph;
    friend class Executor;

    TFL_WORK_SUBCLASS_FRIENDS;

protected:
    /// @brief 生命周期状态枚举
    enum class State : std::int32_t {
        Idle     = 0, ///< 初始状态，尚未启动
        Running  = 1, ///< 执行中，任务正在被调度
        Locking  = 2, ///< 锁定状态，动态添加任务时的瞬态
        Finished = 3  ///< 执行完成
    };

    explicit Topology(Executor& exec) noexcept;
    ~Topology() = default;

private:
    // 热数据区：按访问频率降序排列
    std::atomic<State> m_state{State::Idle};  ///< 状态机 + 轻量级锁
    std::atomic_flag m_stopped = ATOMIC_FLAG_INIT; ///< 停止标志

    Work* m_work{nullptr};     ///< 入口任务节点
    Executor& m_executor;      ///< 所属调度器
    std::atomic<std::size_t> m_use_count{0}; ///< 引用计数

    // 状态查询与控制
    void _wait() const noexcept;
    void _stop() noexcept;
    void _incref() noexcept;
    bool _decref() noexcept;
    [[nodiscard]] bool _is_stopped() const noexcept;
    [[nodiscard]] bool _is_running() const noexcept;
    [[nodiscard]] bool _is_finished() const noexcept;
};

// ============================================================================
// Implementation
// ============================================================================

inline Topology::Topology(Executor& exec) noexcept
    : m_executor{exec} {}

/// @brief 阻塞等待拓扑完成
///
/// @memory_order
/// - load(acquire): 获取当前状态
/// - wait: 原子等待，OS 挂起
/// - notify_all: 由调度器触发
inline void Topology::_wait() const noexcept {
    auto state = m_state.load(std::memory_order_acquire);
    while (state != State::Finished) {
        m_state.wait(state, std::memory_order_acquire);
        state = m_state.load(std::memory_order_acquire);
    }
}

/// @brief 请求停止执行
///
/// @performance: lock-free 原子操作，立即生效
inline void Topology::_stop() noexcept {
    m_stopped.test_and_set(std::memory_order_relaxed);
}

/// @brief 引用计数递增
///
/// @memory_order: relaxed - 仅计数，不涉及跨线程数据同步
inline void Topology::_incref() noexcept {
    m_use_count.fetch_add(1, std::memory_order_relaxed);
}

/// @brief 引用计数递减
/// @return true: 计数归零，可销毁
///
/// @memory_order
/// - fetch_sub(acq_rel): 确保其他线程的所有操作对当前线程可见
///   防止在内存释放后仍有其他线程的延迟写入
inline bool Topology::_decref() noexcept {
    return m_use_count.fetch_sub(1, std::memory_order_acq_rel) == 1;
}

inline bool Topology::_is_stopped() const noexcept {
    return m_stopped.test(std::memory_order_relaxed);
}

inline bool Topology::_is_running() const noexcept {
    auto s = m_state.load(std::memory_order_relaxed);
    return s == State::Running || s == State::Locking;
}

inline bool Topology::_is_finished() const noexcept {
    return m_state.load(std::memory_order_relaxed) == State::Finished;
}

}  // namespace tfl
