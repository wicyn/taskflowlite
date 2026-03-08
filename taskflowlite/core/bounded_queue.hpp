/// @file bounded_queue.hpp
/// @brief 无锁有界环形队列 - Work-Stealing 调度器的本地任务队列核心
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <functional>
#include <new>
#include <type_traits>
#include "utility.hpp"
#include "macros.hpp"

namespace tfl {

/// @brief 无锁双端环形队列 - Work-Stealing 算法的核心数据结构
///
/// @details
/// 基于环形缓冲区实现，支持单线程 Owner 操作（push/pop）与多线程 Stealer 操作（steal）。
/// 采用 2 的幂次方容量设计，利用位运算替代取模，提升热点路径性能。
///
/// @par 内存模型与同步协议
/// - Owner 线程：独占 bottom 指针的写入权限，通过 release 语义发布元素
/// - Stealer 线程：独占 top 指针的写入权限，通过 acquire 语义获取元素
/// - 两者通过 bottom > top 的偏序关系隐式同步，无需显式锁
///
/// @par ABA 问题防御
/// - 使用 64 位整数作为指针索引，ABA 发生概率理论上可忽略
/// - pop() 的 single-element 路径采用 CAS 解决与 steal() 的竞争
///
/// @tparam Tp 队列元素类型（必须为指针类型）
/// @tparam cap 队列容量（必须为 2 的 n 次方）
template <typename Tp, std::size_t cap = TFL_DEFAULT_QUEUE_SIZE>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
class BoundedQueue : public Immovable<BoundedQueue<Tp, cap>> {
public:
    using value_type = Tp;

    constexpr BoundedQueue() noexcept;
    ~BoundedQueue() noexcept = default;

    [[nodiscard]] static constexpr std::size_t capacity() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::int64_t ssize() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    /// @brief 尝试非阻塞推入（Owner 线程）
    /// @return true: 成功推入 | false: 队列满
    [[nodiscard]] bool try_push(Tp val) noexcept;

    /// @brief 带溢出回调的推入操作（Owner 线程）
    /// @param on_full 队列满时调用，接收待推入元素
    template <typename C>
        requires (std::invocable<C&&> && std::same_as<std::invoke_result_t<C&&>, void>)
    void push(Tp val, C&& on_full);

    /// @brief 批量推入操作（Owner 线程）
    /// @param first 元素范围起始迭代器
    /// @param n 元素数量
    /// @param on_full 溢出回调，接收剩余未推送元素
    template <std::random_access_iterator Iterator, typename C>
        requires (std::convertible_to<std::iter_reference_t<Iterator>, Tp> &&
                 std::invocable<C&&, Iterator, std::size_t> &&
                 std::same_as<std::invoke_result_t<C&&, Iterator, std::size_t>, void>)
    void push(Iterator first, std::size_t n, C&& on_full);

    /// @brief 从队列尾部弹出（Owner 线程，LIFO）
    /// @return 成功返回元素，队列空或 CAS 竞争失败返回 nullptr
    [[nodiscard]] Tp pop() noexcept;

    /// @brief 从队列头部窃取（Stealer 线程，FIFO）
    /// @return 成功返回元素，队列空或 CAS 竞争失败返回 nullptr
    [[nodiscard]] Tp steal() noexcept;

    /// @brief 窃取并统计连续空窃取次数
    /// @param num_empty_steals 输出：连续空窃取次数（成功窃取时重置为 0）
    [[nodiscard]] Tp steal(std::size_t& num_empty_steals) noexcept;

private:
    // 容量为 2^n 时，(cap-1) 可作为位掩码，index & k_mask == index % cap
    static constexpr std::size_t k_mask = cap - 1;

    // 2 倍缓存行对齐（典型值为 128 字节），隔离 owner 修改的 bottom 与 stealer 修改的 top
    alignas(2 * std::hardware_destructive_interference_size)
        std::atomic<Tp> m_buf[cap];

    alignas(2 * std::hardware_destructive_interference_size)
        std::atomic<std::int64_t> m_top;   ///< 仅 Stealer 写入

    alignas(2 * std::hardware_destructive_interference_size)
        std::atomic<std::int64_t> m_bottom; ///< 仅 Owner 写入
};

// ============================================================================
// Implementation - 内联实现以优化热点路径
// ============================================================================

template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
constexpr BoundedQueue<Tp, cap>::BoundedQueue() noexcept
    : m_buf{}
    , m_top{0}
    , m_bottom{0} {}

template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
constexpr std::size_t BoundedQueue<Tp, cap>::capacity() noexcept {
    return cap;
}

template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
std::size_t BoundedQueue<Tp, cap>::size() const noexcept {
    return static_cast<std::size_t>(ssize());
}

template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
std::int64_t BoundedQueue<Tp, cap>::ssize() const noexcept {
    // @invariant: bottom - top 即为队列当前元素数（当 bottom >= top 时）
    // relaxed 序仅保证本线程内可见性，不提供跨线程同步
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_relaxed);
    return std::max(bottom - top, std::int64_t{0});
}

