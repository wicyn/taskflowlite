/// @file  notifier.hpp
/// @brief 原子 wait/notify 唤醒原语 —— 两阶段协议消除 Lost Wake-up。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
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

// ============================================================================
//  两线程过程演练（wid=0 / wid=1）—— 读代码前先建立心智模型
// ============================================================================
//
//  状态字记法 {e, p, s}：
//    e = epoch 轮次（高 32 bit，原始递增值，允许回绕）
//    p = prewaiter 预等待计数（中 16 bit）
//    s = 栈顶索引（低 16 bit，FFFF = 空栈哨兵）
//  增量常量：prewaiter_inc = 0x1'0000，epoch_inc = 0x1'0000'0000
//  构造后初态：m_state = {0, 0, FFFF}
//
//  ----------------------------------------------------------------------------
//  Phase 1：两个 worker 各自 prepare_wait（票号在此刻确定）
//  ----------------------------------------------------------------------------
//    T0 fetch_add: 返回旧值 {0,0,FFFF} → m_state={0,1,FFFF}; fence(seq_cst)
//       waiter[0].epoch = {0,0,FFFF}  → target0 = e(0) + p(0) = 0
//    T1 fetch_add: 返回旧值 {0,1,FFFF} → m_state={0,2,FFFF}; fence(seq_cst)
//       waiter[1].epoch = {0,1,FFFF}  → target1 = e(0) + p(1) = 1
//
//    fetch_add 返回"自增前"的值 → prepare 的物理顺序天然刻进票号。
//    票号 0 必须先结算，票号 1 后结算 —— 这是后续所有"乱序自旋"的根因。
//    紧接着两个 worker 各做一次业务谓词 double-check（fence 之后），
//    谓词为真 → cancel_wait（有活，不睡）；为假 → commit_wait（没活，睡）。
//
//  ----------------------------------------------------------------------------
//  Case A：两个都 cancel（都发现有活）
//  ----------------------------------------------------------------------------
//   A1 顺序结算（T0 先）：
//     T0 cancel,t0=0: load{0,2,FFFF},(0-0)=0 命中 → CAS→{1,1,FFFF} return
//     T1 cancel,t1=1: load{1,1,FFFF},(1-1)=0 命中 → CAS→{2,0,FFFF} return
//     终态 {2,0,FFFF}：两 prewaiter 全撤，epoch 推进 2，栈空（干净）
//   A2 乱序（T1 物理先动手）：
//     T1 cancel,t1=1: load{0,2,FFFF},(0-1)=-1<0 → 分支一:yield,reload,自旋…
//     T0 cancel,t0=0: load{0,2,FFFF},(0-0)=0 → CAS→{1,1,FFFF} return
//     T1 reload{1,1,FFFF},(1-1)=0 → CAS→{2,0,FFFF} return（与 A1 同终态）
//     要点：票号机把乱序物理执行压成票号序；同一 epoch 上两线程不会
//           同时进入"正好轮到"分支去抢同一次 CAS（已被票号串行化）。
//
//  ----------------------------------------------------------------------------
//  Case B：T0 cancel，T1 commit（睡）
//  ----------------------------------------------------------------------------
//   B1 T1 正常入栈睡眠：
//     T0 cancel→{1,1,FFFF}
//     T1 commit,t1=1: load(seq_cst){1,1,FFFF},(1-1)=0 命中
//        new = {1,1,FFFF}-pre+epoch={2,0,FFFF}; (&~stack)|1 → {2,0,0001}
//        旧栈=FFFF(空) → w1.next=nullptr; CAS(release)→{2,0,0001}
//        _park(w1): CAS NotSignaled→Waiting ✓ → futex wait（T1 睡）
//     终态 {2,0,0001}，T1 挂栈顶等 notify
//   B2 notify 抢在 T1 commit 之前（prewaiter 快速路径 → T1 命中分支二）：
//     notify_one: load(acquire){1,1,FFFF},p=1 → 快速路径
//        new = state + epoch_inc - pre_inc = {2,0,FFFF}; CAS ✓; return（不发 futex）
//     T1 commit,t1=1: load{2,0,FFFF},(2-1)=+1>0 → 分支二 → return（不入栈、不挂起）
//     终态 {2,0,FFFF}：省下一对 futex wait/wake 系统调用
//
//  ----------------------------------------------------------------------------
//  Case C：T0 commit（睡），T1 cancel
//  ----------------------------------------------------------------------------
//     T0 commit,t0=0: load(seq_cst){0,2,FFFF},(0-0)=0 命中
//        new={1,1,FFFF};(&~stack)|0→{1,1,0000}; 旧栈FFFF→w0.next=null
//        CAS(release)→{1,1,0000}; _park(w0)→Waiting,futex wait（T0 睡）
//     T1 cancel,t1=1: load(relaxed){1,1,0000},(1-1)=0 命中
//        CAS(relaxed){1,1,0000}→{2,0,0000} return  ← 栈位 0000 没动!
//     终态 {2,0,0000}：cancel 只动 e/p，绝不碰栈，T0 安然留栈等后续 notify
//
//  ----------------------------------------------------------------------------
//  Case D：两个都 commit（都睡）
//  ----------------------------------------------------------------------------
//   D1 顺序入栈，形成 LIFO 链：
//     T0 commit→{1,1,0000}, w0.next=null, T0 睡
//     T1 commit,t1=1: load(seq_cst){1,1,0000},(1-1)=0
//        new={2,0,0000};(&~stack)|1→{2,0,0001}; 旧栈0000≠FFFF→w1.next=&waiter[0]
//        CAS(release)→{2,0,0001}; _park(w1)→Waiting, T1 睡
//     终态 {2,0,0001}：栈 = [w1 → w0 → null]（LIFO）
//     D1-a 之后 notify_one：弹栈顶 w1，state→{2,0,0000}，_unpark(w1)；T0 仍睡
//     D1-b 之后 notify_all：state→{2,0,FFFF}，_unpark 沿 next 链唤 w1→w0
//   D2 notify_all 抢在两个 commit 之前（全是 prewaiter）：
//     notify_all: load(acquire){0,2,FFFF},num_pre=2
//        new=(e0 + epoch_inc*2)|FFFF={2,0,FFFF}; CAS ✓; 旧栈FFFF→return(不 unpark)
//     T0/T1 commit: load{2,0,FFFF},(2-0)/(2-1)>0 → 均命中分支二 return（都不睡）
//     终态 {2,0,FFFF}：epoch 一次跳 2，连一次 futex 都不发
//   D3 混合（T0 已 park，T1 仍 prewaiter）notify_one 优先消费 prewaiter：
//     state={1,1,0000}(T0 睡); notify_one: p=1 快速路径
//        new={1,1,0000}+epoch_inc-pre_inc={2,0,0000}; CAS ✓; return（不发 futex）
//     T1 commit: load{2,0,0000},(2-1)>0 → 分支二 return（T1 不睡）
//     终态 {2,0,0000}：notify_one 的"一个"被更便宜的 prewaiter 满足，
//        物理上更早入栈的睡眠者 T0 留在原地等下一发 notify（不构成饿死，
//        因正常用法 notify 次数与任务提交次数对齐）
//
//  ----------------------------------------------------------------------------
//  第三层：park / unpark 在单个 w->state 上的竞争（与 m_state 无关）
//  ----------------------------------------------------------------------------
//   交错一（notify 先到，_park 省 syscall）：
//     T0 已 CAS 入栈但未 _park; _unpark: exchange(w0.state,Signaled)，
//        此刻 w0.state=NotSignaled → 旧值≠Waiting → 不发 wake;
//     T0 _park: CAS(NotSignaled→Waiting) 现值=Signaled → 失败 → 跳过 futex wait（不睡）
//   交错二（_park 先睡，notify 正常发 wake）：
//     T0 _park: CAS→Waiting → futex wait; _unpark: exchange→Signaled,旧==Waiting→wake
//   两交错皆安全：park 的 CAS 与 unpark 的 exchange 同作用于一个 w->state
//   变量 → 有全序，三态机闭合此窗口。
//
//  ----------------------------------------------------------------------------
//  T1 先 prepare 的镜像：票号对调（T1=0,T0=1），上表每行 T0/T1 互换即可，
//  状态字算术分毫不变。
// ============================================================================

