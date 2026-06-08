/// @file  bounded_queue.hpp
/// @brief Chase-Lev 无锁有界双端队列 —— Worker 本地任务队列的核心数据结构。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

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

/// @brief Chase-Lev 无锁有界双端队列 —— Owner LIFO 推入/弹出，Stealer FIFO 窃取。
///
/// Owner 单线程 push/pop，Stealer 多线程 steal。通过 acquire/release 配对
/// 与单元素 seq_cst CAS 保证无锁安全。容量编译期固定为 2 的幂。
/// @tparam Tp  必须为指针类型
/// @tparam cap 容量，必须为 2 的幂且 > 1
template <typename Tp, std::size_t cap = TFL_DEFAULT_QUEUE_SIZE>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
class BoundedQueue : public Immovable<BoundedQueue<Tp, cap>> {
public:
    /// @brief 元素类型别名。
    using value_type = Tp;

    /// @brief 创建空队列，环形 buffer 槽位和 top/bottom 索引全部零初始化。
    constexpr BoundedQueue() noexcept;

    /// @brief 析构函数（默认）。
    ~BoundedQueue() noexcept = default;

    /// @brief 返回编译期容量 2 的幂值。
    /// @return 模板参数 cap，确保始终为 2 的幂。
    [[nodiscard]] static constexpr std::size_t capacity() noexcept;

    /// @brief 队列当前元素数量的估计值（relaxed 快照，非原子精确值）。
    [[nodiscard]] std::size_t size() const noexcept;

    /// @brief 返回队列中元素数量的估计值，int64_t 版本。
    ///
    /// relaxed 读取 bottom 和 top 获得瞬时快照，跨线程使用时仅为近似值。
    ///
    /// @return max(bottom - top, 0)，不小于零。
    [[nodiscard]] std::int64_t ssize() const noexcept;

    /// @brief 判断队列是否为空，top >= bottom 时无元素可消费。
    /// @return true 若所有已推入元素均已被消费。
    [[nodiscard]] bool empty() const noexcept;

    /// @brief 尝试非阻塞推入（仅 Owner 线程调用，single-producer）。
    ///
    /// 队列满时返回 false，不阻塞，不调用溢出回调。
    ///
    /// @memory_order
    /// - top: acquire load —— 必须看到 stealer 的最新进度；
    /// - buf[idx]: relaxed store —— 此时尚未对 stealer 可见；
    /// - bottom: release store —— 元素发布点，synchronizes-with stealer 的 acquire。
    [[nodiscard]] bool try_push(Tp val) noexcept;

    /// @brief 推入元素，队列满时调用溢出回调 on_full()（仅 Owner 线程）。
    ///
    /// 典型实现：将溢出元素推入全局 UnboundedQueue。
    ///
    /// @tparam C  可调用类型，`void()` 签名
    /// @param on_full  队列满时的回调
    template <typename C>
        requires (std::invocable<C&&> && std::same_as<std::invoke_result_t<C&&>, void>)
    void push(Tp val, C&& on_full);

    /// @brief 批量推入元素，容量不足时剩余部分传递给溢出回调（仅 Owner 线程）。
    ///
    /// 一次 release store 发布所有已写入元素以减少原子操作次数。
    ///
    /// @tparam Iterator  随机访问迭代器，解引用结果可转为 Tp
    /// @tparam C         可调用类型 `void(Iterator, size_t)`
    /// @param first      起始迭代器
    /// @param n          待推入总数
    /// @param on_full    溢出回调，接收剩余元素的起始迭代器和数量
    template <std::random_access_iterator Iterator, typename C>
        requires (std::convertible_to<std::iter_reference_t<Iterator>, Tp> &&
                 std::invocable<C&&, Iterator, std::size_t> &&
                 std::same_as<std::invoke_result_t<C&&, Iterator, std::size_t>, void>)
    void push(Iterator first, std::size_t n, C&& on_full);

