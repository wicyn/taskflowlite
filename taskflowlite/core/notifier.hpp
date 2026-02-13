#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <thread>
#include <vector>

#include "utility.hpp"

namespace tfl {

/**
 * 非阻塞通知器（Non-blocking Notifier）
 *
 * 类似条件变量，但等待谓词以乐观方式求值，不需要互斥锁保护。
 *
 * ===== 等待协议（两阶段） =====
 *
 *   if (predicate) return act();
 *   notifier.prepare_wait(wid);        // 进入预等待阶段
 *   if (predicate) {
 *       notifier.cancel_wait(wid);     // 谓词已满足，取消等待
 *       return act();
 *   }
 *   notifier.commit_wait(wid);         // 提交等待，线程挂起直到被通知
 *
 * ===== 通知协议 =====
 *
 *   predicate = true;
 *   notifier.notify_one();
 *
 * ===== 同步原理 =====
 *
 * 算法依赖两个共享变量：用户定义的谓词和内部状态变量。
 * 为避免丢失唤醒（lost wake-up），协议遵循两阶段 "先更新再检查" 模式：
 *   - 等待线程先更新状态（publish intent），再重新检查谓词
 *   - 通知线程先更新谓词，再检查状态
 * 顺序一致性屏障（seq_cst fence）保证至少一方能观察到对方的更新。
 * 因此：要么等待者看到谓词为真而保持活跃，要么通知者看到等待者并发出唤醒。
 * 二者不可能同时错过对方的更新。
 *
 * ===== 状态布局（64 位） =====
 *
 *   [ 32 位 epoch 计数 | 16 位预等待者计数 | 16 位等待者栈顶索引 ]
 *
 *   - 栈顶索引：全 1（0xFFFF）表示空栈，否则为 m_waiters 数组下标
 *   - 预等待者计数：处于 prepare_wait 与 commit/cancel 之间的线程数
 *   - epoch：每次处理一个请求时递增，用于判断轮次
 *
 * 参考: Eigen EventCount
 *       https://gitlab.com/libeigen/eigen/-/blob/master/Eigen/src/ThreadPool/EventCount.h
 */
class Notifier : Immovable<Notifier> {

    friend class Executor;

public:

    /// 等待者节点
    struct alignas(2 * std::hardware_destructive_interference_size) Waiter {
        std::atomic<Waiter*> next;       // 侵入式栈中的下一个等待者
        std::uint64_t epoch;             // prepare_wait 时记录的全局状态快照

        enum : unsigned {
            kNotSignaled = 0,  // 初始 / 已重置
            kWaiting     = 1,  // 已挂起，等待唤醒
            kSignaled    = 2,  // 已被通知
        };
        std::atomic<unsigned> state{kNotSignaled};
    };

    // ---- 状态位域常量 ----

    /// 栈索引位宽
    static constexpr std::uint64_t k_stack_bits  = 16;
    /// 栈索引掩码
    static constexpr std::uint64_t k_stack_mask  = (1ULL << k_stack_bits) - 1;

    /// 预等待者计数位宽
    static constexpr std::uint64_t k_prewaiter_bits  = 16;
    /// 预等待者计数位移
    static constexpr std::uint64_t k_prewaiter_shift = k_stack_bits;
    /// 预等待者计数掩码
    static constexpr std::uint64_t k_prewaiter_mask  = ((1ULL << k_prewaiter_bits) - 1) << k_prewaiter_shift;
    /// 预等待者计数递增量
    static constexpr std::uint64_t k_prewaiter_inc   = 1ULL << k_prewaiter_shift;

    /// epoch 位宽
    static constexpr std::uint64_t k_epoch_bits  = 32;
    /// epoch 位移
    static constexpr std::uint64_t k_epoch_shift = k_stack_bits + k_prewaiter_bits;
    /// epoch 掩码
    static constexpr std::uint64_t k_epoch_mask  = ((1ULL << k_epoch_bits) - 1) << k_epoch_shift;
    /// epoch 递增量
    static constexpr std::uint64_t k_epoch_inc   = 1ULL << k_epoch_shift;

    // ================================================================
    //  构造 / 析构
    // ================================================================