template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
bool BoundedQueue<Tp, cap>::empty() const noexcept {
    // @invariant: top >= bottom <=> 队列为空
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_relaxed);
    return top >= bottom;
}

// ============================================================================
// try_push: Owner 线程非阻塞推入
// ============================================================================
/// @brief
/// Owner 线程尝试将元素推入队列尾部。
///
/// @memory_order 推演
/// - bottom: relaxed 读取（Owner 独占，无竞争风险）
/// - top: acquire 读取（必须看到 Stealer 的最新位置，否则可能溢出）
/// - buf[idx]: relaxed 写入（紧邻 bottom 修改，尚未对 Stealer 可见）
/// - bottom: release 写入（同步点，确保 buf 写入对 Stealer 可见）
///
/// @synchronizes-with: Stealer 的 acquire 读取 bottom
template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
bool BoundedQueue<Tp, cap>::try_push(Tp val) noexcept {
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_acquire);

    // @invariant: 队列满的条件是 (bottom - top) + 1 > cap
    // 即剩余空间 < 1 时不可推入
    if (static_cast<std::int64_t>(cap) < (bottom - top) + 1) {
        return false;
    }

    // 写入缓冲区（此时尚未对 Stealer 可见）
    m_buf[static_cast<std::size_t>(bottom) & k_mask].store(val, std::memory_order_relaxed);
    // release: 元素.publish()，Stealer 通过 acquire 可见
    m_bottom.store(bottom + 1, std::memory_order_release);
    return true;
}

// ============================================================================
// push(val, on_full): Owner 线程带溢出回调的推入
// ============================================================================
template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
             template <typename C>
                 requires (std::invocable<C&&> && std::same_as<std::invoke_result_t<C&&>, void>)
void BoundedQueue<Tp, cap>::push(Tp val, C&& on_full) {
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_acquire);

    if (static_cast<std::int64_t>(cap) < (bottom - top) + 1) {
        std::invoke(on_full); // 溢出处理
        return;
    }

    m_buf[static_cast<std::size_t>(bottom) & k_mask].store(val, std::memory_order_relaxed);
    m_bottom.store(bottom + 1, std::memory_order_release);
}

// ============================================================================
// push(first, n, on_full): Owner 线程批量推入
// ============================================================================
/// @brief
/// 批量推入元素，充分利用局部性（locality）减少原子操作次数。
///
/// @performance: O(min(n, available)) 批量写入，仅一次 atomic store
template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
             template <std::random_access_iterator Iterator, typename C>
                 requires (std::convertible_to<std::iter_reference_t<Iterator>, Tp> &&
                          std::invocable<C&&, Iterator, std::size_t> &&
                          std::same_as<std::invoke_result_t<C&&, Iterator, std::size_t>, void>)
void BoundedQueue<Tp, cap>::push(Iterator first, std::size_t n, C&& on_full) {
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_acquire);
    std::int64_t const available = static_cast<std::int64_t>(cap) - (bottom - top);

    if (available <= 0) [[unlikely]] {
        std::invoke(on_full, first, n);
        return;
    }

    std::size_t const count = static_cast<std::size_t>(
        std::min(static_cast<std::int64_t>(n), available)
    );

    // 批量写入缓冲区（全部使用 relaxed 序，最后一次性 publish）
    for (std::size_t i = 0; i < count; ++i) {
        m_buf[static_cast<std::size_t>(bottom + i) & k_mask].store(
            static_cast<Tp>(first[i]), std::memory_order_relaxed);
    }

    // 一次性 release，.publish() 所有已写入元素
    m_bottom.store(bottom + static_cast<std::int64_t>(count), std::memory_order_release);

    if (count < n) [[unlikely]] {
        std::invoke(on_full, first + count, n - count);
    }
}

