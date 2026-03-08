/// @file notifier.hpp
/// @brief 无锁非阻塞通知器 - 消除"丢失唤醒"的条件变量替代方案
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

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

/// @brief 无锁非阻塞通知器 - 替代条件变量的高性能同步原语
///
/// @details
/// 类似条件变量但采用乐观锁与无锁设计，无需互斥锁保护。
/// 为彻底消除"丢失唤醒（Lost Wake-up）"的竞态条件，强制要求调用方遵循两阶段等待协议：
/// 1. prepare_wait(): 登记等待意图（发布状态）
/// 2. Double-check: 再次校验业务谓词
/// 3. commit_wait() 或 cancel_wait(): 提交等待或撤销
///
/// @par 内部状态字布局
/// 使用单个 64 位原子变量同步所有状态流转：
/// +----------------+----------------+----------------+
/// |   32-bit      |   16-bit       |   16-bit       |
/// |    Epoch      |  Pre-waiters   |  Stack Top     |
/// | (轮次计数)    | (预等待计数)    |  (等待栈顶)    |
/// +----------------+----------------+----------------+
/// 
/// @par 核心设计
/// - Epoch 轮次：每次 prepare_wait 递增，区分不同批次的等待者
/// - Pre-waiters：已调用 prepare_wait 但尚未 commit/cancel 的线程数
/// - Stack Top：侵入式无锁栈的栈顶索引（k_stack_mask 表示空栈）
///
/// @invariant
/// - pre-waiters > 0: 存在已 prepare 但未 commit/cancel 的线程
/// - stack 为空: stack_top == k_stack_mask && pre_waiters == 0
class Notifier : Immovable<Notifier> {
    friend class Executor;

public:
    /// @brief 侵入式无锁栈的等待者节点
    /// @details 每个 Worker 线程对应一个 Waiter，组成等待栈
    /// @note 按 2 倍缓存行对齐，消除多线程更新状态时的伪共享
    struct alignas(2 * std::hardware_destructive_interference_size) Waiter {
        std::atomic<Waiter*> next;  ///< 侵入式链表指针
        std::uint64_t epoch;        ///< prepare_wait 时的全局状态快照

        /// @brief 等待状态三态机
        enum : unsigned {
            kNotSignaled = 0, ///< 初始态或刚被重置
            kWaiting     = 1, ///< 已提交 OS 等待，等待唤醒
            kSignaled    = 2  ///< 已被通知器标记唤醒
        };
        std::atomic<unsigned> state{kNotSignaled};
    };

    // ====================================================================
    // 状态字位操作常量
    // ====================================================================
    static constexpr std::uint64_t k_stack_bits  = 16;
    static constexpr std::uint64_t k_stack_mask  = (1ULL << k_stack_bits) - 1; // 0xFFFF

    static constexpr std::uint64_t k_prewaiter_bits  = 16;
    static constexpr std::uint64_t k_prewaiter_shift = k_stack_bits;
    static constexpr std::uint64_t k_prewaiter_mask  = ((1ULL << k_prewaiter_bits) - 1) << k_prewaiter_shift;
    static constexpr std::uint64_t k_prewaiter_inc   = 1ULL << k_prewaiter_shift;

    static constexpr std::uint64_t k_epoch_bits  = 32;
    static constexpr std::uint64_t k_epoch_shift = k_stack_bits + k_prewaiter_bits;
    static constexpr std::uint64_t k_epoch_mask  = ((1ULL << k_epoch_bits) - 1) << k_epoch_shift;
    static constexpr std::uint64_t k_epoch_inc   = 1ULL << k_epoch_shift;

public:
    /// @brief 构造函数
    /// @param n 最大并发等待者数量
    /// @pre n < 2^16 - 1 (65535)
    explicit Notifier(std::size_t n)
        : m_state{k_stack_mask}  // 空栈：stack_top = k_stack_mask
        , m_waiters(n)
    {
        assert(n < (1ULL << k_prewaiter_bits) - 1);
    }

    ~Notifier() noexcept {
        // @invariant: 析构时栈必须为空（所有线程已退出）
        assert((m_state.load() & (k_stack_mask | k_prewaiter_mask)) == k_stack_mask);
    }