    /**
     * 构造通知器，支持最多 n 个等待者
     * n 必须小于 2^k_prewaiter_bits - 1 = 65535
     */
    explicit Notifier(std::size_t n)
        : m_state{k_stack_mask}   // 空栈标记（栈索引全 1）
        , m_waiters(n)
    {
        assert(n < (1ULL << k_prewaiter_bits) - 1 && "等待者数量超过上限");
    }

    ~Notifier() noexcept {
        // 析构时不应有任何预等待者或已提交的等待者
        assert((m_state.load() & (k_stack_mask | k_prewaiter_mask)) == k_stack_mask);
    }

    // ================================================================
    //  等待接口
    // ================================================================

    /**
     * 进入预等待阶段
     *
     * 递增预等待者计数并记录当前状态快照。
     * 调用后必须恰好跟随一次 commit_wait(wid) 或 cancel_wait(wid)。
     *
     * @param wid 调用线程的等待者索引，范围 [0, n)
     */
    void prepare_wait(std::size_t wid) noexcept {
        m_waiters[wid].epoch = m_state.fetch_add(k_prewaiter_inc, std::memory_order_relaxed);
        // seq_cst 屏障：保证此后重新检查谓词时，能观察到通知线程对谓词的更新
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    /**
     * 提交等待：将线程注册为已提交的等待者并挂起
     *
     * 调用前必须已重新检查过谓词且谓词为假。
     * 线程将被推入无锁等待者栈，然后 park 直到被 notify 唤醒。
     *
     * @param wid 调用线程的等待者索引（与 prepare_wait 相同）
     */
    void commit_wait(std::size_t wid) noexcept {

        Waiter* w = &m_waiters[wid];
        w->state.store(Waiter::kNotSignaled, std::memory_order_relaxed);

        /*
         * Epoch / Ticket 语义
         *
         * prepare_wait 时记录的快照：[base_epoch | ticket | stack]
         *
         *   sepoch = m_state & EPOCH_MASK      （当前全局 epoch）
         *   wepoch = w->epoch & EPOCH_MASK     （快照中的 base epoch）
         *   ticket = w->epoch & PREWAITER_MASK （快照中的预等待计数，即排队序号）
         *
         * 每个进入预等待阶段的线程获得一个单调递增的 ticket，决定处理顺序。
         * 全局 epoch 每处理一个请求递增一次。因此 sepoch - wepoch 的差值
         * 指示当前轮到哪个 ticket：
         *
         *   sepoch - wepoch == ticket  →  轮到本等待者
         *   sepoch - wepoch >  ticket  →  本等待者的 ticket 已过期（已被通知）
         *   sepoch - wepoch <  ticket  →  前序等待者尚未完成
         *
         * 目标 epoch = wepoch + ticket * EPOCH_INC
         *
         * 无符号回绕的正确性：
         *   所有运算为 unsigned 模 2^N 算术，转 int64_t 解释差值。
         *   只要真实差值不超过 2^(EPOCH_BITS-1)，结果正确。
         *   实际差值受限于等待者数量（<= 2^16），远小于 2^31。
         */
        std::uint64_t epoch =
            (w->epoch & k_epoch_mask) +
            (((w->epoch & k_prewaiter_mask) >> k_prewaiter_shift) << k_epoch_shift);

        std::uint64_t state = m_state.load(std::memory_order_seq_cst);

        for (;;) {
            if (std::int64_t((state & k_epoch_mask) - epoch) < 0) {
                // 前序等待者尚未完成决策（cancel/commit/被通知），自旋让出
                std::this_thread::yield();
                state = m_state.load(std::memory_order_seq_cst);
                continue;
            }

            if (std::int64_t((state & k_epoch_mask) - epoch) > 0) {
                // 在等待期间已被通知（epoch 已超越目标），无需挂起
                return;
            }

            // epoch 差值 == 0：轮到本等待者
            // 从预等待计数中移除，推入已提交等待者栈
            assert((state & k_prewaiter_mask) != 0);

            std::uint64_t new_state = state - k_prewaiter_inc + k_epoch_inc;
            new_state = (new_state & ~k_stack_mask) | static_cast<std::uint64_t>(wid);

            // 维护侵入式链表：链接到原栈顶
            if ((state & k_stack_mask) == k_stack_mask) {
                w->next.store(nullptr, std::memory_order_relaxed);   // 栈为空
            } else {
                w->next.store(&m_waiters[state & k_stack_mask], std::memory_order_relaxed);
            }

            if (m_state.compare_exchange_weak(state, new_state, std::memory_order_release)) {
                break;  // 成功入栈
            }
        }

        _park(w);
    }

    /**
     * 取消等待：谓词已满足，退出预等待阶段
     *
     * @param wid 调用线程的等待者索引（与 prepare_wait 相同）
     */
    void cancel_wait(std::size_t wid) noexcept {

        std::uint64_t epoch =
            (m_waiters[wid].epoch & k_epoch_mask) +
            (((m_waiters[wid].epoch & k_prewaiter_mask) >> k_prewaiter_shift) << k_epoch_shift);

        std::uint64_t state = m_state.load(std::memory_order_relaxed);

        for (;;) {
            if (std::int64_t((state & k_epoch_mask) - epoch) < 0) {
                // 前序等待者尚未完成，自旋让出
                std::this_thread::yield();
                state = m_state.load(std::memory_order_relaxed);
                continue;
            }

            if (std::int64_t((state & k_epoch_mask) - epoch) > 0) {
                // 已被通知消费
                return;
            }

            // 移除预等待计数并推进 epoch
            assert((state & k_prewaiter_mask) != 0);
            if (m_state.compare_exchange_weak(
                    state,
                    state - k_prewaiter_inc + k_epoch_inc,
                    std::memory_order_relaxed)) {
                return;
            }
        }
    }

    // ================================================================
    //  通知接口
    // ================================================================

    /**
     * 唤醒一个等待者
     *
     * 优先解除预等待者（仅递增 epoch，开销极小），
     * 若无预等待者则弹出栈顶已提交等待者并执行 unpark。
     * 当无任何等待者时开销极低。
     */
    void notify_one() noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::uint64_t state = m_state.load(std::memory_order_acquire);

        for (;;) {
            // 无等待者（栈空且预等待计数为零）
            if ((state & k_stack_mask) == k_stack_mask && (state & k_prewaiter_mask) == 0) {
                return;
            }

            std::uint64_t num_pre = (state & k_prewaiter_mask) >> k_prewaiter_shift;
            std::uint64_t new_state;

            if (num_pre) {
                // 有预等待者：递增 epoch + 递减计数
                // 该预等待者在 commit_wait/cancel_wait 中会发现 diff > 0 从而直接返回
                new_state = state + k_epoch_inc - k_prewaiter_inc;
            } else {
                // 弹出栈顶已提交的等待者
                Waiter* w = &m_waiters[state & k_stack_mask];
                Waiter* wnext = w->next.load(std::memory_order_relaxed);
                std::uint64_t next = k_stack_mask;
                if (wnext != nullptr) {
                    next = static_cast<std::uint64_t>(wnext - &m_waiters[0]);
                }
                // 注意：此处不递增 epoch。无 ABA 问题：等待者重新入栈前
                // 必经 prepare_wait，该过程必定导致 epoch 递增。
                new_state = (state & k_epoch_mask) | next;
            }

            if (m_state.compare_exchange_weak(state, new_state, std::memory_order_acquire)) {
                if (num_pre) {
                    return;  // 预等待者无需实际唤醒
                }
                Waiter* w = &m_waiters[state & k_stack_mask];
                w->next.store(nullptr, std::memory_order_relaxed);
                _unpark(w);
                return;
            }
        }
    }

