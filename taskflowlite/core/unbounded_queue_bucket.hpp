/// @file unbounded_queue_bucket.hpp
/// @brief 提供无界队列桶，用于多线程任务分发时的负载均衡。
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstdint>
#include <iterator>
#include <new>
#include <vector>
#include <thread>

#include "unbounded_queue.hpp"

namespace tfl {

/// @brief 无界队列桶，用于多线程任务分发时的负载均衡
///
/// @details 在多线程工作窃取系统中，如果外部线程同时提交任务到单一共享队列，
/// 会产生严重的锁竞争（Contention）。
///
/// 该类内部包含多个 UnboundedQueue，通过哈希将任务分散到不同队列，
/// 减少锁竞争，提升吞吐量。队列数量为 log2(线程数)。
///
/// @tparam Tp 元素类型（必须为指针类型）
template <typename Tp>
    requires std::is_pointer_v<Tp>
class UnboundedQueueBucket : public Immovable<UnboundedQueueBucket<Tp>> {

public:
    /// @brief 构造函数
    /// @param n 工作线程数量，内部计算合适的队列数量（通常为 log2(n)）
    explicit UnboundedQueueBucket(std::size_t n);

    /// @brief 将任务推入桶中，自动选择空闲队列
    /// @param val 待推送任务
    void push(Tp val);

    /// @brief 批量将任务推入桶中
    /// @param first 元素范围起始迭代器
    /// @param n 元素数量
    template <std::random_access_iterator Iterator>
        requires std::convertible_to<std::iter_reference_t<Iterator>, Tp>
    void push(Iterator first, std::size_t n);

    /// @brief 从指定队列窃取任务
    /// @param w 队列索引
    /// @return 成功窃取返回任务，队列空返回 nullptr
    [[nodiscard]] Tp steal(std::size_t w) noexcept;

    /// @brief 从指定队列窃取任务并统计空窃取次数
    /// @param w 队列索引
    /// @param num_empty_steals 输出参数，记录连续空窃取次数
    /// @return 成功窃取返回任务，队列空返回 nullptr
    [[nodiscard]] Tp steal(std::size_t w, std::size_t& num_empty_steals) noexcept;

    /// @brief 检查指定队列是否为空
    /// @param w 队列索引
    /// @return 空返回 true
    [[nodiscard]] bool empty(std::size_t w) const noexcept;

    /// @brief 返回桶中的队列数量
    /// @return 队列数量
    [[nodiscard]] std::size_t size() const noexcept;

private:
    // Why: 使用 2 倍缓存行大小对齐，防止伪共享
    // 当多个线程同时访问不同队列的锁时，避免缓存行失效
    struct alignas(2 * std::hardware_destructive_interference_size) AlignedMutex {
        std::atomic_flag flag = ATOMIC_FLAG_INIT;
    };

    std::vector<AlignedMutex> m_mutexes;      ///< 每个队列的轻量级自旋锁
    std::vector<UnboundedQueue<Tp>> m_queues; ///< 无界队列数组
};

// ============================================================================
// Implementation
// ============================================================================

template <typename Tp>
    requires std::is_pointer_v<Tp>
UnboundedQueueBucket<Tp>::UnboundedQueueBucket(std::size_t n)
    // Why: 使用 bit_width 计算 log2，队列数量为 log2(n)
    // 例如 16 个线程对应 5 个队列，足够分散压力
    : m_mutexes{static_cast<std::size_t>(std::bit_width(n))}
    , m_queues{static_cast<std::size_t>(std::bit_width(n))} {}


template <typename Tp>
    requires std::is_pointer_v<Tp>
void UnboundedQueueBucket<Tp>::push(Tp val) {
    std::uintptr_t const ptr = reinterpret_cast<std::uintptr_t>(val);
    std::size_t const size = m_queues.size();

    // Why: 使用指针地址作为哈希种子，计算目标队列索引
    std::size_t const b = ((ptr >> 16) ^ (ptr >> 8)) % size;

    // 循环直到成功推送
    for (;;) {
        // 第一轮：从目标队列开始向后查找可用队列
        for (std::size_t curr_b = b; curr_b < size; ++curr_b) {
            auto& flag = m_mutexes[curr_b].flag;
            // 尝试获取锁
            if (!flag.test_and_set(std::memory_order_acquire)) {
                m_queues[curr_b].push(val);
                // 释放锁
                flag.clear(std::memory_order_release);
                return;
            }
        }

        // 第二轮：如果后面都满了，从头开始查找
        for (std::size_t curr_b = 0; curr_b < b; ++curr_b) {
            auto& flag = m_mutexes[curr_b].flag;
            if (!flag.test_and_set(std::memory_order_acquire)) {
                m_queues[curr_b].push(val);
                flag.clear(std::memory_order_release);
                return;
            }
        }

        // Why: 所有队列都忙，让出 CPU 等待
        std::this_thread::yield();
    }
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
template <std::random_access_iterator Iterator>
    requires std::convertible_to<std::iter_reference_t<Iterator>, Tp>
void UnboundedQueueBucket<Tp>::push(Iterator first, std::size_t n) {
    // 逻辑与单体 push 相同，批量推送
    std::uintptr_t const ptr = reinterpret_cast<std::uintptr_t>(*first);
    std::size_t const size = m_queues.size();
    std::size_t const b = ((ptr >> 16) ^ (ptr >> 8)) % size;

    for (;;) {
        for (std::size_t curr_b = b; curr_b < size; ++curr_b) {
            auto& flag = m_mutexes[curr_b].flag;
            if (!flag.test_and_set(std::memory_order_acquire)) {
                m_queues[curr_b].push(first, n);
                flag.clear(std::memory_order_release);
                return;
            }
        }

        for (std::size_t curr_b = 0; curr_b < b; ++curr_b) {
            auto& flag = m_mutexes[curr_b].flag;
            if (!flag.test_and_set(std::memory_order_acquire)) {
                m_queues[curr_b].push(first, n);
                flag.clear(std::memory_order_release);
                return;
            }
        }

        std::this_thread::yield();
    }
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
Tp UnboundedQueueBucket<Tp>::steal(std::size_t w) noexcept {
    // 从指定队列窃取
    return m_queues[w].steal();
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
Tp UnboundedQueueBucket<Tp>::steal(std::size_t w, std::size_t& num_empty_steals) noexcept {
    return m_queues[w].steal(num_empty_steals);
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
bool UnboundedQueueBucket<Tp>::empty(std::size_t w) const noexcept {
    return m_queues[w].empty();
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
std::size_t UnboundedQueueBucket<Tp>::size() const noexcept {
    return m_queues.size();
}

} // namespace tfl
