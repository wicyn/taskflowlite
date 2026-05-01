/// @file semaphore.hpp
/// @brief 任务级信号量 Semaphore —— 不阻塞 Worker 线程的并发限流原语
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cstddef>
#include <vector>
#include <algorithm>
#include <cassert>
#include <mutex>
#include <string>
#include <string_view>
#include <functional>

#include "small_vector.hpp"
#include "exception.hpp"

namespace tfl {

/// @brief 任务级并发限流信号量 —— 配额不足时挂起任务而非线程。
///
/// @details
/// `Semaphore` 是 taskflow-lite 在 OS 级信号量之上做的关键再设计：
/// **不允许 Worker 线程阻塞**。任务 `acquire` 失败时，Worker 不睡眠，而是
/// 把该任务放进信号量内部的等待队列，**立即返回去执行其他就绪任务** ——
/// 这是工作池在面对"资源竞争"场景时仍保持满 CPU 利用率的根因。
///
/// ============================================================================
///  与 OS Semaphore 的本质差异
/// ============================================================================
///
/// | 维度          | OS Semaphore (POSIX/std)        | tfl::Semaphore                 |
/// |---------------|----------------------------------|--------------------------------|
/// | 阻塞对象      | **线程**                          | **任务**（Work* 入等待队列）    |
/// | 失败行为      | 调用线程 OS 挂起                   | 任务挂起，线程返回工作池           |
/// | 适用场景      | 跨进程同步、生产者消费者            | DAG 内并发限流、资源池             |
/// | 死锁风险      | 高（线程被锁，工作池可能枯竭）       | 极低（线程不阻塞，仍能 steal）     |
/// | 唤醒          | OS condvar / futex                | release 时把 waiter 推回调度器   |
///
/// 这是"M:N 调度（M 任务 : N 线程）"模型成立的必要前提。
///
/// ============================================================================
///  Acquire / Release 协议
/// ============================================================================
/// **Acquire（`_try_acquire`）**：
/// - 配额充足 → 原子扣减 + 返回 true，调度器继续 invoke 任务；
/// - 配额不足 → 把任务推入 `m_waiters`，返回 false。Worker 拿到 false 立刻让
///   该任务"消失"出本次调度，不做任何 tear_down 处理 —— 由后续 release 唤醒。
/// - **多 sem 的部分回滚**：节点声明多个 acquire 时，任意一个失败就把已成功
///   获取的逐个 release 回滚 —— `_try_acquire_semaphores` 的工作。
///
/// **Release（`_release`）**：
/// - 加回配额（防御性裁剪到 max_value 上限，防止用户 release 数对不上）；
/// - 把全部 waiter 一次性 swap 到栈局部变量后释放锁 —— 经典"惊群批处理"
///   优化：唤醒过程不在临界区内进行；
/// - 唤醒的 waiters 由调用方统一推回调度器（参数 `out`）。
///
/// ============================================================================
///  reset 的硬约束
/// ============================================================================
/// `reset()` / `reset(max, current)` 要求 **当前等待队列必须为空**，否则抛
/// `Exception`。原因：waiter 已经把自己锁定在某个 max_value 上，运行时改变
/// 容量会破坏调用契约。
///
/// ============================================================================
///  线程安全
/// ============================================================================
/// 全局 `m_lock` mutex 保护内部状态。这是框架内为数不多的**走 mutex 而非原子**的
/// 组件 —— 因为信号量操作天然要求原子地读 m_value、决定是否扣减、必要时入队 ——
/// 这套"读-决策-写"的 RMW 用 mutex 表达比用复杂的原子组合更直观，且不在热路径上。
///
/// ============================================================================
///  使用示例
/// ============================================================================
/// @code
///   Semaphore db_pool{4};        // 数据库连接池上限 4
///
///   auto t = flow.emplace([]{ query_db(); });
///   t.acquire(db_pool).release(db_pool);   // 进入需占 1，完成时释放 1
///
///   // 或多配额
///   auto heavy = flow.emplace([]{ batch_query(); });
///   heavy.acquire(db_pool, 3).release(db_pool, 3);  // 占 3 个连接
/// @endcode
///
/// @see Task::acquire / Task::release  用户配置入口
/// @see Work::_try_acquire_semaphores  执行期协议
class Semaphore : public Immovable<Semaphore> {
    friend class Work;
    friend class Executor;
    friend class Task;

public:
    /// @brief 构造函数，初始化信号量的最大并发容量。
    /// @param max_value 允许的最大并发数量。初始可用计数也等同于此值。
    /// @param name 信号量的名称（可选，通常用于调试或日志追踪）。
    explicit Semaphore(std::size_t max_value, std::string name = "");

    /// @brief 构造函数，分别指定最大容量与当前初始可用容量。
    /// @param max_value 允许的最大并发数量。
    /// @param current_value 初始时刻的可用计数。
    /// @param name 信号量的名称（可选，通常用于调试或日志追踪）。
    /// @note 内部会自动将初始可用计数裁剪至不超过最大容量。
    Semaphore(std::size_t max_value, std::size_t current_value, std::string name = "");

    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    /// @brief 线程安全地获取当前剩余可用计数。
    [[nodiscard]] std::size_t value() const noexcept;

    /// @brief 获取信号量的最大设计容量。
    [[nodiscard]] std::size_t max_value() const noexcept;