    /// @brief 第一阶段：宣告即将进入等待（两阶段协议）
    /// @param wid 绑定到当前线程的 Waiter 索引
    /// @post 紧接着必须再次校验业务谓词，然后调用 commit_wait 或 cancel_wait
    ///
    /// @memory_order 推演
    /// - fetch_add(prewaiter_inc, relaxed): 仅递增 prewaiters 计数
    /// - fence(seq_cst): 强制与后续业务谓词检查建立全序关系
    ///
    /// @synchronizes-with: notify_* 的 acquire 读取
    void prepare_wait(std::size_t wid) noexcept {
        // 捕获当前全局状态快照，用于后续判断是否被唤醒
        m_waiters[wid].epoch = m_state.fetch_add(k_prewaiter_inc, std::memory_order_relaxed);

        // 同步点：确保此线程的"准备等待"与唤醒方的"通知"严格全序
        // 彻底消除 Lost Wake-up 的时序窗口
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    /// @brief 第二阶段：确认提交等待并将线程挂起
    /// @param wid 当前线程的 Waiter 索引
    /// @pre 调用前业务谓词必须已 double-check 且确认为假
    ///
    /// @algorithm
    /// 1. 计算目标 epoch（期望被唤醒的轮次）
    /// 2. 轮询直到 epoch 到来或被越过
    /// 3. 将自己加入等待栈并执行 OS 挂起
    ///
    /// @memory_order
    /// - compare_exchange: release 语义确保栈链接操作对唤醒线程可见
    void commit_wait(std::size_t wid) noexcept {
        Waiter* w = &m_waiters[wid];
        w->state.store(Waiter::kNotSignaled, std::memory_order_relaxed);

        // 还原目标 Epoch：基于 prepare 时捕获的快照计算期望轮次
        // epoch' = (snapshot & epoch_mask) + ((snapshot & prewaiter_mask) >> 16 << 32)
        std::uint64_t epoch =
            (w->epoch & k_epoch_mask) +
            (((w->epoch & k_prewaiter_mask) >> k_prewaiter_shift) << k_epoch_shift);

        std::uint64_t state = m_state.load(std::memory_order_seq_cst);

        for (;;) {
            // 目标轮次尚未到来：存在更早进入 prepare 的线程尚未完成
            if (std::int64_t((state & k_epoch_mask) - epoch) < 0) {
                std::this_thread::yield();
                state = m_state.load(std::memory_order_seq_cst);
                continue;
            }

            // 已被通知器越過：无需等待，直接返回
            if (std::int64_t((state & k_epoch_mask) - epoch) > 0) {
                return;
            }

            assert((state & k_prewaiter_mask) != 0);

            // 预等待转正：将 prewaiters 计数转化为 epoch 轮次
            std::uint64_t new_state = state - k_prewaiter_inc + k_epoch_inc;
            new_state = (new_state & ~k_stack_mask) | static_cast<std::uint64_t>(wid);

            // 侵入式链表拼接：当前栈顶 -> 新节点 -> nullptr
            if ((state & k_stack_mask) == k_stack_mask) {
                w->next.store(nullptr, std::memory_order_relaxed);
            } else {
                w->next.store(&m_waiters[state & k_stack_mask], std::memory_order_relaxed);
            }

            // release: 栈链接完成前，对唤醒线程不可见
            if (m_state.compare_exchange_weak(state, new_state, std::memory_order_release)) {
                break;
            }
        }

        // OS 挂起：等待被唤醒
        _park(w);
    }

    /// @brief 取消等待：double-check 发现谓词已满足时撤回休眠意图
    /// @param wid 当前线程的 Waiter 索引
    ///
    /// @algorithm
    /// 与 commit_wait 类似，但仅推进 epoch 而不加入等待栈
    /// 同样需要处理 epoch 轮次判断
    void cancel_wait(std::size_t wid) noexcept {
        std::uint64_t epoch =
            (m_waiters[wid].epoch & k_epoch_mask) +
            (((m_waiters[wid].epoch & k_prewaiter_mask) >> k_prewaiter_shift) << k_epoch_shift);

        std::uint64_t state = m_state.load(std::memory_order_relaxed);

        for (;;) {
            if (std::int64_t((state & k_epoch_mask) - epoch) < 0) {
                std::this_thread::yield();
                state = m_state.load(std::memory_order_relaxed);
                continue;
            }

            if (std::int64_t((state & k_epoch_mask) - epoch) > 0) {
                // 已被消费过（被唤醒过），安静退出
                return;
            }

            assert((state & k_prewaiter_mask) != 0);

            // 取消意图同样需要推进 epoch，确保后续线程不被阻塞
            if (m_state.compare_exchange_weak(
                    state,
                    state - k_prewaiter_inc + k_epoch_inc,
                    std::memory_order_relaxed)) {
                return;
            }
        }
    }

    /// @brief 唤醒一个等待者
    ///
    /// @algorithm
    /// - Fast-path: 无等待者，直接返回
    /// - Pre-waiters 路径：优先消耗已 prepare 的线程（避免 OS 挂起开销）
    /// - Sleeping 路径：从等待栈中弹出并唤醒
    void notify_one() noexcept {
        // 同步点：确保业务数据的修改对所有线程可见
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::uint64_t state = m_state.load(std::memory_order_acquire);

        for (;;) {
            // 无等待者
            if ((state & k_stack_mask) == k_stack_mask && (state & k_prewaiter_mask) == 0) {
                return;
            }

            std::uint64_t num_pre = (state & k_prewaiter_mask) >> k_prewaiter_shift;
            std::uint64_t new_state;

            if (num_pre) {
                // Fast-path: 优先抵消预等待线程，避免 OS 挂起开销
                // 直接推进 epoch 轮次即可
                new_state = state + k_epoch_inc - k_prewaiter_inc;
            } else {
                // 从等待栈弹出栈顶
                Waiter* w = &m_waiters[state & k_stack_mask];
                Waiter* wnext = w->next.load(std::memory_order_relaxed);
                std::uint64_t next = k_stack_mask;
                if (wnext != nullptr) {
                    next = static_cast<std::uint64_t>(wnext - &m_waiters[0]);
                }

                // 仅更新栈顶，不增加 epoch（节点入栈时已推进）
                new_state = (state & k_epoch_mask) | next;
            }

            if (m_state.compare_exchange_weak(state, new_state, std::memory_order_acquire)) {
                if (num_pre) {
                    return; // 预等待线程已被"唤醒"
                }
                Waiter* w = &m_waiters[state & k_stack_mask];
                w->next.store(nullptr, std::memory_order_relaxed);
                _unpark(w);
                return;
            }
        }
    }

    /// @brief 唤醒所有等待者
    void notify_all() noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::uint64_t state = m_state.load(std::memory_order_acquire);

        for (;;) {
            if ((state & k_stack_mask) == k_stack_mask && (state & k_prewaiter_mask) == 0) {
                return;
            }

            std::uint64_t num_pre = (state & k_prewaiter_mask) >> k_prewaiter_shift;

            // 一次性消耗所有 prewaiters 并清空栈
            std::uint64_t new_state =
                ((state & k_epoch_mask) + (k_epoch_inc * num_pre)) | k_stack_mask;

            if (m_state.compare_exchange_weak(state, new_state, std::memory_order_acquire)) {
                if ((state & k_stack_mask) == k_stack_mask) {
                    return;
                }
                Waiter* w = &m_waiters[state & k_stack_mask];
                _unpark(w);
                return;
            }
        }
    }