namespace tfl {

/// @brief 无锁高性能通知原语 —— 两阶段协议消除 Lost Wake-up。
///
/// @details
/// `Notifier` 是调度器层面的 EventCount（事件计数器）：一个 worker 抢不到
/// 任务、准备睡眠时，危险窗口在"检查到无任务"与"真正挂起"之间 —— 这期间
/// 若 producer push 任务并 notify，通知既看不到已睡线程（还没睡）就会丢失，
/// worker 永久睡死。两阶段协议（prepare → double-check → commit/cancel）
/// 用一对 seq_cst fence 关闭此窗口。它由 `Executor` 持有，每个 Worker 绑定
/// 一个 `Waiter`。
///
/// 核心决策：epoch（轮次）+ prewaiter（预等待计数）+ 侵入式栈顶，三者打包进
/// **单个 64 位原子字** `m_state`。整套协议的正确性依赖"单次 CAS 同时改动这
/// 三件事"，分成三个独立原子会退化成多变量一致性问题 + ABA。代价是栈字段仅
/// 16 bit → capacity() ≤ 65535，栈里存的是 m_waiters 的**索引**而非指针。
///
/// ============================================================================
///  64 位状态字布局
/// ============================================================================
/// @code
///   63                              32 31              16 15               0
///  ┌──────────────────────────────────┬──────────────────┬──────────────────┐
///  │            epoch (32)             │ prewaiter cnt(16)│  stack top (16)  │
///  └──────────────────────────────────┴──────────────────┴──────────────────┘
///                                                          FFFF = 空栈哨兵
/// @endcode
///
/// ============================================================================
///  三态机与两阶段协议
/// ============================================================================
/// @code
///   prepare_wait(wid);          // 阶段一：登记为 prewaiter + seq_cst fence
///   if (predicate())            // fence 之后再查一次业务谓词（关键）
///       cancel_wait(wid);       // 有活 → 撤回，不睡
///   else
///       commit_wait(wid);       // 确实没活 → 入栈挂起
/// @endcode
///
/// ============================================================================
///  Thread safety（线程安全）
/// ============================================================================
/// 同一 wid 的 prepare/commit/cancel 必须由其绑定线程串行调用（每线程一个
/// Waiter）。notify_one/all/n 可从任意线程并发调用。size()/capacity() 任意
/// 线程安全；num_waiters() 为 O(n) 非精确快照。类为 Immovable —— 一旦 move，
/// &m_waiters[0] 基址失效，所有 index↔pointer 转换全废。
///
/// @see Executor   持有 Notifier，驱动 worker 的 park/unpark 生命周期
/// @see Waiter     每个 worker 的状态节点（侵入式栈元素 + 三态机）
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
    struct alignas(2 * cache_line_size) Waiter {
        std::atomic<Waiter*> next;  ///< 侵入式链表指针：指向栈中下方节点，栈底为 nullptr
        std::uint64_t epoch;        ///< prepare_wait 时 fetch_add 返回的全局状态快照（票号原料）

