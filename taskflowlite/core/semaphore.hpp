/// @file  semaphore.hpp
/// @brief 任务级信号量 Semaphore —— 不阻塞 Worker 线程的并发限流原语。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
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
/// 与 OS 信号量的本质差异：acquire 失败时 Worker 不睡眠，而是把任务放入
/// 内部等待队列后立即返回执行其他就绪任务。release 时把 waiter 批量 swap
/// 出临界区再推回调度器（惊群批处理优化）。这是 M:N 调度模型成立的必要前提。
///
/// @note 线程安全：全局 mutex 保护内部状态，不在热路径上。
class Semaphore : public Immovable<Semaphore> {
    friend class Work;
    friend class Executor;
    friend class Task;

public:
    /// @brief 构造函数, 初始化信号量的最大并发容量。
    /// @param max_value 允许的最大并发数量。初始可用计数也等同于此值。
    /// @param name 信号量的名称 (可选, 通常用于调试或日志追踪)。
    explicit Semaphore(std::size_t max_value, std::string name = "");

    /// @brief 构造函数, 分别指定最大容量与当前初始可用容量。
    /// @param max_value 允许的最大并发数量。
    /// @param current_value 初始时刻的可用计数。
    /// @param name 信号量的名称 (可选)。
    /// @note 内部会自动将初始可用计数裁剪至不超过最大容量, 防止不变量被打破。
    Semaphore(std::size_t max_value, std::size_t current_value, std::string name = "");

    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    /// @brief 线程安全地获取当前剩余可用计数。
    /// @return 当前可用配额数 (加锁快照)。
    [[nodiscard]] std::size_t value() const noexcept;

    /// @brief 获取信号量的最大设计容量。
    /// @return 最大并发配额上限。
    [[nodiscard]] std::size_t max_value() const noexcept;

    /// @brief 重置信号量的容量并完全恢复可用计数。
    /// @param max_value 新的最大并发数量。
    /// @pre 必须确保当前没有任何任务在此信号量上处于等待挂起状态。
    /// @throw Exception 如果内部等待队列非空, 抛出异常。
    void reset(std::size_t max_value);

    /// @brief 重置信号量容量, 并显式指定当前可用计数。
    /// @param max_value 新的最大并发数量。
    /// @param current_value 新的可用计数。
    /// @pre 必须确保当前没有任何任务在此信号量上处于等待挂起状态。
    /// @throw Exception 如果内部等待队列非空, 抛出异常。
    void reset(std::size_t max_value, std::size_t current_value);

    /// @brief 线程安全地获取信号量的名称。
    /// @return 信号量名称的只读视图。
    [[nodiscard]] std::string_view name() const noexcept;

    /// @brief 线程安全地设置信号量的名称。
    /// @tparam S 可构造 std::string 的类型。
    /// @param name 新的信号量名称。
    /// @return *this, 支持链式调用。
    template <typename S>
        requires std::constructible_from<std::string, S>
    Semaphore& name(S&& name);

private:
    std::string m_name;
    /// @brief 保护内部状态的互斥锁 —— 不在热路径上, 原子操作不用在此处过度优化。
    mutable std::mutex m_lock;
    /// @brief 允许的最大并发边界。
    std::size_t m_max_value{0};
    /// @brief 运行时动态追踪的当前可用授权数。
    std::size_t m_value{0};
    /// @brief 因获取失败而被迫挂起的拦截任务队列。
    SmallVector<Work*> m_waiters;


    /// @brief 框架内部调用: 尝试非阻塞地获取指定数量的授权配额。
    /// @param w 发起尝试的任务节点指针。
    /// @param count 需要获取的配额数量。
    /// @return 成功扣减计数返回 true; 否则自动将该任务压入等待队列并返回 false。
    [[nodiscard]] bool _try_acquire(Work* w, std::size_t count);

    /// @brief 框架内部调用: 归还配额并释放等待者。
    /// @param count 归还的配额数量。
    /// @param out   [out] 被唤醒的任务列表, 由调用方统一推回调度器。
    void _release(std::size_t count, SmallVector<Work*>& out);
};


// ==============================================================================
//  实现
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

    // 配额充足, 直接扣减放行
    if (m_value >= count) {
        m_value -= count;
        return true;
    }

    // 当配额不足以满足当前 count 时, 直接将任务记录在案, 随后返回 false。
    // 这指导底层的 Worker 线程立即放弃此任务并投身于窃取网络,
    // 彻底杜绝了并发死锁与 CPU 空转
    m_waiters.push_back(w);
    return false;
}

inline void Semaphore::_release(std::size_t count, SmallVector<Work*>& out) {
    SmallVector<Work*> batch;
    {
        std::lock_guard lk{m_lock};

        // 严格断言不变式, 自证下面的减法绝对不会发生无符号下溢
        TFL_ASSERT(m_value <= m_max_value && "semaphore invariant broken");

        // 归还配额, 并进行安全裁剪以防溢出 (防范用户在 DAG 拓扑中配错 release 数量)
        if (m_max_value - m_value >= count) {
            m_value += count;
        } else {
            m_value = m_max_value;
        }

        // 只有当确实有任务在等待, 且当前有可用资源时, 才进行唤醒操作。
        // 使用 SmallVector swap 是一种经典的锁粒度优化 ("惊群"批处理):
        // 一次性将挂起的队列移交到局部栈上, 使得互斥锁能够被极速释放,
        // 防止因唤醒过程过长拖累并发度
        if (m_waiters.empty()) {
            return;
        }
        batch.swap(m_waiters);
    }

    // 锁外完成 out 聚合, 即使 out 需要 malloc 扩容也不占临界区。
    // 快路径 (单 sem 场景): out 空, 直接 move 整个 storage, 3 个指针赋值
    if (out.empty()) {
        out = std::move(batch);
    } else {
        out.reserve(out.size() + batch.size());
        out.insert(out.end(), batch.begin(), batch.end());
    }
}

}  // namespace tfl
