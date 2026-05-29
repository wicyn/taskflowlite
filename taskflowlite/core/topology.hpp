/// @file  topology.hpp
/// @brief 执行拓扑 Topology —— 任务图运行实例的生命周期与状态机。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <stop_token>
#include <cstddef>
#include <cstdint>

#include "forward.hpp"
#include "utility.hpp"

namespace tfl {

/// @brief 单次任务提交的运行时上下文 —— 承载状态机、引用计数与协作式停止。
///
/// 默认初始状态为 Idle，由调度器通过 CAS 推进至 Running，执行完毕迁移至 Finished。
/// Running 与 Locking 之间通过 CAS 自旋锁实现轻量级原子多字段更新（用于动态依赖插入）。
/// m_use_count 引用计数支撑 AsyncTask 强引用语义：节点在最后一份句柄释放后才回收内存。
///
/// @note m_stop_source 提供协作式停止（建议性取消），仅控制未发出任务的执行，已发出的不可逆。
class Topology : public Immovable<Topology> {

    friend class Work;
    friend class Task;
    friend class Runtime;
    friend class Executor;
    template <typename> friend class AsyncTask;

    TFL_WORK_SUBCLASS_FRIENDS;

public:
    /// @brief 构造 Topology 并绑定到 Executor。
    /// @param exec 所属 Executor。
    explicit Topology(Executor* exec) noexcept;

    ~Topology() = default;

private:
    /// @brief 生命周期状态枚举 —— 兼作轻量级自旋锁的锁字。
    ///
    /// Running 和 Locking 之间通过 CAS 切换，实现"执行中暂时锁定以添加新依赖"。
    enum class State : std::int32_t {
        Idle     = 0, ///< 初始状态，尚未启动
        Running  = 1, ///< 执行中，任务正在被调度
        Locking  = 2, ///< 锁定瞬态，动态添加任务时
        Finished = 3  ///< 执行完成
    };

    std::atomic<State>       m_state{State::Idle};            ///< 状态机 + 轻量级锁
    std::stop_source         m_stop_source{};                 ///< 停止源
    Executor*                m_executor{nullptr};             ///< 所属执行器
    std::atomic<std::size_t> m_use_count{0};                  ///< 引用计数

    /// @brief 阻塞等待拓扑完成。
    ///
    /// load(acquire) 获取当前状态；wait 用 OS 挂起线程；
    /// 调度器在状态切换为 Finished 时触发 notify_all 唤醒。
    ///
    /// @post m_state == State::Finished
    void _wait() const noexcept;

    /// @brief 引用计数递增。
    ///
    /// relaxed 序 —— 仅计数，不涉及跨线程数据同步。
    void _incref() noexcept;

    /// @brief 引用计数递减。
    ///
    /// @return true 表示计数归零，调用方可销毁本对象。
    ///
    /// fetch_sub(acq_rel): 确保归零方观察到所有持有者侧的写入，
    /// 防止在内存释放后仍有其他线程的延迟写入命中已释放区域。
    [[nodiscard]] bool _decref() noexcept;

    /// @brief 是否处于执行中（Running 或 Locking）。
    [[nodiscard]] bool _is_running() const noexcept;

    /// @brief 是否已完成。
    [[nodiscard]] bool _is_finished() const noexcept;

    /// @brief 获取当前引用计数。
    [[nodiscard]] std::size_t _use_count() const noexcept;
};

// ============================================================================
// Implementation
// ============================================================================

inline Topology::Topology(Executor* exec) noexcept
    : m_executor{exec} {}

/// @brief 阻塞等待拓扑完成（自旋 + OS wait 混合等待）。
///
/// 使用 std::atomic::wait 实现用户态高效等待: 状态未变时 OS 挂起线程，
/// 状态变更时调度器 notify_all 唤醒。避免了纯自旋的 CPU 浪费。
///
/// acquire 确保唤醒后观察到所有状态变更。
inline void Topology::_wait() const noexcept {
    auto state = m_state.load(std::memory_order_acquire);
    while (state != State::Finished) {
        m_state.wait(state, std::memory_order_acquire);
        state = m_state.load(std::memory_order_acquire);
    }
}

inline void Topology::_incref() noexcept {
    m_use_count.fetch_add(1, std::memory_order_relaxed);
}

/// @brief 引用计数递减。
///
/// @return true 表示归零，调用方获得独占所有权，可安全销毁。
///
/// fetch_sub(acq_rel): 归零方需要 acquire 所有持有者侧的数据写入
/// （尤其是 m_exception_ptr 的写入），防止 UAF。非归零方只需 release 自身写入。
inline bool Topology::_decref() noexcept {
    return m_use_count.fetch_sub(1, std::memory_order_acq_rel) == 1;
}

/// @brief 是否处于执行中状态 (Running 或 Locking)。
///
/// relaxed 足够 —— 仅作状态查询，不做跨线程同步。
inline bool Topology::_is_running() const noexcept {
    auto s = m_state.load(std::memory_order_relaxed);
    return s == State::Running || s == State::Locking;
}

/// @brief 是否已完成。relaxed 足够 —— 仅作状态查询。
inline bool Topology::_is_finished() const noexcept {
    return m_state.load(std::memory_order_relaxed) == State::Finished;
}

/// @brief 获取当前引用计数。relaxed 足够 —— 瞬时快照。
inline std::size_t Topology::_use_count() const noexcept {
    return m_use_count.load(std::memory_order_relaxed);
}

}  // namespace tfl