        /// @brief 等待状态三态机 —— 关闭"入栈到挂起之间"的唤醒窗口
        enum : unsigned {
            kNotSignaled = 0, ///< 初始态或刚被重置（commit 开头写入）
            kWaiting     = 1, ///< 已提交 OS 等待，正在 futex 上挂起
            kSignaled    = 2  ///< 已被通知器标记唤醒（park 据此跳过挂起）
        };
        std::atomic<unsigned> state{kNotSignaled};
    };

    // ========================================================================
    //  状态字位操作常量 (布局: [ Epoch (32bit) | Prewaiter (16bit) | Stack (16bit) ])
    // ========================================================================
    // stack 字段：低 16 bit，存 m_waiters 索引；全 1（FFFF）作"空栈"哨兵
    static constexpr std::uint64_t k_stack_bits      = 16;
    static constexpr std::uint64_t k_stack_mask      = (1ULL << k_stack_bits) - 1;                 // 0x0000'0000'0000'FFFF (65535)

    // prewaiter 字段：中 16 bit，记录"已 prepare 但尚未 commit/cancel"的线程数
    static constexpr std::uint64_t k_prewaiter_bits  = 16;
    static constexpr std::uint64_t k_prewaiter_shift = k_stack_bits;                               // 16
    static constexpr std::uint64_t k_prewaiter_mask  = ((1ULL << k_prewaiter_bits) - 1) << k_prewaiter_shift; // 0x0000'0000'FFFF'0000 (4294901760)
    static constexpr std::uint64_t k_prewaiter_inc   = 1ULL << k_prewaiter_shift;                  // 0x0000'0000'0001'0000 (65536)