    /// @brief 重置信号量的容量并完全恢复可用计数。
    /// @param max_value 新的最大并发数量。
    /// @pre 必须确保当前没有任何任务在此信号量上处于等待挂起状态。
    /// @exception Exception 如果内部等待队列非空，抛出异常。
    void reset(std::size_t max_value);

    /// @brief 重置信号量容量，并显式指定当前可用计数。
    /// @param max_value 新的最大并发数量。
    /// @param current_value 新的可用计数。
    /// @pre 必须确保当前没有任何任务在此信号量上处于等待挂起状态。
    /// @exception Exception 如果内部等待队列非空，抛出异常。
    void reset(std::size_t max_value, std::size_t current_value);

    /// @brief 线程安全地获取信号量的名称。
    [[nodiscard]] std::string_view name() const noexcept;

    /// @brief 线程安全地设置信号量的名称。
    /// @param name 新的信号量名称。
    template <typename S>
        requires std::constructible_from<std::string, S>
    Semaphore& name(S&& name);
private:
    std::string m_name;
    mutable std::mutex m_lock;       ///< 保护内部状态的互斥锁
    std::size_t m_max_value{0};      ///< 允许的最大并发边界
    std::size_t m_value{0};          ///< 运行时动态追踪的当前可用授权数
    SmallVector<Work*> m_waiters;    ///< 因获取失败而被迫挂起的拦截任务队列


    /// @brief 框架内部调用：尝试非阻塞地获取指定数量的授权配额。
    /// @param w 发起尝试的任务节点指针。
    /// @param count 需要获取的配额数量。
    /// @return 成功扣减计数返回 true；否则自动将该任务压入等待队列并返回 false。
    [[nodiscard]] bool _try_acquire(Work* w, std::size_t count);

    void _release(std::size_t count, SmallVector<Work*>& out);
};


// ==============================================================================
// 类的实现部分 (Implementation)
// ==============================================================================

inline Semaphore::Semaphore(std::size_t max_value, std::string name)
    : m_name{std::move(name)}
    , m_max_value{max_value}
    , m_value{max_value} {}

inline Semaphore::Semaphore(std::size_t max_value, std::size_t current_value, std::string name)
    : m_name{std::move(name)}
    , m_max_value{max_value}
    , m_value{(std::min)(current_value, max_value)} {}

inline std::size_t Semaphore::value() const noexcept {
    std::lock_guard lk{m_lock};
    return m_value;
}

inline std::size_t Semaphore::max_value() const noexcept {
    return m_max_value;
}

inline void Semaphore::reset(std::size_t max_value) {
    std::lock_guard lk{m_lock};
    if (!m_waiters.empty()) {
        throw Exception("cannot reset while waiters exist.");
    }
    m_max_value = max_value;
    m_value = m_max_value;
}

inline void Semaphore::reset(std::size_t max_value, std::size_t current_value) {
    std::lock_guard lk{m_lock};
    if (!m_waiters.empty()) {
        throw Exception("cannot reset while waiters exist.");
    }
    m_max_value = max_value;
    m_value = (std::min)(current_value, max_value);
}

inline std::string_view Semaphore::name() const noexcept {
    return m_name;
}

template <typename S>
    requires std::constructible_from<std::string, S>
inline Semaphore& Semaphore::name(S&& name) {
    m_name = std::forward<S>(name);
    return *this;
}

inline bool Semaphore::_try_acquire(Work* w, std::size_t count) {
    std::lock_guard lk{m_lock};

    // 配额充足，直接扣减放行
    if (m_value >= count) {
        m_value -= count;
        return true;
    }

    // Why: 当配额不足以满足当前 count 时，直接将任务记录在案，随后返回 false。
    // 这指导底层的 Worker 线程立即放弃此任务并投身于窃取网络，彻底杜绝了并发死锁与 CPU 空转。
    m_waiters.push_back(w);
    return false;
}

inline void Semaphore::_release(std::size_t count, SmallVector<Work*>& out) {
    SmallVector<Work*> batch;
    {
        std::lock_guard lk{m_lock};

        // Why: 严格断言不变式，自证下面的减法绝对不会发生无符号下溢
        TFL_ASSERT(m_value <= m_max_value && "semaphore invariant broken");

        // 归还配额，并进行安全裁剪以防溢出（防范用户在 DAG 拓扑中配错 release 数量）
        if (m_max_value - m_value >= count) {
            m_value += count;
        } else {
            m_value = m_max_value;
        }

        // Why: 只有当确实有任务在等待，且当前有可用资源时，才进行唤醒操作。
        // 使用 std::vector::swap 是一种经典的锁粒度优化（"惊群"批处理）。
        // 一次性将挂起的队列移交到局部栈上，使得互斥锁能够被极速释放，防止因唤醒过程过长拖累并发度。
        if (m_waiters.empty()) {
            return;
        }
        batch.swap(m_waiters);
    }

    // Why: 锁外完成 out 聚合, 即使 out 需要 malloc 扩容也不占临界区
    // 快路径 (单 sem 场景): out 空, 直接 move 整个 storage, 3 个指针赋值
    if (out.empty()) {
        out = std::move(batch);
    } else {
        out.reserve(out.size() + batch.size());
        out.insert(out.end(), batch.begin(), batch.end());
    }
}

}  // namespace tfl