    /// @brief 唤醒指定数量的等待者
    /// @param n 欲唤醒的数量
    void notify_n(std::size_t n) noexcept {
        if (n == 0) return;

        if (n >= m_waiters.size()) {
            notify_all();
            return;
        }

        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::uint64_t state = m_state.load(std::memory_order_acquire);

        do {
            if ((state & k_stack_mask) == k_stack_mask && (state & k_prewaiter_mask) == 0) {
                return;
            }

            std::uint64_t num_pre = (state & k_prewaiter_mask) >> k_prewaiter_shift;
            std::uint64_t new_state;
            std::size_t consumed;

            if (num_pre) {
                // 优先消耗 pre-waiters
                consumed = std::min(n, static_cast<std::size_t>(num_pre));
                new_state = state
                            + (k_epoch_inc * consumed)
                            - (k_prewaiter_inc * consumed);
            } else {
                // 从栈中弹出
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

    [[nodiscard]] std::size_t size() const noexcept { return m_waiters.size(); }

    [[nodiscard]] std::size_t num_waiters() const noexcept {
        std::size_t count = 0;
        for (const auto& w : m_waiters) {
            count += (w.state.load(std::memory_order_relaxed) == Waiter::kWaiting);
        }
        return count;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return (1ULL << k_stack_bits) - 1;
    }

private:
    std::atomic<std::uint64_t> m_state;    ///< 64 位全局状态字
    std::vector<Waiter> m_waiters;         ///< 等待者池（每个 Worker 一个）

    /// @brief 线程挂起（Park）
    ///
    /// @algorithm
    /// 使用三态原语优化：
    /// - 尝试原子地将 state 从 NotSignaled 改为 Waiting
    /// - 若失败：说明在入栈到挂起之间已被唤醒，直接跳过 OS 挂起
    ///
    /// @performance: 成功挂起时调用 OS 挂起，失败时（已被唤醒）则省去一次系统调用
    static void _park(Waiter* w) noexcept {
        unsigned expected = Waiter::kNotSignaled;
        // CAS 失败说明在入栈和挂起之间已被唤醒，无需再睡
        if (w->state.compare_exchange_strong(
                expected, Waiter::kWaiting,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            // 真正进入 OS 等待
            w->state.wait(Waiter::kWaiting, std::memory_order_relaxed);
        }
    }

    /// @brief 唤醒等待者（Unpark）
    ///
    /// @algorithm
    /// 遍历等待栈，逐个唤醒节点
    /// 使用 exchange 获取先前的 state：若为 Waiting 则唤醒，否则跳过
    static void _unpark(Waiter* waiters) noexcept {
        Waiter* next = nullptr;
        for (Waiter* w = waiters; w != nullptr; w = next) {
            // 先读取 next 指针（线程复用后可能被覆盖）
            next = w->next.load(std::memory_order_relaxed);

            // exchange 返回旧状态：Waiting -> 唤醒 | 其他 -> 跳过
            if (w->state.exchange(Waiter::kSignaled, std::memory_order_relaxed)
                == Waiter::kWaiting) {
                w->state.notify_one(); // 唤醒线程
            }
        }
    }
};

} // namespace tfl
