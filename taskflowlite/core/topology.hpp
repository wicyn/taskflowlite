/// @file topology.hpp
/// @brief 执行拓扑 Topology —— 任务图运行实例的生命周期与状态机
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <stop_token>
#include <cstddef>
#include <cstdint>

#include "forward.hpp"
#include "utility.hpp"

namespace tfl {

/// @brief 任务图执行拓扑 —— 一次"提交→运行→完成"的运行时容器。
///
/// @details
/// 每次向 Executor 提交任务图（Flow 或动态任务）都会创建一个 `Topology`：
/// - `Flow` 是 **静态结构**（节点 + 边），可以反复提交；
/// - `Topology` 是 **动态实例**（状态 / 计数 / 引用），每次提交一份新的。
///
/// 把这两件事拆开是设计上的关键决断 —— 否则同一 Flow 不能并发跑两次。
///
/// ============================================================================
///  状态机：Idle → Running → Finished
/// ============================================================================
/// @code
///                 (start)              (finish)
///   Idle ─────────────────────► Running ─────────────► Finished
///    ▲                            │
///    │                            │ (动态添加依赖时短暂 Locking)
///    └──────  Locking 自旋 ───────┘
/// @endcode
///
/// **Running ↔ Locking** 是巧妙之处：动态 `dependent_async` 添加新依赖时需要
/// 临时锁定状态做原子性的多字段更新。直接用 `std::mutex` 太重，这里用
/// `compare_exchange_weak(Running ↔ Locking)` 实现 **轻量级自旋锁**：
/// - 只有添加新依赖时才进 Locking，时间极短；
/// - 状态字段本身就是原子，复用其 CAS 不引入新原语；
/// - notify/wait 通过 `m_state.notify_all` / `wait` 直接生效。
///
/// ============================================================================
///  引用计数 m_use_count —— AsyncTask 内存释放协议
/// ============================================================================
/// 每个引用 Topology 的 AsyncTask 在构造 / 拷贝时 `_incref`，析构时 `_decref`。
/// `_decref` 归零返回 true 通知调用方"可释放节点"。这是 AsyncTask 强引用语义
/// 的实现底座 —— 节点的物理内存只在 **最后一份句柄释放后** 才回收，
/// 与"任务是否完成"完全解耦。
///
/// ============================================================================
///  m_stopped —— 软停止信号
/// ============================================================================
/// `atomic_flag` 表达 **建议性** 取消：
/// - 节点 invoke 前自检 `_stop_requested`（沿父链查询）—— 命中则跳过 body；
/// - 已开始 invoke 的节点不会被强制中断（C++ 没有可移植的硬中断）；
/// - 仅控制"未来还会跑的节点"是否真去跑，已发出的不可逆。
///
/// 这种"软停止" 在工程上是唯一可靠的协作式取消模型。
///
/// @see AsyncTask  使用 m_use_count 实现引用计数
/// @see Executor::wait_for_all  全局 topology 计数的等待者
/// @see ExplicitAnchorGuard / Runtime  父链异常归档
class Topology : public Immovable<Topology> {

    friend class Work;
    friend class Task;
    friend class AsyncTask;
    friend class DeferredAsyncTask;
    friend class Graph;
    friend class Runtime;
    friend class Executor;

    TFL_WORK_SUBCLASS_FRIENDS;

public:
    explicit Topology(Executor& exec) noexcept;
    ~Topology() = default;

private:
    /// @brief 生命周期状态枚举
    enum class State : std::int32_t {
        Idle     = 0, ///< 初始状态，尚未启动
        Running  = 1, ///< 执行中，任务正在被调度
        Locking  = 2, ///< 锁定状态，动态添加任务时的瞬态
        Finished = 3  ///< 执行完成
    };

    std::atomic<State> m_state{State::Idle};         ///< 状态机 + 轻量级锁
    std::stop_source   m_stop_source;
    Executor&          m_executor;                   ///< 所属调度器
    std::atomic<std::size_t> m_use_count{0};         ///< 引用计数

    // 状态查询与控制
    void _wait() const noexcept;
    void _incref() noexcept;
    [[nodiscard]] bool _decref() noexcept;
    [[nodiscard]] bool _is_running() const noexcept;
    [[nodiscard]] bool _is_finished() const noexcept;
    [[nodiscard]] std::size_t _use_count() const noexcept;
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
/// - notify_all: 由调度器在状态切换为 Finished 时触发
inline void Topology::_wait() const noexcept {
    auto state = m_state.load(std::memory_order_acquire);
    while (state != State::Finished) {
        m_state.wait(state, std::memory_order_acquire);
        state = m_state.load(std::memory_order_acquire);
    }
}

/// @brief 引用计数递增
///
/// @memory_order relaxed —— 仅计数，不涉及跨线程数据同步
inline void Topology::_incref() noexcept {
    m_use_count.fetch_add(1, std::memory_order_relaxed);
}

/// @brief 引用计数递减
/// @return true 表示计数归零，调用方可销毁本对象
///
/// @memory_order
/// - fetch_sub(acq_rel): 确保归零方观察到所有持有者侧的写入，
///   防止在内存释放后仍有其他线程的延迟写入命中已释放区域
inline bool Topology::_decref() noexcept {
    return m_use_count.fetch_sub(1, std::memory_order_acq_rel) == 1;
}

inline bool Topology::_is_running() const noexcept {
    auto s = m_state.load(std::memory_order_relaxed);
    return s == State::Running || s == State::Locking;
}

inline bool Topology::_is_finished() const noexcept {
    return m_state.load(std::memory_order_relaxed) == State::Finished;
}

inline std::size_t Topology::_use_count() const noexcept {
    return m_use_count.load(std::memory_order_relaxed);
}

}  // namespace tfl