    /**
     * 唤醒所有等待者
     *
     * 一次 CAS 清空预等待计数和等待者栈，然后遍历链表唤醒所有已提交等待者。
     * 当无任何等待者时开销极低。
     */
    void notify_all() noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::uint64_t state = m_state.load(std::memory_order_acquire);

        for (;;) {
            // 无等待者
            if ((state & k_stack_mask) == k_stack_mask && (state & k_prewaiter_mask) == 0) {
                return;
            }

            std::uint64_t num_pre = (state & k_prewaiter_mask) >> k_prewaiter_shift;

            // 清空所有：epoch 推进 num_pre 次 + 栈置空
            std::uint64_t new_state =
                ((state & k_epoch_mask) + (k_epoch_inc * num_pre)) | k_stack_mask;

            if (m_state.compare_exchange_weak(state, new_state, std::memory_order_acquire)) {
                if ((state & k_stack_mask) == k_stack_mask) {
                    return;  // 栈本就为空，只有预等待者
                }
                Waiter* w = &m_waiters[state & k_stack_mask];
                _unpark(w);
                return;
            }
        }
    }

    /**
     * 唤醒最多 n 个等待者
     *
     * 先批量解除预等待者（单次 CAS），再逐个弹出栈上已提交等待者。
     * 若 n >= 等待者总数则等价于 notify_all()。
     * 当无任何等待者时开销极低。
     *
     * @param n 最多唤醒的等待者数量
     */
    void notify_n(std::size_t n) noexcept {

        if (n == 0) return;

        if (n >= m_waiters.size()) {
            notify_all();
            return;
        }

        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::uint64_t state = m_state.load(std::memory_order_acquire);

        do {
            // 无等待者
            if ((state & k_stack_mask) == k_stack_mask && (state & k_prewaiter_mask) == 0) {
                return;
            }

            std::uint64_t num_pre = (state & k_prewaiter_mask) >> k_prewaiter_shift;
            std::uint64_t new_state;
            std::size_t consumed;

            if (num_pre) {
                // 批量解除预等待者
                consumed = std::min(n, static_cast<std::size_t>(num_pre));
                new_state = state
                            + (k_epoch_inc * consumed)
                            - (k_prewaiter_inc * consumed);
            } else {
                // 弹出栈顶一个已提交等待者
                Waiter* w = &m_waiters[state & k_stack_mask];
                Waiter* wnext = w->next.load(std::memory_order_relaxed);
                std::uint64_t next = k_stack_mask;
                if (wnext != nullptr) {
                    next = static_cast<std::uint64_t>(wnext - &m_waiters[0]);
                }
                new_state = (state & k_epoch_mask) | next;
                consumed = 1;
            }

            if (m_state.compare_exchange_weak(state, new_state, std::memory_order_acquire)) {
                n -= consumed;
                if (num_pre == 0) {
                    Waiter* w = &m_waiters[state & k_stack_mask];
                    w->next.store(nullptr, std::memory_order_relaxed);
                    _unpark(w);
                }
            }
        } while (n > 0);
    }