    /// @brief 从尾部 LIFO 弹出元素（仅 Owner 线程，single-consumer）。
    ///
    /// 算法：
    /// 1. 预占位置 bottom = bottom - 1
    /// 2. seq_cst fence 确保 bottom 写在 top 读之前完成
    /// 3. 若 top <= bottom，读取 slot 元素
    /// 4. 若 top == bottom（最后一个元素），seq_cst CAS 与 stealer 竞争
    ///
    /// @return 弹出元素的指针，队列空或竞争失败返回 nullptr。
    ///
    /// @warning 竞争失败返回 nullptr 是正常行为（非错误），调用方应重新检查队列。
    [[nodiscard]] Tp pop() noexcept;

    /// @brief 从头部 FIFO 窃取元素（任意线程，multi-consumer）。
    ///
    /// CAS 失败不重试以避免 cache line 风暴。
    ///
    /// @return 窃取的元素，队列空或 CAS 竞争失败返回 nullptr。
    [[nodiscard]] Tp steal() noexcept;

    /// @brief 窃取元素并统计连续空窃取次数。
    ///
    /// 成功窃取时重置 @p num_empty_steals 为 0，空窃取时递增。
    /// 上层调度器使用此计数决定退避策略（yield 频率 / sleep 时长）。
    ///
    /// @param num_empty_steals [in,out] 连续空窃取计数器
        [[nodiscard]] Tp steal(std::size_t& num_empty_steals) noexcept;

private:
    /// @brief 位掩码 = cap - 1，将索引映射到缓冲区范围。
    static constexpr std::size_t k_mask = cap - 1;

    /// @brief 环形缓冲区。
    ///
    /// 元素为 std::atomic<Tp>。alignas(2 * cache_line_size) 将 buffer
    /// 与 bottom/top 所在 cache line 物理隔离，消除伪共享。
    alignas(2 * cache_line_size) std::atomic<Tp> m_buf[cap];

    /// @brief 头部索引 —— 仅 Stealer 通过 CAS 写入。
    ///
    /// 独占 2x cache line 对齐区域，与 m_bottom 物理隔离。
    alignas(2 * cache_line_size) std::atomic<std::int64_t> m_top{0};

    /// @brief 尾部索引 —— 仅 Owner 线程写入。
    ///
    /// Owner 用 release store 写入新 bottom 值作为"元素发布"的同步点。
    alignas(2 * cache_line_size) std::atomic<std::int64_t> m_bottom{0};
};

// ---------------------------------------------------------------------------
// 实现部分
// ---------------------------------------------------------------------------

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
    // relaxed 读取 bottom/top 获得的是一致性快照，非线性化精确值
    // 在 owner 线程中调用时 bottom 是最新的（该线程独占修改），
    // 跨线程使用时仅作容量估计参考
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_relaxed);
    return std::max(bottom - top, std::int64_t{0});
}

template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
bool BoundedQueue<Tp, cap>::empty() const noexcept {
    // top >= bottom 等价于队列为空 —— 所有已 push 元素均被消费
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_relaxed);
    return top >= bottom;
}

