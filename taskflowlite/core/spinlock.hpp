/// @file spinlock.hpp
/// @brief 提供底层的硬件级 CPU 暂停指令与高性能自旋锁实现。
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include <atomic>
#include <thread>

#include "macros.hpp"
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace tfl {

/// @brief 跨平台的底层 CPU 暂停指令封装。
///
/// 在自旋等待循环（Spin-wait Loop）中调用此函数，可以向处理器暗示当前正处于自旋状态。
// Why:
// 1. 避免由于频繁的内存重排导致的流水线清空（Pipeline Flush）开销。
// 2. 降低自旋时的功耗，缓解内存总线压力。
// 3. 在超线程（Hyper-Threading）架构中，主动让渡执行资源给同核的兄弟线程。
TFL_FORCE_INLINE void cpu_relax() noexcept {
#if defined(_MSC_VER)
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("isb sy" ::: "memory");  // yield 效果太弱，isb 更可靠
#elif defined(__arm__)
    asm volatile("yield" ::: "memory");
#elif defined(__powerpc__) || defined(__ppc__)
    asm volatile("or 27,27,27" ::: "memory");  // PPC yield hint
#elif defined(__riscv)
    asm volatile(".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");  // pause
#else
    // 最后手段：阻止编译器过度优化循环
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// ============================================================================
// Spinlock：TTAS + 两阶段退避
// ============================================================================

/// @brief 基于 TTAS (Test-Test-And-Set) 与两阶段退避策略的高性能自旋锁。
///
/// 适用于临界区极小、竞争时间极短的并发保护场景。
///
// Why: 使用 alignas 强制按照硬件的破坏性干扰尺寸（通常为 64 或 128 字节）进行对齐。
// 这是无锁/自旋锁设计的铁律，彻底杜绝多线程操作不同锁实例时由于共享同一缓存行而引发的“伪共享（False Sharing）”性能雪崩。
class alignas(std::hardware_destructive_interference_size) Spinlock {
    std::atomic_flag flag_{};

    static constexpr unsigned kSpinPhase1 = 64;   ///< 第一阶段：纯自旋次数阈值
    static constexpr unsigned kSpinPhase2 = 256;  ///< 第二阶段：穿插 yield 前的自旋次数阈值

public:
    /// @brief 默认构造函数，初始化为未锁状态。
    Spinlock() = default;

    Spinlock(const Spinlock&) = delete;
    Spinlock& operator=(const Spinlock&) = delete;

    /// @brief 阻塞获取锁。
    /// @note 优先尝试无争用情况下的极速 TAS 路径。
    void lock() noexcept {
        // Why: 使用 [[likely]] 优化分支预测。在低争用场景下，锁极大概率能一次性获取成功，
        // 从而将快速路径的汇编指令以最紧凑的方式排列，避免跳转开销。
        if (!flag_.test_and_set(std::memory_order_acquire)) [[likely]] {
            return;
        }
        lock_slow();
    }

    /// @brief 尝试非阻塞获取锁。
    /// @return 成功获取返回 true，否则立即返回 false。
    [[nodiscard]] bool try_lock() noexcept {
        // Why: 经典的 TTAS (Test, Test-And-Set) 优化。
        // 先进行一次代价极低的只读探查（relaxed）。若已被锁，则直接返回。
        // 这避免了盲目执行代价高昂的 test_and_set，防止在底层触发 MESI 缓存一致性协议的
        // 独占（Exclusive）无效化广播（Invalidate Broadcast），从而保护内存总线带宽。
        return !flag_.test(std::memory_order_relaxed) &&
               !flag_.test_and_set(std::memory_order_acquire);
    }

    /// @brief 释放锁。
    void unlock() noexcept {
        // Why: release 内存序建立同步屏障，确保在解锁前临界区内的所有内存写入操作
        // 对于随后获取该锁的线程完全可见。
        flag_.clear(std::memory_order_release);
    }

private:
    /// @brief 锁获取失败后的两阶段自旋退避慢路径。
    // Why: 显式阻止编译器内联。将慢路径代码移出热点缓存（I-Cache），
    // 保证 lock() 函数的快速路径绝对精简。
    [[gnu::noinline]] void lock_slow() noexcept {
        // 阶段 1：TTAS 纯自旋
        // Why: 在争用的最初阶段，预期持有者会极快释放锁。因此进行无系统调用的密集自旋，追求微秒级的极低延迟。
        for (unsigned i = 0; i < kSpinPhase1; ++i) {
            while (flag_.test(std::memory_order_relaxed)) {
                cpu_relax();
            }
            if (!flag_.test_and_set(std::memory_order_acquire)) {
                return;
            }
        }

        // 阶段 2：TTAS + 周期性 yield
        // Why: 如果短时间内未能获取，说明可能遭遇了系统级抢占或长耗时临界区。
        // 此时在自旋一定次数后主动调用 yield()，将当前时间片让渡给操作系统调度器，防止 CPU 算力被白白榨干或引发线程饥饿死锁。
        for (;;) {
            for (unsigned i = 0; i < kSpinPhase2; ++i) {
                while (flag_.test(std::memory_order_relaxed)) {
                    cpu_relax();
                }
                if (!flag_.test_and_set(std::memory_order_acquire)) {
                    return;
                }
            }
            std::this_thread::yield();
        }
    }
};

// ============================================================================
// 作用域锁守卫
// ============================================================================

/// @brief 符合 RAII 语义的异常安全局部锁守卫。
/// @tparam Mutex 满足 BasicLockable 概念的互斥量或自旋锁类型。
// Why: 使用 [[nodiscard]] 强制要求必须将其实例化为命名变量。
// 避免开发者因笔误写出诸如 `ScopedLock(mtx);` 的代码——这会创建一个纯右值临时对象，
// 在该语句结束时立刻析构解锁，导致后续的临界区完全处于裸奔状态。
template <typename Mutex>
class [[nodiscard]] ScopedLock {
    Mutex& mtx_;

public:
    /// @brief 构造并立即获取锁。
    explicit ScopedLock(Mutex& m) noexcept : mtx_(m) { mtx_.lock(); }

    /// @brief 析构并自动释放锁。
    ~ScopedLock() { mtx_.unlock(); }

    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
};

/// @brief 用户自定义推导指南 (CTAD)。
/// @note 允许在 C++17 及以上版本中省略模板参数，直接使用 `ScopedLock lock(mtx);`。
template <typename Mutex>
ScopedLock(Mutex&) -> ScopedLock<Mutex>;

}  // namespace tfl