    // epoch 字段：高 32 bit，全局轮次票号；只增不减，允许回绕（序列号算术）
    static constexpr std::uint64_t k_epoch_bits      = 32;
    static constexpr std::uint64_t k_epoch_shift     = k_stack_bits + k_prewaiter_bits;            // 32
    static constexpr std::uint64_t k_epoch_mask      = ((1ULL << k_epoch_bits) - 1) << k_epoch_shift; // 0xFFFFFFFF'0000'0000 (18446744069414584320)
    static constexpr std::uint64_t k_epoch_inc       = 1ULL << k_epoch_shift;                      // 0x0000'0001'0000'0000 (4294967296)



    // // ========================================================================
    // //  状态字位操作常量 (布局: [ Epoch (24bit) | Prewaiter (20bit) | Stack (20bit) ])
    // // ========================================================================
    // // stack 字段：低 20 bit，存 m_waiters 索引；全 1（0xFFFFF）作"空栈"哨兵
    // static constexpr std::uint64_t k_stack_bits      = 20;
    // static constexpr std::uint64_t k_stack_mask      = (1ULL << k_stack_bits) - 1;                 // 0x0000'0000'000F'FFFF (1048575)

    // // prewaiter 字段：中 20 bit，记录"已 prepare 但尚未 commit/cancel"的线程数
    // static constexpr std::uint64_t k_prewaiter_bits  = 20;
    // static constexpr std::uint64_t k_prewaiter_shift = k_stack_bits;                               // 20
    // static constexpr std::uint64_t k_prewaiter_mask  = ((1ULL << k_prewaiter_bits) - 1) << k_prewaiter_shift; // 0x0000'00FF'FFF0'0000
    // static constexpr std::uint64_t k_prewaiter_inc   = 1ULL << k_prewaiter_shift;                  // 0x0000'0000'0010'0000 (1048576)

    // // epoch 字段：高 24 bit，全局轮次票号；只增不减，允许回绕（序列号算术）
    // static constexpr std::uint64_t k_epoch_bits      = 24;
    // static constexpr std::uint64_t k_epoch_shift     = k_stack_bits + k_prewaiter_bits;            // 40
    // static constexpr std::uint64_t k_epoch_mask      = ((1ULL << k_epoch_bits) - 1) << k_epoch_shift; // 0xFFFF'FF00'0000'0000
    // static constexpr std::uint64_t k_epoch_inc       = 1ULL << k_epoch_shift;                      // 0x0000'0100'0000'0000 (1099511627776)
public:
    // ========================================================================
    //  Construction（构造 / 析构）
    // ========================================================================

    /// @brief 构造一个最多容纳 n 个并发等待者的 Notifier。
    /// @param n 最大并发 worker 数（决定 m_waiters 池大小）
    /// @pre  n < 2^16 - 1 (65535)；prewaiter 与 stack 字段均为 16 bit，n 必须同时
    ///       塞得进"并发 prewaiter 计数"和"栈索引"两个 16 位空间
    explicit Notifier(std::size_t n)
        : m_state{k_stack_mask}  // 初态空栈：stack_top = FFFF，epoch=0，prewaiter=0
        , m_waiters(n)
    {
        TFL_ASSERT(n < (1ULL << k_prewaiter_bits) - 1);
    }

    /// @brief 析构。
    /// @pre  析构时栈必须为空、无残留 prewaiter（所有 worker 已退出协议）。
    ///       故意**不校验 epoch** —— epoch 回绕是合法的，其绝对值无意义。
    ~Notifier() noexcept {
        TFL_ASSERT((m_state.load() & (k_stack_mask | k_prewaiter_mask)) == k_stack_mask);
    }

    // ========================================================================
    //  两阶段等待协议（prepare → commit / cancel）
    // ========================================================================

