#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>

#include "utility.hpp"
#include "macros.hpp"
namespace tfl {

template <typename Tp>
    requires std::is_pointer_v<Tp>
class AtomicRingBuffer : public Immovable<AtomicRingBuffer<Tp>> {
public:
    explicit AtomicRingBuffer(std::int64_t cap);

    ~AtomicRingBuffer();

    AtomicRingBuffer(const AtomicRingBuffer&) = delete;
    AtomicRingBuffer& operator=(const AtomicRingBuffer&) = delete;
    AtomicRingBuffer(AtomicRingBuffer&&) = delete;
    AtomicRingBuffer& operator=(AtomicRingBuffer&&) = delete;

    [[nodiscard]] TFL_FORCE_INLINE std::int64_t capacity() const noexcept;

    TFL_FORCE_INLINE void store(std::int64_t index, Tp val) noexcept;

    [[nodiscard]] TFL_FORCE_INLINE Tp load(std::int64_t index) const noexcept;

    // 默认扩容 2 倍
    [[nodiscard]] TFL_FORCE_INLINE AtomicRingBuffer* resize(std::int64_t bottom, std::int64_t top) const;

    // 扩容到能容纳额外 n 个元素
    [[nodiscard]] TFL_FORCE_INLINE AtomicRingBuffer* resize(std::int64_t bottom, std::int64_t top, std::size_t n) const;
private:
    std::int64_t m_cap;
    std::int64_t m_mask;
    std::atomic<Tp>* m_buf;
};

template <typename Tp>
    requires std::is_pointer_v<Tp>
class UnboundedQueue : public Immovable<UnboundedQueue<Tp>> {
public:
    using value_type = Tp;

    explicit UnboundedQueue(std::int64_t cap = 2 * TFL_DEFAULT_QUEUE_SIZE);

    ~UnboundedQueue() noexcept;

    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] std::int64_t ssize() const noexcept;

    [[nodiscard]] std::int64_t capacity() const noexcept;

    [[nodiscard]] bool empty() const noexcept;

    void push(Tp val);

    template <std::random_access_iterator Iterator>
        requires std::convertible_to<std::iter_reference_t<Iterator>, Tp>
    void push(Iterator first, std::size_t n);

    [[nodiscard]] Tp pop() noexcept;

    [[nodiscard]] Tp steal() noexcept;

    [[nodiscard]] Tp steal(std::size_t& num_empty_steals) noexcept;

private:
    static constexpr std::size_t k_garbage_reserve = 64;

    alignas(2 * std::hardware_destructive_interference_size) std::atomic<std::int64_t> m_top;
    alignas(2 * std::hardware_destructive_interference_size) std::atomic<std::int64_t> m_bottom;
    std::atomic<AtomicRingBuffer<Tp>*> m_buf;
    std::vector<std::unique_ptr<AtomicRingBuffer<Tp>>> m_garbage;
};

// ============================================================================
// AtomicRingBuffer Implementation
// ============================================================================

template <typename Tp>
    requires std::is_pointer_v<Tp>
AtomicRingBuffer<Tp>::AtomicRingBuffer(std::int64_t cap)
    : m_cap{static_cast<std::int64_t>(
          std::max<std::size_t>(2, std::bit_ceil(static_cast<std::size_t>(cap))))}
    , m_mask{m_cap - 1}
    , m_buf{new std::atomic<Tp>[static_cast<std::size_t>(m_cap)]} {}

template <typename Tp>
    requires std::is_pointer_v<Tp>
