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

template <typename Tp>
    requires std::is_pointer_v<Tp>
class UnboundedQueueBucket : public Immovable<UnboundedQueueBucket<Tp>> {

public:
    explicit UnboundedQueueBucket(std::size_t n);

    void push(Tp val);

    template <std::random_access_iterator Iterator>
        requires std::convertible_to<std::iter_reference_t<Iterator>, Tp>
    void push(Iterator first, std::size_t n);

    [[nodiscard]] Tp steal(std::size_t w) noexcept;

    [[nodiscard]] Tp steal(std::size_t w, std::size_t& num_empty_steals) noexcept;

    [[nodiscard]] bool empty(std::size_t w) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct alignas(2 * std::hardware_destructive_interference_size) AlignedMutex {
        std::atomic_flag flag = ATOMIC_FLAG_INIT;
    };

    std::vector<AlignedMutex> m_mutexes;
    std::vector<UnboundedQueue<Tp>> m_queues;
};

// ============================================================================
// Implementation
// ============================================================================


template <typename Tp>
    requires std::is_pointer_v<Tp>
UnboundedQueueBucket<Tp>::UnboundedQueueBucket(std::size_t n)
    : m_mutexes{static_cast<std::size_t>(std::bit_width(n))}
    , m_queues{static_cast<std::size_t>(std::bit_width(n))} {}


template <typename Tp>
    requires std::is_pointer_v<Tp>
void UnboundedQueueBucket<Tp>::push(Tp val) {
    std::uintptr_t const ptr = reinterpret_cast<std::uintptr_t>(val);
    std::size_t const size = m_queues.size();
    std::size_t const b = ((ptr >> 16) ^ (ptr >> 8)) % size;

    for (;;) {
        for (std::size_t curr_b = b; curr_b < size; ++curr_b) {
            auto& flag = m_mutexes[curr_b].flag;
            if (!flag.test_and_set(std::memory_order_acquire)) {
                m_queues[curr_b].push(val);
                flag.clear(std::memory_order_release);
                return;
            }
        }

        for (std::size_t curr_b = 0; curr_b < b; ++curr_b) {
            auto& flag = m_mutexes[curr_b].flag;
            if (!flag.test_and_set(std::memory_order_acquire)) {
                m_queues[curr_b].push(val);
                flag.clear(std::memory_order_release);
                return;
            }
        }

        std::this_thread::yield();
    }
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
template <std::random_access_iterator Iterator>
    requires std::convertible_to<std::iter_reference_t<Iterator>, Tp>
void UnboundedQueueBucket<Tp>::push(Iterator first, std::size_t n) {
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