    /// @brief 阶段一：宣告即将进入等待（两阶段协议的第一步）。
    /// @param wid 绑定到当前线程的 Waiter 索引
    /// @post 调用方紧接着必须再次校验业务谓词，然后调用 commit_wait 或 cancel_wait
    ///
    /// @details
    /// 这是消除 Lost Wake-up 的"前半个 Dekker"。与 notify_* 中的 seq_cst fence
    /// 构成对称的 Store-Buffer（SB）litmus：
    /// @code
    ///   worker:  store(prewaiter) → fence(seq_cst) → load(predicate)
    ///   producer: store(任务/predicate) → fence(seq_cst) → load(prewaiter)
    /// @endcode
    /// 两个 full fence 都进入全序 S，Dekker 论证保证不可能"两边同时读到陈旧的
    /// 0" —— 即不会"worker 读到没活 ∧ producer 读到没等待者"，唤醒必不丢。
    void prepare_wait(std::size_t wid) noexcept {
        // fetch_add 返回**自增前**的快照：既登记自己为 prewaiter（计数 +1），又
        // 让快照天然带上排队顺序（前面有几个 prewaiter）。relaxed 即可 —— 计数
        // 本身不需要序，下一行的 fence 才是 Dekker 的那一极，store 只需 sequenced-
        // before fence。（ARM 上省去把 RMW 升级成 full-barrier RMW 的成本。）
        m_waiters[wid].epoch = m_state.fetch_add(k_prewaiter_inc, std::memory_order_relaxed);

        // Dekker 同步点：强制"本线程登记 prewaiter"与"唤醒方读 prewaiter"严格全序。
        // 彻底关闭 Lost Wake-up 的时序窗口。
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    /// @brief 阶段二（睡）：double-check 仍为假，确认提交等待并挂起线程。
    /// @param wid 当前线程的 Waiter 索引
    /// @pre  调用前业务谓词必须已 double-check 且确认为假
    ///
    /// @details 算法：① 由快照还原目标票号 → ② 按票号排队（自旋等轮到）→
    ///          ③ 把自己压入侵入式栈并推进 epoch → ④ _park 挂起。
    void commit_wait(std::size_t wid) noexcept {
        Waiter* w = &m_waiters[wid];
        // 复位三态机：本次 park 从 NotSignaled 起步。relaxed —— 真正的发布由下方
        // 入栈 CAS 携带；此 store sequenced-before 那次 CAS。
        w->state.store(Waiter::kNotSignaled, std::memory_order_relaxed);

        // 还原目标票号 target = snapshot.epoch + snapshot.prewaiter_count：
        // "全局 epoch 推进到此值时，才轮到我结算"。与 cancel_wait 共用同一公式。
        std::uint64_t epoch = (w->epoch & k_epoch_mask) + (((w->epoch & k_prewaiter_mask) >> k_prewaiter_shift) << k_epoch_shift);

        // seq_cst 读：commit 是会导致睡眠的路径，读陈旧 epoch 有睡死风险，故保守
        // 地放进全序 S（cancel 不睡，用 relaxed 即可 —— 见 cancel_wait）。
        std::uint64_t state = m_state.load(std::memory_order_seq_cst);

        for (;;) {
            // 分支一：当前 epoch < target —— 排在我前面的 prewaiter 还没结算。
            // 序列号算术（RFC 1982 风格）：差值取符号，回绕安全（详见类说明）。
            if (std::int64_t((state & k_epoch_mask) - epoch) < 0) {
                std::this_thread::yield();                       // 让出 CPU
                state = m_state.load(std::memory_order_seq_cst); // 重读后重判
                continue;
            }

            // 分支二：当前 epoch > target —— 已被某个 notify 越过（已被"叫醒"）。
            // 无需入栈、无需挂起，直接返回（撤销工作已被 notify 代劳）。
            if (std::int64_t((state & k_epoch_mask) - epoch) > 0) {
                return;
            }

            // 分支三：epoch == target，正好轮到我。此刻计数里至少有我自己。
            TFL_ASSERT((state & k_prewaiter_mask) != 0);

            // 结算：prewaiter 转正为 epoch 轮次（p--、e++），并把栈顶设为自己 wid。
            std::uint64_t new_state = state - k_prewaiter_inc + k_epoch_inc;
            new_state = (new_state & ~k_stack_mask) | static_cast<std::uint64_t>(wid);

            // 侵入式链表拼接：新节点的 next 指向"旧栈顶"（空栈则 nullptr）。
            // relaxed —— 该 store sequenced-before 下方 release-CAS，落在其 release 链内。
            if ((state & k_stack_mask) == k_stack_mask) {
                w->next.store(nullptr, std::memory_order_relaxed);
            } else {
                w->next.store(&m_waiters[state & k_stack_mask], std::memory_order_relaxed);
            }

            // release：栈链接（next + state 复位）完成前，本节点对唤醒线程不可见。
            if (m_state.compare_exchange_weak(state, new_state, std::memory_order_release)) {
                break;
            }
            // CAS 失败：weak 已把最新值刷回 state，循环重判（可能落入分支二）。
        }

        _park(w); // 真正挂起，等待 _unpark
    }

    /// @brief 阶段二（不睡）：double-check 发现谓词已满足，撤回休眠意图。
    /// @param wid 当前线程的 Waiter 索引
    ///
    /// @details 与 commit_wait 同走票号机，但命中"正好轮到"时**只推进 epoch、
    ///          不入栈**。"必须推进 epoch"是精髓 —— 否则紧排在后面（target+1）的
    ///          线程会永远卡在它的分支一，拖死整条队列。
    void cancel_wait(std::size_t wid) noexcept {
        // 目标票号公式与 commit_wait 完全一致。
        std::uint64_t epoch = (m_waiters[wid].epoch & k_epoch_mask) + (((m_waiters[wid].epoch & k_prewaiter_mask) >> k_prewaiter_shift) << k_epoch_shift);

        // relaxed 读：cancel 根本不会睡，读到陈旧值至多多 yield 几圈再 CAS，无
        // 正确性风险（对比 commit_wait 的 seq_cst）。cancel 不发布任何节点，故
        // 全程 relaxed —— 不存在跨变量发布，epoch 推进的可见性由 m_state 单变量
        // 的 modification order 兜底。
        std::uint64_t state = m_state.load(std::memory_order_relaxed);

        for (;;) {
            // 分支一：还没轮到我 —— 前面的 prewaiter 未结算，自旋等待。
            if (std::int64_t((state & k_epoch_mask) - epoch) < 0) {
                std::this_thread::yield();
                state = m_state.load(std::memory_order_relaxed);
                continue;
            }

            // 分支二：已被越过 —— notify 走 prewaiter 快速路径时做的是
            // (epoch_inc - prewaiter_inc)，**已替我把计数减掉**。我若再减一次会
            // 下溢，故必须静默 return，什么都不做。
            if (std::int64_t((state & k_epoch_mask) - epoch) > 0) {
                return;
            }

            // 分支三：正好轮到我。此刻计数里至少有我自己。
            TFL_ASSERT((state & k_prewaiter_mask) != 0);

            // 撤销：减掉自己的 prewaiter 计数（p--）+ 推进 epoch（e++），栈不动。
            // relaxed 正确且最优 —— 无节点发布，仅调整两个计数字段。
            if (m_state.compare_exchange_weak(
                    state,
                    state - k_prewaiter_inc + k_epoch_inc,
                    std::memory_order_relaxed)) {
                return;
            }
        }
    }

    // ========================================================================
    //  唤醒（notify_one / notify_all / notify_n）
    // ========================================================================

    /// @brief 唤醒一个等待者。
    ///
    /// @details
    /// 优先级：无等待者 → 直接返回；有 prewaiter → 走快速路径只推 epoch（避免
    /// OS 挂起开销）；否则从栈弹出栈顶并 _unpark。开头的 seq_cst fence 是 Dekker
    /// 的另一极（与 prepare_wait 的 fence 配对）。
    void notify_one() noexcept {
        // Dekker 同步点：确保业务数据的修改对所有 worker 可见，且本次"读等待者"
        // 与 worker 的"登记 prewaiter"严格全序。
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::uint64_t state = m_state.load(std::memory_order_acquire);

        for (;;) {
            // 无任何等待者（栈空且无 prewaiter）→ 通知无人可收，直接返回。
            if ((state & k_stack_mask) == k_stack_mask && (state & k_prewaiter_mask) == 0) {
                return;
            }

            std::uint64_t num_pre = (state & k_prewaiter_mask) >> k_prewaiter_shift;
            std::uint64_t new_state;

            if (num_pre) {
                // 快速路径：优先抵消一个 prewaiter。只需推进 epoch 轮次（e++、p--），
                // 那个尚在 commit 自旋的线程会在分支二看到"被越过"而自行放弃挂起。
                new_state = state + k_epoch_inc - k_prewaiter_inc;
            } else {
                // 慢路径：从栈弹出栈顶。先读栈顶节点的 next，算出新栈顶索引。
                Waiter* w = &m_waiters[state & k_stack_mask];
                Waiter* wnext = w->next.load(std::memory_order_relaxed);
                std::uint64_t next = k_stack_mask;             // 默认空栈
                if (wnext != nullptr) {
                    next = static_cast<std::uint64_t>(wnext - &m_waiters[0]); // 指针→索引
                }
                // 仅换栈顶，不动 epoch —— 节点在入栈（commit）时已推进过 epoch。
                new_state = (state & k_epoch_mask) | next;
            }

            // REVIEW: 仅 acquire（无 release）。pop 之后若有线程 re-push 并 read-from
            if (m_state.compare_exchange_weak(state, new_state, std::memory_order_acquire)) {
                if (num_pre) {
                    return; // prewaiter 已被"唤醒"（无需 futex）
                }
                // 慢路径成功：弹出的是旧 state 记录的栈顶（CAS 成功不改 state）。
                Waiter* w = &m_waiters[state & k_stack_mask];
                w->next.store(nullptr, std::memory_order_relaxed); // 摘链，避免悬挂
                _unpark(w);
                return;
            }
            // CAS 失败：state 已刷新，循环重算（栈顶/prewaiter 可能已变）。
        }
    }

    /// @brief 唤醒所有等待者。
    ///
    /// @details 一次 CAS 消耗全部 prewaiter（epoch += num_pre）并清空栈，随后由
    ///          _unpark 沿 next 链一次性唤醒整条睡眠者链。
    void notify_all() noexcept {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::uint64_t state = m_state.load(std::memory_order_acquire);

        for (;;) {
            if ((state & k_stack_mask) == k_stack_mask && (state & k_prewaiter_mask) == 0) {
                return;
            }

            std::uint64_t num_pre = (state & k_prewaiter_mask) >> k_prewaiter_shift;

            // 一次性：epoch 推进 num_pre（消耗所有 prewaiter）+ 栈清空（| FFFF）。
            // 栈中节点入栈时已各自推进过 epoch，故此处只为 prewaiter 补推。
            std::uint64_t new_state =
                ((state & k_epoch_mask) + (k_epoch_inc * num_pre)) | k_stack_mask;

            // REVIEW: 同 notify_one，acquire 单侧。整条链的跨节点遍历对内存序最敏感，
            if (m_state.compare_exchange_weak(state, new_state, std::memory_order_acquire)) {
                if ((state & k_stack_mask) == k_stack_mask) {
                    return; // 只有 prewaiter，无栈节点可 unpark
                }
                // _unpark 从旧栈顶出发，沿 next 链遍历唤醒全部睡眠者。
                Waiter* w = &m_waiters[state & k_stack_mask];
                _unpark(w);
                return;
            }
        }
    }

    /// @brief 唤醒指定数量的等待者。
    /// @param n 欲唤醒的数量（0 直接返回；≥ 池大小等价于 notify_all）
    ///
    /// @details prewaiter 批量消费（一次 CAS 抵消 min(n, num_pre) 个），栈节点
    ///          逐个弹出。CAS 失败时 n 不减、state 刷新后重算。
    void notify_n(std::size_t n) noexcept {
        if (n == 0) [[unlikely]] {
            return;
        }

        if (n >= m_waiters.size()) {
            notify_all();
            return;
        }

        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::uint64_t state = m_state.load(std::memory_order_acquire);

        do {
            if ((state & k_stack_mask) == k_stack_mask && (state & k_prewaiter_mask) == 0) {
                return; // 等待者已耗尽，提前结束
            }

            std::uint64_t num_pre = (state & k_prewaiter_mask) >> k_prewaiter_shift;
            std::uint64_t new_state;
            std::size_t consumed;

            if (num_pre) {
                // 批量消费 prewaiter：一次 CAS 抵消 min(n, num_pre) 个。
                consumed = (std::min)(n, static_cast<std::size_t>(num_pre));
                new_state = state
                            + (k_epoch_inc * consumed)
                            - (k_prewaiter_inc * consumed);
            } else {
                // 从栈弹出一个。
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
                n -= consumed;                 // 仅成功时扣减剩余配额
                if (num_pre == 0) {
                    Waiter* w = &m_waiters[state & k_stack_mask];
                    w->next.store(nullptr, std::memory_order_relaxed);
                    _unpark(w);
                }
            }
            // 注意：CAS 失败时 state 已被刷新，下一轮用最新值重算（n 不变）。
        } while (n > 0);
    }

    // ========================================================================
    //  State queries（状态查询）
    // ========================================================================

    /// @brief 等待者池容量（最大并发 worker 数，运行期不变）。
    [[nodiscard]] std::size_t size() const noexcept { return m_waiters.size(); }

    /// @brief 当前处于 kWaiting（OS 挂起）状态的等待者数量。
    /// @return O(n) 遍历快照，**不保证精确**（并发下随时变化）。
    [[nodiscard]] std::size_t num_waiters() const noexcept {
        std::size_t count = 0;
        for (const auto& w : m_waiters) {
            count += (w.state.load(std::memory_order_relaxed) == Waiter::kWaiting);
        }
        return count;
    }

    /// @brief 栈索引字段可表示的最大等待者数量（编译期常量）。
    /// @return 2^16 - 1 = 65535，由状态字 16-bit stack 字段决定。
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return (1ULL << k_stack_bits) - 1;
    }

private:
    std::atomic<std::uint64_t> m_state;    ///< 64 位全局状态字（epoch | prewaiter | stack）
    std::vector<Waiter> m_waiters;         ///< 等待者池（每个 Worker 一个，构造期定容）

