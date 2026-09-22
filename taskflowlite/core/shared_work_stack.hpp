/// @file shared_work_stack.hpp
/// @brief 基于原子发布链和原子消费门的共享 Work 调度栈。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-09-20
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <type_traits>

#include "macros.hpp"
#include "utility.hpp"
#include "work.hpp"

namespace tfl {

/// @brief 面向 64 位平台的两阶段 intrusive Work 共享调度栈。
///
/// Work 通过 `m_next` 组成单向链表，SharedWorkStack 不拥有 Work。
/// 多个 producer 通过原子 `m_incoming` 发布 Work；多个 thief 通过
/// `m_state` 最高位竞争消费端独占权，成功取得独占权的 thief 才能访问
/// 非原子的本地消费链 `m_head`。
///
/// `m_state` 使用 uint64_t，第 63 位为消费端独占位，低 63 位保存已计数但尚未完成
/// 窃取的 Work 数量，包括已经计数但尚未发布到 incoming 链的 Work。
/// push 在计数不溢出的前提下只增加低位计数，不影响独占位；steal 在
/// 完成节点摘除后一次性减少任务计数并清除独占位。
///
/// steal 首先通过 relaxed load 预检查任务计数和消费端独占位，观察到
/// 空栈或已有 thief 持有独占权时直接返回 nullptr；预检查通过后，再以
/// fetch_or 竞争消费端独占权，并重新检查实际取得的状态。
///
/// 当本地消费链为空时，steal 使用一次 atomic exchange 接管当前完整的
/// incoming 链；后续窃取直接从本地链摘取 Work，直到本地链再次为空。
///
/// push 不获取消费端独占权；steal 竞争独占权失败时直接返回 nullptr，
/// 由 Worker 继续尝试其他本地队列或共享调度栈。消费进度依赖持有独占权
/// 的 thief 完成操作，因此整个容器不提供 lock-free 的消费进度保证。
///
/// 批量 push 在发布前完成 Work 链连接，最终通过一次成功的 CAS 将整条链
/// 挂到 `m_incoming` 头部；竞争时 CAS 可能重试，但无需逐个原子发布节点。
///
/// push 在发布 Work 前增加任务计数，steal 在完成节点摘除后减少任务计数，
/// 因此计数包含尚未发布或正在摘除的 Work。size 和 empty 使用 relaxed
/// 读取，仅用于调度观察，不保证观察到其他线程的最新计数。
///
/// @note 不保证 FIFO 或严格的全局 LIFO 调度顺序；已经接管到 `m_head` 的 Work
///       会优先于后续进入 `m_incoming` 的 Work 被当前消费链处理。
/// @note 同一个 Work 在被成功 steal 前不得再次加入任何使用 `m_next` 的运行期链，
///       也不得由外部修改其 m_next 或结束其生命周期。
/// @note 所有并发 push 必须保证累计未完成窃取的计数不超过 SIZE_MASK；容量断言
///       仅用于调试诊断，不提供溢出恢复或 Release 模式下的容量检查。
/// @note steal 返回 nullptr 不代表全局无任务，调用方仍须遵循重试和睡眠前复查协议。
/// @note 销毁栈前必须停止所有并发访问；析构不释放 Work，也不逐个清理其 m_next。
class alignas(TFL_CACHE_LINE_SIZE) SharedWorkStack : public Immovable<SharedWorkStack> {
public:
    /// @brief 将单个 Work 发布到共享调度栈。
    /// @param work 要发布的 Work。
    /// @pre work 非空，m_next 为 nullptr，且当前不属于任何使用 m_next 的运行期链。
    /// @pre 增加计数后，累计未完成窃取的 Work 数量不超过 SIZE_MASK。
    TFL_FORCE_INLINE void push(Work* work) noexcept;

    /// @brief 将一组 Work 批量发布到共享调度栈。
    /// @param first 指向首个 Work 的随机访问迭代器。
    /// @param n 要发布的 Work 数量。
    /// @pre n > 0，n <= SIZE_MASK，且 first 可合法访问连续 n 个元素。
    /// @pre 区间中的 Work 均非空、互不相同，m_next 均为 nullptr，且不属于其他运行期链。
    /// @pre 迭代器下标访问不抛异常，且访问期间输入区间不被并发修改。
    /// @pre 增加计数后，累计未完成窃取的 Work 数量不超过 SIZE_MASK。
    template <std::random_access_iterator Iterator>
        requires std::same_as<std::remove_cvref_t<std::iter_reference_t<Iterator>>, Work*>
    TFL_FORCE_INLINE void push(Iterator first, std::size_t n) noexcept;

    /// @brief 将一条已经连接完成的 Work 链发布到共享调度栈。
    /// @param first 链表首节点。
    /// @param last 链表尾节点。
    /// @param n 链表中的 Work 数量。
    /// @pre first 和 last 非空，n > 0，n <= SIZE_MASK，且 last->m_next 为 nullptr。
    /// @pre first 到 last 构成恰好包含 n 个互不相同节点的无环链。
    /// @pre 整条链当前不属于其他使用 m_next 的运行期链。
    /// @pre 增加计数后，累计未完成窃取的 Work 数量不超过 SIZE_MASK。
    TFL_FORCE_INLINE void push(Work* first, Work* last, std::size_t n) noexcept;