AtomicRingBuffer<Tp>::~AtomicRingBuffer() {
    delete[] m_buf;
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
TFL_FORCE_INLINE std::int64_t AtomicRingBuffer<Tp>::capacity() const noexcept {
    return m_cap;
}


template <typename Tp>
    requires std::is_pointer_v<Tp>
TFL_FORCE_INLINE void AtomicRingBuffer<Tp>::store(std::int64_t index, Tp val) noexcept {
    m_buf[static_cast<std::size_t>(index & m_mask)].store(val, std::memory_order_relaxed);
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
TFL_FORCE_INLINE Tp AtomicRingBuffer<Tp>::load(std::int64_t index) const noexcept {
    return m_buf[static_cast<std::size_t>(index & m_mask)].load(std::memory_order_relaxed);
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
TFL_FORCE_INLINE AtomicRingBuffer<Tp>* AtomicRingBuffer<Tp>::resize(
    std::int64_t bottom, std::int64_t top) const {
    auto* ptr = new (std::nothrow) AtomicRingBuffer{2 * m_cap};
    for (std::int64_t i = top; i != bottom; ++i) {
        ptr->store(i, load(i));
    }
    return ptr;

}

template <typename Tp>
    requires std::is_pointer_v<Tp>
TFL_FORCE_INLINE AtomicRingBuffer<Tp>* AtomicRingBuffer<Tp>::resize(
    std::int64_t bottom, std::int64_t top, std::size_t n) const {
    // 确保新容量是 2 的幂次，且足够容纳现有元素 + n 个新元素
    std::int64_t const new_cap = std::bit_ceil(m_cap + n);
    auto* ptr = new AtomicRingBuffer{new_cap};
    for (std::int64_t i = top; i != bottom; ++i) {
        ptr->store(i, load(i));
    }
    return ptr;
}

// ============================================================================
// UnboundedQueue Implementation
// ============================================================================
template <typename Tp>
    requires std::is_pointer_v<Tp>
UnboundedQueue<Tp>::UnboundedQueue(std::int64_t cap)
    : m_top{0}
    , m_bottom{0}
    , m_buf{new AtomicRingBuffer<Tp>{cap}} {
    m_garbage.reserve(k_garbage_reserve);
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
UnboundedQueue<Tp>::~UnboundedQueue() noexcept {
    delete m_buf.load(std::memory_order_relaxed);
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
std::size_t UnboundedQueue<Tp>::size() const noexcept {
    return static_cast<std::size_t>(ssize());
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
std::int64_t UnboundedQueue<Tp>::ssize() const noexcept {
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_relaxed);
    return std::max(bottom - top, std::int64_t{0});
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
std::int64_t UnboundedQueue<Tp>::capacity() const noexcept {
    return m_buf.load(std::memory_order_relaxed)->capacity();
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
bool UnboundedQueue<Tp>::empty() const noexcept {
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_relaxed);
    return top >= bottom;
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
void UnboundedQueue<Tp>::push(Tp val) {
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_acquire);
    auto* buf = m_buf.load(std::memory_order_relaxed);

    if (buf->capacity() < (bottom - top) + 1) [[unlikely]] {
        auto* bigger = buf->resize(bottom, top);
        [&]() noexcept {
            m_garbage.emplace_back(std::exchange(buf, bigger));
        }();
        m_buf.store(bigger, std::memory_order_release);
    }

    buf->store(bottom, val);
    m_bottom.store(bottom + 1, std::memory_order_release);
}


template <typename Tp>
    requires std::is_pointer_v<Tp>
template <std::random_access_iterator Iterator>
    requires std::convertible_to<std::iter_reference_t<Iterator>, Tp>
void UnboundedQueue<Tp>::push(Iterator first, std::size_t n) {
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_acquire);
    auto* buf = m_buf.load(std::memory_order_relaxed);

    std::int64_t const shortage = bottom - top + static_cast<std::int64_t>(n) - buf->capacity();

    if (shortage > 0) [[unlikely]] {
        auto* bigger = buf->resize(bottom, top, static_cast<std::size_t>(shortage));
        [&]() noexcept {
            m_garbage.emplace_back(std::exchange(buf, bigger));
        }();
        m_buf.store(bigger, std::memory_order_release);
    }

    for (std::size_t i = 0; i < n; ++i) {
        buf->store(bottom + static_cast<std::int64_t>(i), static_cast<Tp>(first[i]));
    }

    m_bottom.store(bottom + static_cast<std::int64_t>(n), std::memory_order_release);
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
Tp UnboundedQueue<Tp>::pop() noexcept {
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed) - 1;
    AtomicRingBuffer<Tp>* buf = m_buf.load(std::memory_order_relaxed);
    m_bottom.store(bottom, std::memory_order_relaxed);

    std::atomic_thread_fence(std::memory_order_seq_cst);

    std::int64_t top = m_top.load(std::memory_order_relaxed);

    if (top <= bottom) {
        Tp val = buf->load(bottom);

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

template <typename Tp>
    requires std::is_pointer_v<Tp>
Tp UnboundedQueue<Tp>::steal() noexcept {
    std::int64_t top = m_top.load(std::memory_order_acquire);
    for (;;) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::int64_t const bottom = m_bottom.load(std::memory_order_acquire);

        if (top < bottom) {
            Tp val = m_buf.load(std::memory_order_acquire)->load(top);

            if (m_top.compare_exchange_weak(top, top + 1,
                                            std::memory_order_seq_cst, std::memory_order_relaxed)) {
                return val;
            }
        } else {
            return nullptr;
        }
    }
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
Tp UnboundedQueue<Tp>::steal(std::size_t& num_empty_steals) noexcept {
    std::int64_t top = m_top.load(std::memory_order_acquire);

    for (;;) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::int64_t const bottom = m_bottom.load(std::memory_order_acquire);

        if (top < bottom) {
            num_empty_steals = 0;
            Tp val = m_buf.load(std::memory_order_acquire)->load(top);

            if (m_top.compare_exchange_weak(top, top + 1,
                                            std::memory_order_seq_cst, std::memory_order_relaxed)) {
                return val;
            }
        } else {
            ++num_empty_steals;
            return nullptr;
        }
    }
}

} // namespace tfl