    // ========================================================================
    //  OS 挂起原语（park / unpark）—— 第三层竞争，作用于单个 w->state
    // ========================================================================

    /// @brief 线程挂起（Park）。
    ///
    /// @details
    /// 三态机优化：尝试把 w->state 从 NotSignaled 原子改为 Waiting。
    /// - 成功 → 确实尚未被唤醒 → 进入 futex 等待；
    /// - 失败 → 入栈到挂起之间已被 _unpark 标记 kSignaled → **跳过 OS 挂起**，
    ///   省下一对 futex wait/wake 系统调用（这是 doc 所称关键优化）。
    static void _park(Waiter* w) noexcept {
        unsigned expected = Waiter::kNotSignaled;
        // CAS 与 _unpark 的 exchange 同作用于一个 w->state → 单变量全序，窗口闭合。
        // relaxed：本变量的 RMW 链自带顺序；任务数据可见性已由 eventcount 的
        // seq_cst fence 在 park 之前建立，此处无需额外 acquire/release。
        if (w->state.compare_exchange_strong(expected, Waiter::kWaiting, std::memory_order_relaxed, std::memory_order_relaxed)) {
            w->state.wait(Waiter::kWaiting, std::memory_order_relaxed); // 真正 futex 挂起
        }
    }

    /// @brief 唤醒等待者（Unpark）—— 沿 next 链遍历，逐个唤醒。
    ///
    /// @details exchange 取回旧状态：Waiting → 真睡了，发 futex wake；其他（对方
    ///          还没 _park）→ 跳过，对方的 _park CAS 会失败而自行放弃挂起。
    static void _unpark(Waiter* waiters) noexcept {
        Waiter* next = nullptr;
        for (Waiter* w = waiters; w != nullptr; w = next) {
            // 必须**先读 next 再 exchange**：一旦唤醒，对方 Waiter 可能被线程复用、
            // next 被覆写，届时再读就是悬挂值。
            next = w->next.load(std::memory_order_relaxed);

            if (w->state.exchange(Waiter::kSignaled, std::memory_order_relaxed) == Waiter::kWaiting) {
                w->state.notify_one(); // 唤醒挂起线程
            }
        }
    }
};

} // namespace tfl