    // ================================================================
    //  查询接口
    // ================================================================

    /// 返回等待者槽位总数
    [[nodiscard]] std::size_t size() const noexcept {
        return m_waiters.size();
    }

    /// 返回当前已提交（正在挂起）的等待者数量（近似值，仅供诊断）
    [[nodiscard]] std::size_t num_waiters() const noexcept {
        std::size_t count = 0;
        for (const auto& w : m_waiters) {
            count += (w.state.load(std::memory_order_relaxed) == Waiter::kWaiting);
        }
        return count;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return (1ULL << k_stack_bits) - 1;  // 65535
    }

private:

    std::atomic<std::uint64_t> m_state;
    std::vector<Waiter> m_waiters;

    // ================================================================
    //  Park / Unpark
    //
    //  使用每等待者独立的三态原子变量实现精确唤醒，
    //  避免所有线程竞争同一个原子变量导致惊群效应。
    //
    //  挂起者自身只有两条路径：
    //    1. kNotSignaled → (入栈) → kWaiting → wait → (被唤醒)
    //    2. kNotSignaled → (入栈) → kSignaled        → (跳过 wait)
    // ================================================================

    /// 挂起当前线程
    static void _park(Waiter* w) noexcept {
        unsigned expected = Waiter::kNotSignaled;
        if (w->state.compare_exchange_strong(
                expected, Waiter::kWaiting,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            // 成功转为 kWaiting，阻塞等待唤醒
            w->state.wait(Waiter::kWaiting, std::memory_order_relaxed);
        }
        // CAS 失败说明入栈到 park 之间已被 signal（kSignaled），无需等待
    }

    /// 唤醒链表中所有已提交等待者
    /// 使用三态而非二态（atomic_flag）：仅在对方确实处于 kWaiting 时
    /// 才调用 notify_one()，避免多余的系统调用（约 0.1% 性能提升）
    static void _unpark(Waiter* waiters) noexcept {
        Waiter* next = nullptr;
        for (Waiter* w = waiters; w != nullptr; w = next) {
            // 必须在 unpark 前读取 next，因为唤醒后等待者可能立即重用
            next = w->next.load(std::memory_order_relaxed);
            if (w->state.exchange(Waiter::kSignaled, std::memory_order_relaxed)
                == Waiter::kWaiting) {
                w->state.notify_one();
            }
        }
    }
};

} // namespace tfl
