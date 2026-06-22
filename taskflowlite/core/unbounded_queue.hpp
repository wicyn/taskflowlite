/// @file  unbounded_queue.hpp
/// @brief 无锁无界双端队列 —— 运行期自适应扩容的 Chase-Lev 变体。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

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

/// @brief 原子环形缓冲区 —— UnboundedQueue 的底层定长存储。
///
/// 容量 2 的幂，用位掩码替代取模。关键能力是 resize(bottom, top) 分配 2x 新 buffer
/// 拷贝 [top, bottom) 区间。旧 buffer 不立即释放（stealer 可能还在读），由
/// UnboundedQueue 在析构时统一清理。
/// @tparam Tp 必须为指针类型
template <typename Tp>
    requires std::is_pointer_v<Tp>
class AtomicRingBuffer : public Immovable<AtomicRingBuffer<Tp>> {
public:
    /// @brief 构造函数，创建指定容量的缓冲区
    /// @param cap 初始容量
    explicit AtomicRingBuffer(std::int64_t cap);

    /// @brief 析构时释放内部环形缓冲区内存。
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
    [[nodiscard]] TFL_FORCE_INLINE AtomicRingBuffer* resize(std::int64_t bottom, std::int64_t top) const; // NOLINT(bugprone-easily-swappable-parameters)

    /// @brief 扩容：创建足够容纳额外元素的新缓冲区
    /// @param bottom 当前队尾位置
    /// @param top 当前队头位置
    /// @param n 额外需要容纳的元素数量
    /// @return 新缓冲区指针
    [[nodiscard]] TFL_FORCE_INLINE AtomicRingBuffer* resize(std::int64_t bottom, std::int64_t top, std::size_t n) const; // NOLINT(bugprone-easily-swappable-parameters)

private:
    std::int64_t m_cap;    ///< 实际容量（2 的倍数）
    std::int64_t m_mask;   ///< 位掩码（容量 - 1），用于高效取模
    std::atomic<Tp>* m_buf; ///< 存储数组
};


/// @brief 自适应扩容的无锁工作窃取队列 —— Executor 全局共享 buffer。
///
/// 与 BoundedQueue 同源但支持运行期扩容：push 满时自动 resize 到 2x 容量。
/// 旧 buffer 进垃圾列表，析构时统一释放（扩容期间 stealer 仍可安全读取）。
/// 适用于外部线程提交速率不可预测的场景。
/// @tparam Tp 必须为指针类型
template <typename Tp>
    requires std::is_pointer_v<Tp>
class UnboundedQueue : public Immovable<UnboundedQueue<Tp>> {
public:
    using value_type = Tp;

    /// @brief 构造函数，创建指定初始容量的队列
    /// @param cap 初始容量（默认 2 倍默认队列大小）
    explicit UnboundedQueue(std::int64_t cap = 2LL * TFL_DEFAULT_QUEUE_SIZE);

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

    /// @brief 从队列头部窃取元素
    /// @return 成功窃取返回元素，队列空返回 nullptr
    /// @note 供其他线程调用，可能因并发冲突重试
    [[nodiscard]] Tp steal() noexcept;

private:
    static constexpr std::size_t k_garbage_reserve = 64;

    // Why: 使用 2 倍缓存行大小对齐，防止伪共享
    alignas(2 * cache_line_size) std::atomic<std::int64_t> m_top;
    alignas(2 * cache_line_size) std::atomic<std::int64_t> m_bottom;

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
TFL_FORCE_INLINE AtomicRingBuffer<Tp>* AtomicRingBuffer<Tp>::resize( // NOLINT(bugprone-easily-swappable-parameters)
    std::int64_t bottom, std::int64_t top) const {
    auto* ptr = new AtomicRingBuffer{2 * m_cap};
    for (std::int64_t i = top; i != bottom; ++i) {
        ptr->store(i, load(i));
    }
    return ptr;
}

template <typename Tp>
    requires std::is_pointer_v<Tp>
TFL_FORCE_INLINE AtomicRingBuffer<Tp>* AtomicRingBuffer<Tp>::resize( // NOLINT(bugprone-easily-swappable-parameters)
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
Tp UnboundedQueue<Tp>::steal() noexcept {
    // 1. 读取头指针 top（Stealer 端）
    // relaxed 足够：top 仅由本/其他 stealer 的 CAS 推进，单变量竞争由下方
    // CAS 的 RMW 原子性兜底。relaxed 至多读到偏小的陈旧 top，使 t<b 判断偏
    // 保守（少偷一次），绝不会越界；最终 CAS 以最新 top 重新校验。
    std::int64_t top = m_top.load(std::memory_order_relaxed);

    // 2. 读取尾指针 bottom（Owner 端）
    // acquire 与 owner push() 的 m_bottom.store(release) 配对：一旦本线程
    // 观察到新的 bottom，即经由 happens-before 看到 owner 在此之前写入的
    // slot 数据，以及 resize 后的新 m_buf。这是 slot 可见性的唯一来源。
    std::int64_t const bottom = m_bottom.load(std::memory_order_acquire);

    // 3. 判断队列是否有任务
    // [[likely]]：窃取时大概率有任务，引导编译器紧凑排列热路径机器码。
    if (top < bottom) [[likely]] {
        // 4. 必须在 CAS 之前加载数据！
        // 一旦 CAS 成功推进 top，owner 随时可能 push 覆盖该 slot 的内存。
        //
        // m_buf 用 acquire（而非 consume）：consume 在所有主流编译器上都会被
        // 提升为 acquire，标准亦不建议使用；其依赖语义仅覆盖经由该指针派生的
        // 访问，过于脆弱。acquire 语义明确、与编译器实际行为一致，且能接住
        // resize 时 _array.store(release) 发布的新 buffer——更稳健、面向未来。
        // 注：slot 内层 load 用 relaxed，其可见性已由上方 bottom 的 acquire 链承担。
        Tp tmp = m_buf.load(std::memory_order_acquire)->load(top);

        // 5. 强 CAS 抢占该元素的归属权
        // 成功序 acq_rel：无 pop()，top 与 bottom 之间无 Dekker 互斥需求，
        //   纯 stealer-vs-stealer 单变量争用，release/acquire 即可串起 top 的
        //   修改序（modification order），无需进入 seq_cst 全序 S。
        // 失败序 relaxed：失败未发布任何 claim，无需定序。
        // 失败即返回 nullptr，不死循环重试：竞争丢失说明已被他人抢占，
        //   继续自旋只会引发 m_top 的缓存行风暴（cache-line contention）。
        if (!m_top.compare_exchange_strong(top, top + 1,
                                           std::memory_order_acq_rel,
                                           std::memory_order_relaxed)) [[unlikely]] {
            return nullptr;  // 竞争丢失：让出 CPU 去窃取其他队列
        }
        return tmp;  // 成功窃取到元素
    }
    return nullptr;  // 队列为空
}

} // namespace tfl
