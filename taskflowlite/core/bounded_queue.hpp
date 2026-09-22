/// @file bounded_queue.hpp
/// @brief 单 Owner、多 Stealer 的有界工作窃取队列。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>

#include "utility.hpp"
#include "macros.hpp"

namespace tfl {

/// @brief 保存非拥有指针的固定容量单 Owner、多 Stealer 工作窃取队列。
///
/// 唯一 Owner 从尾部执行 push/pop，多个 Stealer 可并发从头部 steal；队列满时
/// 不扩容，由调用方处理溢出。索引查询只是并发近似快照，队列不管理指针目标的生命周期。
///
/// push 在写入槽位后通过 release bottom store 发布元素；Stealer acquire 读取
/// bottom 后即可观察对应槽位。pop/steal 保留 Chase-Lev 竞争协议中的 seq_cst
/// fence 和最后一个元素的 CAS。
///
/// @tparam Tp 存入槽位的指针类型。
/// @tparam cap 大于 1、可由 int64_t 表示的二次幂固定容量。
/// @warning push/pop 系列只能由同一个 Owner 线程调用，销毁时不得仍有并发访问。
/// @warning 索引采用 int64_t，队列使用期间的索引及索引运算不得溢出；不支持索引回绕。
/// @note nullptr 保留为 pop/steal 失败的返回值，不得作为有效元素入队。
/// @note 槽位保持原子类型，以允许尚未完成 CAS 的 Stealer 与 Owner 并发访问复用槽位。
/// @note 销毁队列不会销毁或释放指针指向的对象。
template <typename Tp, std::size_t cap = TFL_DEFAULT_QUEUE_SIZE>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
class BoundedQueue : public Immovable<BoundedQueue<Tp, cap>> {
public:
    using value_type = Tp;

    /// @brief 创建空队列，环形槽位和 top/bottom 索引全部零初始化。
    constexpr BoundedQueue() noexcept;

    ~BoundedQueue() noexcept = default;

    /// @brief 返回队列的固定容量。
    /// @return 模板参数 cap。
    [[nodiscard]] static constexpr std::size_t capacity() noexcept;

    /// @brief 返回队列当前元素数量的估计值。
    /// @note 分别读取 bottom/top，不构成一致快照，不能用于同步或保证后续操作成功。
    [[nodiscard]] std::size_t size() const noexcept;

    /// @brief 返回队列当前元素数量的估计值，int64_t 版本。
    /// @return max(bottom - top, 0)，不小于零。
    /// @note 分别读取 bottom/top，不构成一致快照，跨线程使用时仅供调度观察。
    [[nodiscard]] std::int64_t ssize() const noexcept;

    /// @brief 根据两次 relaxed 读取判断队列是否为空。
    /// @return 该近似快照中 top >= bottom 时返回 true。
    [[nodiscard]] bool empty() const noexcept;

    /// @brief 推入元素，队列满时调用溢出回调 on_failure(val)，仅 Owner 线程调用。
    ///
    /// 直接读取 top 判断剩余容量；队列已满时调用溢出回调，当前元素不入队。
    /// top 只用于容量判断，因此使用 relaxed 读取；旧值只会保守地低估可用容量。
    ///
    /// @tparam C 可调用类型 void(Tp)。
    /// @param val 要推入的非空指针。
    /// @param on_failure 接收未入队元素的溢出回调，函数不会保存它。
    template <typename C>
        requires (std::invocable<C&&, Tp> &&
                 std::same_as<std::invoke_result_t<C&&, Tp>, void>)
    void push(Tp val, C&& on_failure) noexcept(std::is_nothrow_invocable_v<C&&, Tp>);

