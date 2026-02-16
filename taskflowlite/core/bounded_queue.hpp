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

    [[nodiscard]] bool try_push(Tp val) noexcept;

    template <typename C>
        requires (std::invocable<C&&> && std::same_as<std::invoke_result_t<C&&>, void>)
    void push(Tp val, C&& on_full);

    template <std::random_access_iterator Iterator, typename C>
        requires (std::convertible_to<std::iter_reference_t<Iterator>, Tp> &&
                 std::invocable<C&&, Iterator, std::size_t> &&
                 std::same_as<std::invoke_result_t<C&&, Iterator, std::size_t>, void>)
    void push(Iterator first, std::size_t n, C&& on_full);

    // template <std::random_access_iterator Iterator>
    //     requires std::convertible_to<std::iter_reference_t<Iterator>, Tp>
    // [[nodiscard]] std::size_t push(Iterator first, std::size_t n) noexcept;

    [[nodiscard]] Tp pop() noexcept;

    [[nodiscard]] Tp steal() noexcept;

    [[nodiscard]] Tp steal(std::size_t& num_empty_steals) noexcept;

private:
    static constexpr std::size_t k_mask = cap - 1;

    alignas(2 * std::hardware_destructive_interference_size)
        std::atomic<Tp> m_buf[cap];

    alignas(2 * std::hardware_destructive_interference_size)
        std::atomic<std::int64_t> m_top;

    alignas(2 * std::hardware_destructive_interference_size)
        std::atomic<std::int64_t> m_bottom;
};

// ============================================================================
// Implementation
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
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_relaxed);
    return std::max(bottom - top, std::int64_t{0});
}

template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
bool BoundedQueue<Tp, cap>::empty() const noexcept {
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_relaxed);
    return top >= bottom;

}

template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
bool BoundedQueue<Tp, cap>::try_push(Tp val) noexcept {
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_acquire);

    if (static_cast<std::int64_t>(cap) < (bottom - top) + 1) {
        return false;
    }

    m_buf[static_cast<std::size_t>(bottom) & k_mask].store(val, std::memory_order_relaxed);
    m_bottom.store(bottom + 1, std::memory_order_release);
    return true;
}

template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
             template <typename C>
                 requires (std::invocable<C&&> && std::same_as<std::invoke_result_t<C&&>, void>)
void BoundedQueue<Tp, cap>::push(Tp val, C&& on_full) {
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_acquire);

    if (static_cast<std::int64_t>(cap) < (bottom - top) + 1) {
        on_full();
        return;
    }

    m_buf[static_cast<std::size_t>(bottom) & k_mask].store(val, std::memory_order_relaxed);
    m_bottom.store(bottom + 1, std::memory_order_release);
}

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
        on_full(first, n);
        return;
    }

    std::size_t const count = static_cast<std::size_t>(
        std::min(static_cast<std::int64_t>(n), available)
        );

    for (std::size_t i = 0; i < count; ++i) {
        m_buf[static_cast<std::size_t>(bottom + i) & k_mask].store(
            static_cast<Tp>(first[i]), std::memory_order_relaxed);
    }

    m_bottom.store(bottom + static_cast<std::int64_t>(count), std::memory_order_release);

    if (count < n) [[unlikely]] {
        on_full(first + count, n - count);
    }
}

// template <typename Tp, std::size_t cap>
//     requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
//              template <std::random_access_iterator Iterator>
//                  requires std::convertible_to<std::iter_reference_t<Iterator>, Tp>
// std::size_t BoundedQueue<Tp, cap>::push(Iterator first, std::size_t n) noexcept {
//     if (n == 0) [[unlikely]] {
//         return 0;
//     }

//     std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
//     std::int64_t const top = m_top.load(std::memory_order_acquire);
//     std::int64_t const available = static_cast<std::int64_t>(cap) - (bottom - top);

//     if (available <= 0) [[unlikely]] {
//         return 0;
//     }

//     std::size_t const count = static_cast<std::size_t>(
//         std::min(static_cast<std::int64_t>(n), available)
//         );

//     for (std::size_t i = 0; i < count; ++i) {
//         m_buf[static_cast<std::size_t>(bottom + i) & k_mask].store(static_cast<Tp>(first[i]), std::memory_order_relaxed);
//     }

//     m_bottom.store(bottom + static_cast<std::int64_t>(count), std::memory_order_release);
//     return count;
// }

template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
Tp BoundedQueue<Tp, cap>::pop() noexcept {
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed) - 1;
    m_bottom.store(bottom, std::memory_order_relaxed);

    std::atomic_thread_fence(std::memory_order_seq_cst);

    std::int64_t top = m_top.load(std::memory_order_relaxed);

    if (top <= bottom) {
        Tp val = m_buf[static_cast<std::size_t>(bottom) & k_mask].load(std::memory_order_relaxed);

        if (top == bottom) {
            if (!m_top.compare_exchange_strong(top, top + 1,
                                               std::memory_order_seq_cst, std::memory_order_relaxed)) {
                m_bottom.store(bottom + 1, std::memory_order_relaxed);
                return nullptr;
            }
            m_bottom.store(bottom + 1, std::memory_order_relaxed);
        }
        return val;
    }

    m_bottom.store(bottom + 1, std::memory_order_relaxed);
    return nullptr;
}

template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
Tp BoundedQueue<Tp, cap>::steal() noexcept {
    std::int64_t top = m_top.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::int64_t const bottom = m_bottom.load(std::memory_order_acquire);

    Tp val{nullptr};
    if (top < bottom) {
        val = m_buf[static_cast<std::size_t>(top) & k_mask].load(std::memory_order_relaxed);

        if (!m_top.compare_exchange_strong(top, top + 1,
                                           std::memory_order_seq_cst, std::memory_order_relaxed)) {
            return nullptr;
        }
    }

    return val;
}

template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
Tp BoundedQueue<Tp, cap>::steal(std::size_t& num_empty_steals) noexcept {
    std::int64_t top = m_top.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::int64_t const bottom = m_bottom.load(std::memory_order_acquire);

    Tp val{nullptr};
    if (top < bottom) {
        // 有元素，重置计数
        num_empty_steals = 0;
        val = m_buf[static_cast<std::size_t>(top) & k_mask].load(std::memory_order_relaxed);

        if (!m_top.compare_exchange_strong(top, top + 1,
                                           std::memory_order_seq_cst, std::memory_order_relaxed)) {
            return nullptr;
        }
    } else {
        // 队列真的空了才递增
        ++num_empty_steals;
    }

    return val;
}

} // namespace tfl