    /// @brief 尝试从共享调度栈窃取一个 Work。
    /// @return 成功取得消费端独占权且取得 Work 时返回 Work，否则返回 nullptr。
    ///
    /// @note 观察到空栈或已有 thief 持有独占权时直接返回 nullptr，不循环等待独占权。
    ///       当前消费链为空时，通过一次 exchange 接管完整 incoming 链。
    /// @note 返回 Work 的 m_next 已清空；并发发布或独占状态变化时，本次尝试可能错过任务。
    [[nodiscard]] TFL_FORCE_INLINE Work* steal() noexcept;

    /// @brief 返回共享调度栈当前是否为空。
    /// @return 当前观察到的任务计数为 0 时返回 true，否则返回 false。
    ///
    /// @note 仅用于调度观察，可能读取到较旧的计数；非零计数可能包含尚未发布的 Work。
    [[nodiscard]] TFL_FORCE_INLINE bool empty() const noexcept;

    /// @brief 返回当前观察到的未完成窃取的 Work 数量。
    /// @return 当前观察到的任务计数。
    ///
    /// @note 仅用于调度观察，可能读取到较旧的计数，返回后数量也可能立即发生变化。
    [[nodiscard]] TFL_FORCE_INLINE std::int64_t size() const noexcept;

private:
    static constexpr std::uint64_t LOCKED = std::uint64_t{1} << (std::numeric_limits<std::uint64_t>::digits - 1);
    static constexpr std::uint64_t SIZE_MASK = LOCKED - 1;

    alignas(TFL_CACHE_LINE_SIZE) std::atomic<Work*> m_incoming{nullptr};
    alignas(TFL_CACHE_LINE_SIZE) std::atomic<std::uint64_t> m_state{0};
    alignas(TFL_CACHE_LINE_SIZE) Work* m_head{nullptr};
};

inline void SharedWorkStack::push(Work* work) noexcept {
    TFL_ASSERT(work);
    TFL_ASSERT(work->m_next == nullptr);

    [[maybe_unused]] const std::uint64_t state = m_state.fetch_add(1, std::memory_order_relaxed);
    TFL_ASSERT((state & SIZE_MASK) < SIZE_MASK);

    Work* head = m_incoming.load(std::memory_order_relaxed);

    do {
        work->m_next = head;
    } while (!m_incoming.compare_exchange_weak(
        head,
        work,
        std::memory_order_release,
        std::memory_order_relaxed
        ));
}

template <std::random_access_iterator Iterator>
    requires std::same_as<std::remove_cvref_t<std::iter_reference_t<Iterator>>, Work*>
inline void SharedWorkStack::push(Iterator first, std::size_t n) noexcept {
    TFL_ASSERT(n != 0);
    TFL_ASSERT(n <= SIZE_MASK);

    Work* const head = first[0];
    TFL_ASSERT(head);
    TFL_ASSERT(head->m_next == nullptr);

    Work* tail = head;

    for (std::size_t i = 1; i < n; ++i) {
        Work* const work = first[i];

        TFL_ASSERT(work);
        TFL_ASSERT(work != tail);
        TFL_ASSERT(work->m_next == nullptr);

        tail->m_next = work;
        tail = work;
    }

    [[maybe_unused]] const std::uint64_t state = m_state.fetch_add(n, std::memory_order_relaxed);
    TFL_ASSERT((state & SIZE_MASK) <= SIZE_MASK - n);

    Work* current = m_incoming.load(std::memory_order_relaxed);

    do {
        tail->m_next = current;
    } while (!m_incoming.compare_exchange_weak(
        current,
        head,
        std::memory_order_release,
        std::memory_order_relaxed
        ));
}

inline void SharedWorkStack::push(Work* first, Work* last, std::size_t n) noexcept {
    TFL_ASSERT(first);
    TFL_ASSERT(last);
    TFL_ASSERT(n != 0);
    TFL_ASSERT(n <= SIZE_MASK);
    TFL_ASSERT(last->m_next == nullptr);
    TFL_ASSERT((n == 1) == (first == last));

    [[maybe_unused]] const std::uint64_t state = m_state.fetch_add(n, std::memory_order_relaxed);
    TFL_ASSERT((state & SIZE_MASK) <= SIZE_MASK - n);

    Work* head = m_incoming.load(std::memory_order_relaxed);

    do {
        last->m_next = head;
    } while (!m_incoming.compare_exchange_weak(
        head,
        first,
        std::memory_order_release,
        std::memory_order_relaxed
        ));
}

inline Work* SharedWorkStack::steal() noexcept {
    std::uint64_t state = m_state.load(std::memory_order_relaxed);

    if ((state & LOCKED) || ((state & SIZE_MASK) == 0)) {
        return nullptr;
    }

    state = m_state.fetch_or(LOCKED, std::memory_order_acquire);

    if (state & LOCKED) [[unlikely]] {
        return nullptr;
    }

    if ((state & SIZE_MASK) == 0) [[unlikely]] {
        m_state.fetch_and(SIZE_MASK, std::memory_order_release);
        return nullptr;
    }

    Work* work = m_head;

    if (!work) {
        work = m_incoming.exchange(nullptr, std::memory_order_acquire);

        if (!work) [[unlikely]] {
            m_state.fetch_and(SIZE_MASK, std::memory_order_release);
            return nullptr;
        }
    }

    m_head = work->m_next;
    work->m_next = nullptr;

    m_state.fetch_sub(LOCKED + 1, std::memory_order_release);

    return work;
}

inline bool SharedWorkStack::empty() const noexcept {
    return (m_state.load(std::memory_order_relaxed) & SIZE_MASK) == 0;
}

inline std::int64_t SharedWorkStack::size() const noexcept {
    return m_state.load(std::memory_order_relaxed) & SIZE_MASK;
}

}  // namespace tfl