    /// @brief 批量推入元素，容量不足时将剩余区间传递给溢出回调，仅 Owner 线程调用。
    ///
    /// n 为 0 时直接返回，不访问迭代器或调用回调。非空批次只发布可容纳的前缀；
    /// 先写入全部前缀槽位，再通过一次 release bottom store 发布。
    ///
    /// @tparam Iterator 随机访问迭代器，解引用结果可转为 Tp。
    /// @tparam C 可调用类型 void(Iterator, size_t)。
    /// @param first 起始迭代器。
    /// @param n 待推入总数。
    /// @param on_failure 接收剩余区间起始迭代器和数量的溢出回调。
    /// @pre 输入区间有效，元素转换后均非空，所用偏移可由迭代器差值类型表示。
    /// @pre 访问期间输入区间不被并发修改，迭代器操作及元素转换不得重入本队列的 Owner 接口。
    /// @note 槽位写入期间发生异常时，本批次尚未发布；发布后若剩余迭代器计算或回调抛异常，
    ///       已发布的前缀仍然保留，调用方不得把整个原始区间作为未提交任务直接重试。
    /// @note noexcept 同时考虑下标访问、元素转换、剩余迭代器计算和回调调用。
    template <std::random_access_iterator Iterator, typename C>
        requires (std::convertible_to<std::iter_reference_t<Iterator>, Tp> &&
                 std::invocable<C&&, Iterator, std::size_t> &&
                 std::same_as<std::invoke_result_t<C&&, Iterator, std::size_t>, void>)
    void push(Iterator first, std::size_t n, C&& on_failure) noexcept(
        noexcept(static_cast<Tp>(std::declval<Iterator&>()[std::declval<std::iter_difference_t<Iterator>>()])) &&
        noexcept(std::invoke(std::declval<C&&>(), std::declval<Iterator&>() + std::declval<std::iter_difference_t<Iterator>>(), std::declval<std::size_t>()))
        );

    /// @brief 从尾部 LIFO 弹出元素，仅 Owner 线程调用。
    ///
    /// 先递减 bottom 预占尾部位置，再通过 seq_cst fence 读取 top 判断是否存在元素。
    /// 最后一个元素由 Owner 与 Stealer 通过 top 的 seq_cst CAS 竞争取得。
    ///
    /// @return 弹出的元素；队列空或最后一个元素竞争失败时返回 nullptr。
    [[nodiscard]] Tp pop() noexcept;

    /// @brief 从头部 FIFO 窃取元素，允许多个 Stealer 并发调用。
    /// @return 窃取的元素；队列空或 CAS 竞争失败时返回 nullptr。
    /// @note CAS 失败不重试，调用方可继续尝试其他队列。
    [[nodiscard]] Tp steal() noexcept;

private:
    static_assert(cap <= (std::numeric_limits<std::int64_t>::max)(), "BoundedQueue capacity must fit in int64_t");
    static_assert(std::atomic<Tp>::is_always_lock_free, "BoundedQueue requires lock-free pointer atomics");
    static_assert(std::atomic<std::int64_t>::is_always_lock_free, "BoundedQueue requires lock-free 64-bit atomics");

    /// @brief 位掩码，将索引映射到环形缓冲区范围。
    static constexpr std::size_t k_mask = cap - 1;

    /// @brief 原子指针槽位，允许 Stealer 的推测读取与 Owner 的槽位复用并发发生。
    alignas(TFL_CACHE_LINE_SIZE) std::atomic<Tp> m_buf[cap];

    /// @brief 头部索引，由 Stealer 以及弹出最后一个元素的 Owner 通过 CAS 递增。
    alignas(TFL_CACHE_LINE_SIZE) std::atomic<std::int64_t> m_top{0};

    /// @brief 尾部索引，仅由 Owner 写入，与 top 分开对齐。
    alignas(TFL_CACHE_LINE_SIZE) std::atomic<std::int64_t> m_bottom{0};

};

// ============================================================================
// 实现部分
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
    return (std::max)(bottom - top, std::int64_t{0});
}

template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
bool BoundedQueue<Tp, cap>::empty() const noexcept {
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_relaxed);
    return top >= bottom;
}

// ============================================================================
// push(val, on_failure)：Owner 端带溢出回调的推入
// ============================================================================

