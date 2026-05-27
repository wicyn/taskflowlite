/// @file notifier.hpp
/// @brief 无锁通知器 Notifier —— 消除"丢失唤醒"的条件变量替代品
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

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

/// @brief 无锁高性能通知原语 —— 线程池"无任务时挂起"的实现核心。
///
/// @details
/// `Notifier` 解决经典并发难题：**Lost Wake-up（丢失唤醒）**。
/// 普通条件变量需要 mutex 配合才能避免，而本类用纯原子操作 + 两阶段协议实现，
/// 不需要任何锁。
///
/// ============================================================================
///  Lost Wake-up 问题与传统方案
/// ============================================================================
/// 经典场景：
/// @code
///   Thread A:  if (queue.empty()) cv.wait();      // ← 检查与 wait 之间
///   Thread B:                queue.push(); cv.notify();  // ← 通知发出
///   Thread A:  cv.wait();                          // ← 永远醒不来！
/// @endcode
///
/// 标准方案是用 mutex 包裹"检查 + wait"原子化。但在工作池场景下：
/// - 每次"看队列空 → 准备睡"都加锁太重；
/// - 唤醒方"push + notify" 也得加锁，与提交热路径冲突。
///
/// `Notifier` 用 **两阶段协议** 替代 mutex：
/// 1. `prepare_wait(wid)` —— 登记"我准备睡了"（发布意图）；
/// 2. **再次** double-check 业务谓词；
/// 3. 谓词为真 → `cancel_wait(wid)`；为假 → `commit_wait(wid)` 真正进 OS 等待。
///
/// 唤醒方在改业务状态后调 `notify_*`：若 step 1 已发生但 step 3 未发生，会
/// 直接消耗 prewaiter 计数，根本不让本线程进 OS 挂起 —— 唤醒"先到达"的等待者。
///
/// ============================================================================
///  64 位状态字布局 —— 单原子保护一切
/// ============================================================================
/// @code
///    63                 32 31         16 15           0
///   +-------------------+-------------+--------------+
///   |     Epoch (32)    | Pre-Waiters | Stack Top    |
///   |                   |    (16)     |    (16)      |
///   +-------------------+-------------+--------------+
/// @endcode
/// - **Epoch（32 位）**：轮次计数，每次 prepare 递增。区分不同批次的等待者，
///   是 commit/cancel 判断"是否已被越过"的依据；
/// - **Pre-Waiters（16 位）**：已 prepare 但未 commit/cancel 的线程数。
///   notify 优先消耗这部分（不需要 OS 挂起）；
/// - **Stack Top（16 位）**：侵入式无锁栈的栈顶 waiter 索引。
///   `k_stack_mask` 表示空栈。
///
/// 三个字段共住一个 64 位原子，所有状态变更通过 CAS 一次完成 —— 这是无锁化的
/// 关键。容量上限 65535 等待者（16 位空间），对工作池场景绰绰有余。
///
/// ============================================================================
///  Waiter 节点 —— 三态机
/// ============================================================================
/// @code
///   kNotSignaled (0)  ──prepare→  prepare 后挂栈，未挂 OS
///       │
///       ↓ commit_wait CAS
///   kWaiting (1)      ──notify→ kSignaled 并 atomic::notify_one
///       │
///       ↓ wait 醒
///   kSignaled (2)
/// @endcode
///
/// **关键优化（_park）**：commit_wait 把节点挂到栈后调 `_park` 准备 OS 挂起。
/// `_park` 用 `compare_exchange(NotSignaled → Waiting)`：若 CAS 失败说明
/// 在"挂栈到挂 OS"的窗口内已经被 notify 抢先把状态设为 Signaled，**直接跳过
/// OS 挂起返回**。这省下整次 syscall 开销，在高并发下显著。
///
/// ============================================================================
///  notify_one / notify_all / notify_n
/// ============================================================================
/// 三种唤醒粒度：
/// - **`notify_one`** ：唤醒一个等待者（先消耗 pre-waiter，再弹栈）；
/// - **`notify_all`** ：原子推进 epoch + 清空栈，一次唤醒所有；
/// - **`notify_n(k)`**：循环消耗 / 弹栈，直到唤醒 k 个或栈空 ——
///   Executor::_schedule 批量入队后用这个减少唤醒次数。
///
/// 所有 notify 入口前都 `atomic_thread_fence(seq_cst)`，建立"业务数据可见 →
/// 唤醒动作"的全序关系，**这是 Lost Wake-up 防御的最后一道闸**。
///
/// ============================================================================
///  使用示例（来自 Executor::_wait_for_work）
/// ============================================================================
/// @code
///   notifier.prepare_wait(wid);
///
///   // double-check：扫描所有队列
///   for (auto& q : queues) {
///       if (!q.empty()) {
///           notifier.cancel_wait(wid);
///           return q.steal();
///       }
///   }
///
///   notifier.commit_wait(wid);   // 真正进 OS 挂起
/// @endcode
///
/// @invariant pre-waiters > 0 ⇒ 有线程在 prepare~commit 窗口内
/// @invariant 栈空 ⇔ stack_top == k_stack_mask && pre_waiters == 0
/// @see Executor::_wait_for_work / _schedule  使用方
class Notifier : Immovable<Notifier> {
    friend class Executor;

public:
    /// @brief 单个等待者的状态节点 —— 侵入式无锁栈的链表元素。
    ///
    /// @details
    /// 每个 Worker 对应一个 `Waiter`，它同时是：
    /// - **侵入式链表节点**（`next` 指针指向栈中下一个）；
    /// - **三态机原子单元**（kNotSignaled / kWaiting / kSignaled）；
    /// - **epoch 快照容器**（prepare 时记录全局状态，commit 时计算目标 epoch）。
    ///
    /// `alignas(2 * cache_line_size)` —— 多个 worker 同时在自己的 Waiter 上写，
    /// 隔离 cache line 防止伪共享。这种 per-worker 数据的对齐是高并发数据结构的
    /// 标配。
    ///
    /// @see Notifier  本节点的容器
    struct alignas(2 * cache_line_size) Waiter {
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

        while (true) {
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

        while (true) {
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

        while (true) {
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

        while (true) {
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
