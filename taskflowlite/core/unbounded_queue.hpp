/// @file unbounded_queue.hpp
/// @brief 提供可动态增长的无界环形队列，用于任务分发。
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

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

/// @brief 原子环形缓冲区底层容器
///
/// @details 定长容器，容量为 2 的幂次方。负责存储数据，不管理头尾指针。
/// 提供在指定位置存取数据的能力，以及数据迁移到更大缓冲区的功能。
///
/// @tparam Tp 元素类型（必须为指针类型）
template <typename Tp>
    requires std::is_pointer_v<Tp>
class AtomicRingBuffer : public Immovable<AtomicRingBuffer<Tp>> {
public:
    /// @brief 构造函数，创建指定容量的缓冲区
    /// @param cap 初始容量
    explicit AtomicRingBuffer(std::int64_t cap);

    ~AtomicRingBuffer();

    AtomicRingBuffer(const AtomicRingBuffer&) = delete;
    AtomicRingBuffer& operator=(const AtomicRingBuffer&) = delete;
    AtomicRingBuffer(AtomicRingBuffer&&) = delete;
    AtomicRingBuffer& operator=(AtomicRingBuffer&&) = delete;

    /// @brief 返回缓冲区容量
    /// @return 最大容量
    [[nodiscard]] TFL_FORCE_INLINE std::int64_t capacity() const noexcept;

    /// @brief 在指定索引位置存储元素（自动循环取模）
    /// @param index 索引（可为负数或超过容量）
    /// @param val 待存储的元素
    TFL_FORCE_INLINE void store(std::int64_t index, Tp val) noexcept;

    /// @brief 从指定索引位置加载元素
    /// @param index 索引
    /// @return 元素值
    [[nodiscard]] TFL_FORCE_INLINE Tp load(std::int64_t index) const noexcept;

    /// @brief 扩容：创建容量翻倍的新缓冲区并迁移数据
    /// @param bottom 当前队尾位置
    /// @param top 当前队头位置
    /// @return 新缓冲区指针
    [[nodiscard]] TFL_FORCE_INLINE AtomicRingBuffer* resize(std::int64_t bottom, std::int64_t top) const;

    /// @brief 扩容：创建足够容纳额外元素的新缓冲区
    /// @param bottom 当前队尾位置
    /// @param top 当前队头位置
    /// @param n 额外需要容纳的元素数量
    /// @return 新缓冲区指针
    [[nodiscard]] TFL_FORCE_INLINE AtomicRingBuffer* resize(std::int64_t bottom, std::int64_t top, std::size_t n) const;

private:
    std::int64_t m_cap;    ///< 实际容量（2 的倍数）
    std::int64_t m_mask;   ///< 位掩码（容量 - 1），用于高效取模
    std::atomic<Tp>* m_buf; ///< 存储数组
};


/// @brief 可自动扩容的无锁工作窃取队列
///
/// @details 与 BoundedQueue 类似：Owner 线程在尾部 push/pop，Stealer 线程在头部 steal。
/// 区别：当 Owner 发现队列满时，自动扩容创建更大的缓冲区并迁移数据。
///
/// @tparam Tp 元素类型（必须为指针类型）
template <typename Tp>
    requires std::is_pointer_v<Tp>
class UnboundedQueue : public Immovable<UnboundedQueue<Tp>> {
public:
    using value_type = Tp;

    /// @brief 构造函数，创建指定初始容量的队列
    /// @param cap 初始容量（默认 2 倍默认队列大小）
    explicit UnboundedQueue(std::int64_t cap = 2 * TFL_DEFAULT_QUEUE_SIZE);

    /// @brief 析构函数，释放当前缓冲区和历史缓冲区
    ~UnboundedQueue() noexcept;

    /// @brief 返回队列中的近似元素数量
    /// @return 元素数量（近似值）
    [[nodiscard]] std::size_t size() const noexcept;

    /// @brief 返回队列中的可能带符号的元素数量
    /// @return 队列大小（可能为负数，表示并发冲突）
    [[nodiscard]] std::int64_t ssize() const noexcept;

    /// @brief 返回当前缓冲区的最大容量
    /// @return 容量
    [[nodiscard]] std::int64_t capacity() const noexcept;

    /// @brief 检查队列是否为空
    /// @return 空返回 true
    [[nodiscard]] bool empty() const noexcept;

    /// @brief 将元素推入队列尾部，容量不足时自动扩容
    /// @param val 待推送元素
    void push(Tp val);

    /// @brief 批量将元素推入队列尾部
    /// @param first 元素范围起始迭代器
    /// @param n 元素数量
    template <std::random_access_iterator Iterator>
        requires std::convertible_to<std::iter_reference_t<Iterator>, Tp>
    void push(Iterator first, std::size_t n);

    /// @brief 从队列尾部弹出一个元素
    /// @return 成功弹出返回元素，队列空返回 nullptr
    /// @note 仅 Owner 线程可调用
    [[nodiscard]] Tp pop() noexcept;

    /// @brief 从队列头部窃取元素
    /// @return 成功窃取返回元素，队列空返回 nullptr
    /// @note 供其他线程调用，可能因并发冲突重试
    [[nodiscard]] Tp steal() noexcept;

