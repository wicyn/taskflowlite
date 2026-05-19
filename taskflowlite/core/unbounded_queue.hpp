/// @file unbounded_queue.hpp
/// @brief 自适应扩容的无锁队列 UnboundedQueue —— 全局共享 buffer 实现
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
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
/// @details
/// `AtomicRingBuffer` 是 UnboundedQueue 的内部组件，单独抽出是为了把"定长存储 +
/// 索引循环 + 数据迁移" 这件事和"队列协议（top/bottom 移动）" 分离。
///
/// 容量必须 2 的幂次方（同 BoundedQueue），用 `m_mask` 替代取模。
/// 元素槽位是 `std::atomic<Tp>`，每次 store / load 都是原子的 —— 这是
/// UnboundedQueue 在 owner 与 stealer 不同端并发访问时的安全前提。
///
/// 关键能力是 `resize(bottom, top)` ——
/// 队列满时由 owner 调用：分配 2× 大小的新 buffer，把 `[top, bottom)` 区间的
/// 元素拷贝过去，返回新 buffer。`resize(bottom, top, n)` 接收"额外要塞 n 个"的
/// 提示，按 `bit_ceil(needed)` 分配，避免 push(iter, n) 触发多次扩容。
///
/// **旧 buffer 不能立刻释放** —— stealer 可能还在用旧地址读元素。
/// UnboundedQueue 把旧 buffer 收进 `m_garbage` 等析构时一并清理（只在 owner 终止
/// 后释放是安全的）。
///
/// @tparam Tp 必须为指针类型
/// @see UnboundedQueue  基于本类的无锁无界队列
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


/// @brief 自适应扩容的无锁工作窃取队列 —— Executor 共享 buffer 的实现。
///
/// @details
/// 与 `BoundedQueue` 同源，但 owner 在队列满时不调用回调，而是 **就地扩容**
/// 到 2× 容量的新 buffer。这是为 Executor 的全局共享 buffer 设计的：
///
/// | 特性          | `BoundedQueue`                | `UnboundedQueue`               |
/// |---------------|--------------------------------|-------------------------------|
/// | 容量          | 编译期固定（2^n）              | 运行期自适应扩容（2× 翻倍）    |
/// | 满处理        | 返回 false / 调溢出回调         | 自动 resize 后继续 push        |
/// | 适用场景      | Worker 本地队列（容量可控）     | 全局共享 buffer（容量难预估）  |
/// | 性能          | 无扩容开销，最热路径            | 扩容时一次内存分配 + 拷贝       |
///
/// ============================================================================
///  为什么外部线程入队走 Unbounded 而不是 Bounded？
/// ============================================================================
/// 外部线程的提交速率不可预测 —— 一个 main 函数突然 push 几万个任务是合理的。
/// 用 BoundedQueue 会反复触发"满 → 回退"，破坏批量入队效率。
/// UnboundedQueue 的扩容代价（一次 alloc + memcpy）摊薄到大量任务上几乎可忽略。
///
/// 反过来，Worker 本地队列容量受限于"任务粒度合理时不会失控" 的常理 ——
/// 用 Bounded 既省内存又避免扩容卡顿。
///
/// ============================================================================
///  与 BoundedQueue 共享的 Chase-Lev 协议
/// ============================================================================
/// - 同样的 LIFO push/pop + FIFO steal 双端模型；
/// - 同样的 acquire/release 配对内存序；
/// - 同样的 single-element CAS 仲裁；
/// - 唯一区别：push 满时调用 `m_buffer = m_buffer->resize(b, t)` 替换底层。
///
/// 旧 buffer 进 `m_garbage` 列表，析构时统一释放 —— 期间 stealer 仍能安全
/// 读取（旧 buffer 内存不动）。
///
/// @tparam Tp 必须为指针类型
/// @see BoundedQueue        定长对偶
/// @see Executor::Buffer    包装成全局 buffer 的容器
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

    if (top <= bottom) [[likely]] {
        Tp val = buf->load(bottom);

        // 队列仅剩一个元素，可能与 Stealer 竞争
        if (top == bottom) [[unlikely]] {
            // 使用 CAS 尝试原子增加 top
            if (!m_top.compare_exchange_strong(top, top + 1,
                                               std::memory_order_seq_cst, std::memory_order_relaxed)) [[unlikely]] {
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
    // 1. 读取当前的头指针（Stealer 端）
    std::int64_t top = m_top.load(std::memory_order_acquire);

    // Why: Chase-Lev 工作窃取算法的灵魂屏障（Memory Fence）
    // 强制确保对 m_top 的读取绝对发生在对 m_bottom 的读取之前。
    // 如果没有这个 seq_cst 屏障，CPU 乱序执行可能会让 Stealer 先看到新的 bottom，
    // 再看到旧的 top，从而误以为队列有元素，引发致命的并发错误。
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // 2. 读取当前的尾指针（Owner 端）
    std::int64_t const bottom = m_bottom.load(std::memory_order_acquire);

    // 3. 判断队列是否有任务
    // 使用 [[likely]] 引导编译器：我们期望窃取时大概率是有任务的，让这段机器码紧凑排列
    if (top < bottom) [[likely]] {

        // Why: 必须在 CAS 之前加载数据！
        // 因为一旦 m_top 的 CAS 成功，Owner 线程随时可能推入新任务并覆盖这个位置的内存。
        Tp tmp = m_buf.load(std::memory_order_acquire)->load(top);

        // 4. 尝试用强 CAS 抢占该元素的归属权
        // Why: CAS 失败即返回 nullptr,不做死循环重试。若竞争丢失,
        // 说明有其他 Stealer 或 Owner 抢先抢占,继续等待会引发缓存行风暴。
        if (!m_top.compare_exchange_strong(top, top + 1,
                                           std::memory_order_seq_cst,
                                           std::memory_order_relaxed)) [[unlikely]] {
            return nullptr;  // CAS 失败：竞争丢失，让出 CPU 去窃取其他队列
        }

        return tmp;  // 成功窃取到元素！
    }

    return nullptr;  // 队列为空，直接返回
}


template <typename Tp>
    requires std::is_pointer_v<Tp>
Tp UnboundedQueue<Tp>::steal(std::size_t& num_empty_steals) noexcept {
    std::int64_t top = m_top.load(std::memory_order_acquire);

    std::atomic_thread_fence(std::memory_order_seq_cst);

    std::int64_t const bottom = m_bottom.load(std::memory_order_acquire);

    if (top < bottom) [[likely]] {
        // 发现目标队列中有任务（即使后续可能没抢到），重置空载窃取计数
        num_empty_steals = 0;

        Tp tmp = m_buf.load(std::memory_order_acquire)->load(top);

        if (!m_top.compare_exchange_strong(top, top + 1,
                                           std::memory_order_seq_cst,
                                           std::memory_order_relaxed)) [[unlikely]] {
            // CAS 失败说明队列有任务只是被人抢先了，所以不算作 empty steal
            return nullptr;
        }

        return tmp;  // 成功返回元素
    } else {
        // 队列真正为空（top >= bottom），累加连续空载窃取计数
        // 外层调度器可以根据这个计数值决定当前线程是否需要 yield 或 sleep
        ++num_empty_steals;
    }

    return nullptr;  // 队列为空返回
}

} // namespace tfl