template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
             template <typename C>
                 requires (std::invocable<C&&, Tp> &&
                          std::same_as<std::invoke_result_t<C&&, Tp>, void>)
void BoundedQueue<Tp, cap>::push(Tp val, C&& on_failure) noexcept(std::is_nothrow_invocable_v<C&&, Tp>) {
    TFL_ASSERT(val);

    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_acquire);

    if (bottom - top >= static_cast<std::int64_t>(cap)) [[unlikely]] {
        std::invoke(std::forward<C>(on_failure), val);
        return;
    }

    m_buf[static_cast<std::size_t>(bottom) & k_mask].store(val, std::memory_order_relaxed);
    m_bottom.store(bottom + 1, std::memory_order_release);
}

// ============================================================================
// push(first, n, on_failure)：Owner 端批量推入
// ============================================================================

template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
             template <std::random_access_iterator Iterator, typename C>
                 requires (std::convertible_to<std::iter_reference_t<Iterator>, Tp> &&
                          std::invocable<C&&, Iterator, std::size_t> &&
                          std::same_as<std::invoke_result_t<C&&, Iterator, std::size_t>, void>)
void BoundedQueue<Tp, cap>::push(Iterator first, std::size_t n, C&& on_failure) noexcept(
    noexcept(static_cast<Tp>(std::declval<Iterator&>()[std::declval<std::iter_difference_t<Iterator>>()])) &&
    noexcept(std::invoke(std::declval<C&&>(), std::declval<Iterator&>() + std::declval<std::iter_difference_t<Iterator>>(), std::declval<std::size_t>()))
    ) {
    if (n == 0) {
        return;
    }

    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_relaxed);
    std::int64_t const available = static_cast<std::int64_t>(cap) - (bottom - top);

    std::size_t const count = available > 0 ? (std::min)(n, static_cast<std::size_t>(available)) : 0;

    if (count != 0) {
        for (std::size_t i = 0; i < count; ++i) {
            Tp const val = static_cast<Tp>(first[static_cast<std::iter_difference_t<Iterator>>(i)]);
            TFL_ASSERT(val);
            m_buf[(static_cast<std::size_t>(bottom) + i) & k_mask].store(val, std::memory_order_relaxed);
        }

        m_bottom.store(bottom + static_cast<std::int64_t>(count), std::memory_order_release);
    }

    if (count < n) [[unlikely]] {
        std::invoke(std::forward<C>(on_failure), first + static_cast<std::iter_difference_t<Iterator>>(count), n - count);
    }
}

// ============================================================================
// pop：Owner 端 LIFO 弹出
// ============================================================================

template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
Tp BoundedQueue<Tp, cap>::pop() noexcept {
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed) - 1;
    m_bottom.store(bottom, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);

    std::int64_t top = m_top.load(std::memory_order_relaxed);

    if (top <= bottom) [[likely]] {
        Tp val = m_buf[static_cast<std::size_t>(bottom) & k_mask].load(std::memory_order_relaxed);

        if (top == bottom) [[unlikely]] {
            if (!m_top.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) [[unlikely]] {
                val = nullptr;
            }
            m_bottom.store(bottom + 1, std::memory_order_relaxed);
        }

        return val;
    }

    m_bottom.store(bottom + 1, std::memory_order_relaxed);
    return nullptr;
}

// ============================================================================
// steal：Stealer 端 FIFO 窃取
// ============================================================================

template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
Tp BoundedQueue<Tp, cap>::steal() noexcept {
    std::int64_t top = m_top.load(std::memory_order_acquire);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::int64_t const bottom = m_bottom.load(std::memory_order_acquire);

    if (top < bottom) {
        Tp const val = m_buf[static_cast<std::size_t>(top) & k_mask].load(std::memory_order_relaxed);

        if (m_top.compare_exchange_strong(top, top + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) [[likely]] {
            return val;
        }
    }

    return nullptr;
}

} // namespace tfl