    /// @brief 从队列头部窃取元素并统计空窃取次数
    /// @param num_empty_steals 输出参数，记录连续空窃取次数
    /// @return 成功窃取返回元素，队列空返回 nullptr
    [[nodiscard]] Tp steal(std::size_t& num_empty_steals) noexcept;

private:
    static constexpr std::size_t k_garbage_reserve = 64;

    // Why: 使用 2 倍缓存行大小对齐，防止伪共享
    alignas(2 * std::hardware_destructive_interference_size) std::atomic<std::int64_t> m_top;
    alignas(2 * std::hardware_destructive_interference_size) std::atomic<std::int64_t> m_bottom;

    // Why: 使用原子指针因为缓冲区可能被替换
    std::atomic<AtomicRingBuffer<Tp>*> m_buf;

    // Why: 垃圾回收机制
    // 扩容时旧缓冲区不能立即释放，因为可能正有 Stealer 线程读取
    // 将旧缓冲区存入垃圾向量，等待析构时统一释放
    std::vector<std::unique_ptr<AtomicRingBuffer<Tp>>> m_garbage;
};

// ============================================================================
// AtomicRingBuffer Implementation
// ============================================================================

template <typename Tp>
    requires std::is_pointer_v<Tp>
AtomicRingBuffer<Tp>::AtomicRingBuffer(std::int64_t cap)
    // Why: std::bit_ceil 自动将容量向上对齐到 2 的幂次方
    // 例如传入 100 会得到 128，使后续可用位运算 & m_mask 替代 % 取模
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
    // 确保新容量为 2 的幂次方且足够容纳现有元素 + 新增元素
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
    // m_garbage 为 vector<unique_ptr>，析构时自动释放所有旧缓冲区
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

    // 容量不足，触发扩容
    if (buf->capacity() < (bottom - top) + 1) [[unlikely]] {
        auto* bigger = buf->resize(bottom, top);

        // Why: 使用 lambda 和 std::exchange 原子地替换缓冲区
        // 旧缓冲区自动移入垃圾向量
        [&]() noexcept {
            m_garbage.emplace_back(std::exchange(buf, bigger));
        }();

        // 通知其他线程缓冲区已更换
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

    // 计算所需额外容量，容量不足时扩容
    if (shortage > 0) [[unlikely]] {
        auto* bigger = buf->resize(bottom, top, static_cast<std::size_t>(shortage));
        [&]() noexcept {
            m_garbage.emplace_back(std::exchange(buf, bigger));
        }();
        m_buf.store(bigger, std::memory_order_release);
    }

    // 批量存储元素
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

    // Owner 预占一个位置
    m_bottom.store(bottom, std::memory_order_relaxed);

    // Why: seq_cst 内存屏障确保指令顺序
    // 防止 CPU 重排导致先读取 top 再修改 bottom，引发竞争条件
    std::atomic_thread_fence(std::memory_order_seq_cst);

    std::int64_t top = m_top.load(std::memory_order_relaxed);

    if (top <= bottom) {
        Tp val = buf->load(bottom);

        // 队列仅剩一个元素，可能与 Stealer 竞争
        if (top == bottom) {
            // 使用 CAS 尝试原子增加 top
            if (!m_top.compare_exchange_strong(top, top + 1,
                                               std::memory_order_seq_cst, std::memory_order_relaxed)) {
                // 竞争失败，还原 bottom 并返回空
                m_bottom.store(bottom + 1, std::memory_order_relaxed);
                return nullptr;
            }
            // 成功获取，还原 bottom
            m_bottom.store(bottom + 1, std::memory_order_relaxed);
        }
        return val;
    }

    // 队列为空，还原 bottom
    m_bottom.store(bottom + 1, std::memory_order_relaxed);
    return nullptr;
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
Tp UnboundedQueue<Tp>::steal() noexcept {
    std::int64_t top = m_top.load(std::memory_order_acquire);

    // Why: Stealer 需要循环因为可能在扩容时失败或与其他 Stealer 竞争
    for (;;) {
        // seq_cst 屏障确保先看到 top 再读取 bottom
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::int64_t const bottom = m_bottom.load(std::memory_order_acquire);

        if (top < bottom) {
            Tp val = m_buf.load(std::memory_order_acquire)->load(top);

            // 竞争：使用 compare_exchange_weak 尝试增加 top
            // weak 版本可能失败但更快，适合循环
            if (m_top.compare_exchange_weak(top, top + 1,
                                            std::memory_order_seq_cst, std::memory_order_relaxed)) {
                return val;
            }
        } else {
            // 队列空
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
            num_empty_steals = 0; // 有元素可窃取，重置计数
            Tp val = m_buf.load(std::memory_order_acquire)->load(top);

            if (m_top.compare_exchange_weak(top, top + 1,
                                            std::memory_order_seq_cst, std::memory_order_relaxed)) {
                return val;
            }
        } else {
            // 队列空，增加空窃取计数
            ++num_empty_steals;
            return nullptr;
        }
    }
}

} // namespace tfl