// ============================================================================
// pop: Owner 线程从尾部弹出（LIFO）
// ============================================================================
/// @brief
/// Owner 线程从队列尾部取出元素。
///
/// @algorithm
/// 1. 预占一个位置（bottom - 1）
/// 2. 读取 top 判断队列是否为空
/// 3. 若为最后一个元素，使用 CAS 处理与 Stealer 的竞争（ABA 防御）
///
/// @memory_order 推演
/// - bottom-1: relaxed 写入（预占位置）
/// - fence(seq_cst): 强制 bottom 写操作先于 top 读取完成
///   防止 CPU 乱序导致在判断队列是否为空时看到过期的 top 值
/// - top: relaxed 读取（仅作为比较基准，不依赖其值进行同步）
/// - buf[idx]: relaxed 读取（Owner 独占访问）
/// - CAS(top, top+1): seq_cst 解决与 Stealer 的竞争
///   - 成功: 当前线程最后一个获取元素，Stealer 无法再窃取
///   - 失败: Stealer 先一步窃取，当前线程放弃
///
/// @ ABA 防御机制
/// 当仅剩一个元素时（top == bottom - 1），pop() 与 steal() 可能同时操作。
/// 使用 CAS 尝试原子递增 top：
/// - 若成功：当前线程获得元素
/// - 若失败（Stealer 已窃取）：放弃并还原 bottom
template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
Tp BoundedQueue<Tp, cap>::pop() noexcept {
    // 预占位置：bottom' = bottom - 1
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed) - 1;
    m_bottom.store(bottom, std::memory_order_relaxed);

    // 强制同步：确保先完成 bottom 写入，再读取 top
    // 防止 CPU 流水线将 top 读取重排到 bottom 写入之前
    std::atomic_thread_fence(std::memory_order_seq_cst);

    std::int64_t top = m_top.load(std::memory_order_relaxed);

    if (top <= bottom) {
        Tp val = m_buf[static_cast<std::size_t>(bottom) & k_mask].load(std::memory_order_relaxed);

        // 最后一个元素的竞争处理
        if (top == bottom) {
            // CAS: 尝试原子地将 top 从 top 改为 top+1
            // success: release 语义使本次 pop 结果对其他线程可见
            // failure: Stealer 已抢先，放弃并还原 bottom
            if (!m_top.compare_exchange_strong(top, top + 1,
                                               std::memory_order_seq_cst, std::memory_order_relaxed)) {
                m_bottom.store(bottom + 1, std::memory_order_relaxed);
                return nullptr;
            }
            m_bottom.store(bottom + 1, std::memory_order_relaxed);
        }
        return val;
    }

    // 队列为空，还原 bottom
    m_bottom.store(bottom + 1, std::memory_order_relaxed);
    return nullptr;
}

// ============================================================================
// steal: Stealer 线程从头部窃取（FIFO）
// ============================================================================
/// @brief
/// 其他工作线程从队列头部窃取元素。
///
/// @algorithm
/// 1. 读取 top（作为候选元素索引）
/// 2. 读取 bottom 确认有元素可窃取
/// 3. CAS 尝试原子递增 top
///
/// @memory_order 推演
/// - top: acquire 读取（必须获取 Owner publish 的最新位置）
/// - fence(seq_cst): 强制 top 读取先于 bottom 读取
/// - bottom: acquire 读取（确认是否有元素可窃取）
/// - buf[idx]: relaxed 读取（窃取后元素仍保留在队列中，直到 CAS 成功）
/// - CAS(top, top+1): seq_cst 确保 top 递增的原子性
///
/// @synchronizes-with: Owner 的 release 写入 bottom
///
/// @note: 即使窃取成功，原元素仍保留在队列中（被覆盖前），这是安全的设计
template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
Tp BoundedQueue<Tp, cap>::steal() noexcept {
    std::int64_t top = m_top.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::int64_t const bottom = m_bottom.load(std::memory_order_acquire);

    Tp val{nullptr};
    if (top < bottom) {
        // 读取元素（此时尚未真正"获取"，仅作为候选）
        val = m_buf[static_cast<std::size_t>(top) & k_mask].load(std::memory_order_relaxed);

        // CAS 尝试获取元素（可能与其他 Stealer 竞争）
        if (!m_top.compare_exchange_strong(top, top + 1,
                                           std::memory_order_seq_cst, std::memory_order_relaxed)) {
            return nullptr; // 竞争失败
        }
    }

    return val;
}

// ============================================================================
// steal(num_empty_steals): 窃取并统计空窃取
// ============================================================================
/// @brief
/// steal() 的变体，统计连续空窃取次数用于自适应退避。
///
/// @performance: 成功窃取时 O(1)，空窃取时 O(1)
template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
Tp BoundedQueue<Tp, cap>::steal(std::size_t& num_empty_steals) noexcept {
    std::int64_t top = m_top.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::int64_t const bottom = m_bottom.load(std::memory_order_acquire);

    Tp val{nullptr};
    if (top < bottom) {
        num_empty_steals = 0; // 成功窃取，重置计数器
        val = m_buf[static_cast<std::size_t>(top) & k_mask].load(std::memory_order_relaxed);

        if (!m_top.compare_exchange_strong(top, top + 1,
                                           std::memory_order_seq_cst, std::memory_order_relaxed)) {
            return nullptr;
        }
    } else {
        ++num_empty_steals; // 空窃取，累计用于调度器决策
    }

    return val;
}

} // namespace tfl