// ---------------------------------------------------------------------------
// try_push: Owner 端非阻塞推入
// ---------------------------------------------------------------------------
template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
bool BoundedQueue<Tp, cap>::try_push(Tp val) noexcept {
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    // acquire: 必须看到 stealer 的最新 top，否则可能错判为"未满"而覆盖未读元素
    std::int64_t const top = m_top.load(std::memory_order_acquire);

    // (bottom - top) 为当前队列中元素数，+1 为推入后元素数
    // 若 > cap 则队列已满，不可推入
    if (static_cast<std::int64_t>(cap) < (bottom - top) + 1) [[unlikely]] {
        return false;
    }

    // relaxed: 元素写入 buffer 此时仅 owner 可见
    m_buf[static_cast<std::size_t>(bottom) & k_mask].store(val, std::memory_order_relaxed);
    // release: 发布点 —— 确保 buffer 写入先于 bottom 更新对所有线程可见
    // synchronizes-with: stealer 的 acquire load(m_top)
    m_bottom.store(bottom + 1, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// push(val, on_full): Owner 端带溢出回调的推入
// ---------------------------------------------------------------------------
template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
             template <typename C>
                 requires (std::invocable<C&&> && std::same_as<std::invoke_result_t<C&&>, void>)
void BoundedQueue<Tp, cap>::push(Tp val, C&& on_full) {
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed);
    std::int64_t const top = m_top.load(std::memory_order_acquire);

    if (static_cast<std::int64_t>(cap) < (bottom - top) + 1) [[unlikely]] {
        // 队列满：将元素交给溢出回调（通常推入全局 UnboundedQueue）
        std::invoke(on_full);
        return;
    }

    m_buf[static_cast<std::size_t>(bottom) & k_mask].store(val, std::memory_order_relaxed);
    m_bottom.store(bottom + 1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// push(first, n, on_full): Owner 端批量推入
// ---------------------------------------------------------------------------
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

    // 批量写入：所有 relaxed store，在最后一次性 release publish
    for (std::size_t i = 0; i < count; ++i) {
        m_buf[static_cast<std::size_t>(bottom + i) & k_mask].store(
            static_cast<Tp>(first[i]), std::memory_order_relaxed);
    }

    // 一次性 release: 所有 count 个元素同时对 stealer 可见
    m_bottom.store(bottom + static_cast<std::int64_t>(count), std::memory_order_release);

    // 容量不足以容纳全部 n 个，剩余部分交给溢出回调
    if (count < n) [[unlikely]] {
        std::invoke(on_full, first + count, n - count);
    }
}

#if 0

// ---------------------------------------------------------------------------
// pop: Owner 端 LIFO 弹出
// ---------------------------------------------------------------------------
template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
Tp BoundedQueue<Tp, cap>::pop() noexcept {
    return steal();
}

// ---------------------------------------------------------------------------
// steal: Stealer 端 FIFO 窃取
// ---------------------------------------------------------------------------
template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
Tp BoundedQueue<Tp, cap>::steal() noexcept {
    std::int64_t top = m_top.load(std::memory_order_relaxed);
    std::int64_t const bottom = m_bottom.load(std::memory_order_acquire);

    Tp val{nullptr};
    if (top < bottom) [[likely]] {
        // relaxed: 该 slot 此时尚未被"认领"，多 stealer 均可能读取相同元素
        val = m_buf[static_cast<std::size_t>(top) & k_mask].load(std::memory_order_relaxed);

        // seq_cst CAS: 多 stealer 竞争递增 top
        // 成功 (acq_rel): 该 stealer 获得元素所有权
        // 失败 (relaxed): 其他 stealer 先一步 —— 返回 nullptr，不重试
        if (!m_top.compare_exchange_strong(top, top + 1,
                                           std::memory_order_acq_rel, std::memory_order_relaxed)) [[unlikely]] {
            return nullptr;
        }
    }

    return val;
}

// ---------------------------------------------------------------------------
// steal(num_empty_steals): 窃取 + 空窃取计数
// ---------------------------------------------------------------------------
template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
Tp BoundedQueue<Tp, cap>::steal(std::size_t& num_empty_steals) noexcept {
    std::int64_t top = m_top.load(std::memory_order_relaxed);
    std::int64_t const bottom = m_bottom.load(std::memory_order_acquire);

    Tp val{nullptr};
    if (top < bottom) [[likely]] {
        // 看到队列中有元素，重置空窃取计数器
        num_empty_steals = 0;
        val = m_buf[static_cast<std::size_t>(top) & k_mask].load(std::memory_order_relaxed);

        if (!m_top.compare_exchange_strong(top, top + 1,
                                           std::memory_order_acq_rel, std::memory_order_relaxed)) [[unlikely]] {
            return nullptr;
        }
    } else {
        // 队列为空：递增计数器，上层调度器据此调整退避策略
        ++num_empty_steals;
    }

    return val;
}
#else
// ---------------------------------------------------------------------------
// pop: Owner 端 LIFO 弹出
// ---------------------------------------------------------------------------
template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
Tp BoundedQueue<Tp, cap>::pop() noexcept {
    // 预占一个位置：将 bottom 减 1，在确认可弹出之前预留 slot
    std::int64_t const bottom = m_bottom.load(std::memory_order_relaxed) - 1;
    m_bottom.store(bottom, std::memory_order_relaxed);

    // seq_cst fence: 强制 bottom 写操作先于下方 top 读操作完成
    // 若没有这个屏障，CPU 可能将 top 读取重排到 bottom 变更之前，
    // 导致在"看起来有元素"时读到旧元素的 race
    std::atomic_thread_fence(std::memory_order_seq_cst);

    std::int64_t top = m_top.load(std::memory_order_relaxed);

    if (top <= bottom) [[likely]] {
        // relaxed: owner 独占访问此 slot，stealer 不会碰（除非是最后一个元素）
        Tp val = m_buf[static_cast<std::size_t>(bottom) & k_mask].load(std::memory_order_relaxed);

        // 最后一个元素：owner 与 stealer 可能同时操作
        if (top == bottom) [[unlikely]] {
            // seq_cst CAS: 尝试原子地将 top 从 top 改为 top + 1
            // - 成功 (acq_rel): owner 获得元素所有权
            // - 失败 (relaxed): stealer 已抢先窃取，放弃并还原 bottom
            if (!m_top.compare_exchange_strong(top, top + 1,
                                               std::memory_order_seq_cst, std::memory_order_relaxed)) [[unlikely]] {
                // 还原 bottom: stealer 已拿走元素
                m_bottom.store(bottom + 1, std::memory_order_relaxed);
                return nullptr;
            }
            // CAS 成功: 还原 bottom 为 top+1（队列空的状态）
            m_bottom.store(bottom + 1, std::memory_order_relaxed);
        }
        return val;
    }

    // 队列为空: 还原 bottom 到原始值
    m_bottom.store(bottom + 1, std::memory_order_relaxed);
    return nullptr;
}

// ---------------------------------------------------------------------------
// steal: Stealer 端 FIFO 窃取
// ---------------------------------------------------------------------------
template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
Tp BoundedQueue<Tp, cap>::steal() noexcept {
    std::int64_t top = m_top.load(std::memory_order_relaxed);
    // seq_cst fence: 强制 top 读取先于 bottom 读取完成
    // 防止 CPU 将 bottom 的加载重排到 top 加载之前 —— 若先看到新 bottom
    // 再看到旧 top，会误判队列有元素而读到垃圾数据
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::int64_t const bottom = m_bottom.load(std::memory_order_acquire);

    Tp val{nullptr};
    if (top < bottom) [[likely]] {
        // relaxed: 该 slot 此时尚未被"认领"，多 stealer 均可能读取相同元素
        val = m_buf[static_cast<std::size_t>(top) & k_mask].load(std::memory_order_relaxed);

        // seq_cst CAS: 多 stealer 竞争递增 top
        // 成功 (acq_rel): 该 stealer 获得元素所有权
        // 失败 (relaxed): 其他 stealer 先一步 —— 返回 nullptr，不重试
        if (!m_top.compare_exchange_strong(top, top + 1,
                                           std::memory_order_seq_cst, std::memory_order_relaxed)) [[unlikely]] {
            return nullptr;
        }
    }

    return val;
}

// ---------------------------------------------------------------------------
// steal(num_empty_steals): 窃取 + 空窃取计数
// ---------------------------------------------------------------------------
template <typename Tp, std::size_t cap>
    requires std::is_pointer_v<Tp> && (cap > 1) && ((cap & (cap - 1)) == 0)
Tp BoundedQueue<Tp, cap>::steal(std::size_t& num_empty_steals) noexcept {
    std::int64_t top = m_top.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::int64_t const bottom = m_bottom.load(std::memory_order_acquire);

    Tp val{nullptr};
    if (top < bottom) [[likely]] {
        // 看到队列中有元素，重置空窃取计数器
        num_empty_steals = 0;
        val = m_buf[static_cast<std::size_t>(top) & k_mask].load(std::memory_order_relaxed);

        if (!m_top.compare_exchange_strong(top, top + 1,
                                           std::memory_order_seq_cst, std::memory_order_relaxed)) [[unlikely]] {
            return nullptr;
        }
    } else {
        // 队列为空：递增计数器，上层调度器据此调整退避策略
        ++num_empty_steals;
    }

    return val;
}
#endif

} // namespace tfl
