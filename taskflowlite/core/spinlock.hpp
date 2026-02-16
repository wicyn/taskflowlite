#pragma once

#include <atomic>
#include <thread>

#include "macros.hpp"
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace tfl {


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

class alignas(std::hardware_destructive_interference_size) Spinlock {
    std::atomic_flag flag_{};

    static constexpr unsigned kSpinPhase1 = 64;   // 纯自旋次数
    static constexpr unsigned kSpinPhase2 = 256;  // yield 前自旋次数

public:
    Spinlock() = default;
    Spinlock(const Spinlock&) = delete;
    Spinlock& operator=(const Spinlock&) = delete;

    void lock() noexcept {
        if (!flag_.test_and_set(std::memory_order_acquire)) [[likely]] {
            return;
        }
        lock_slow();
    }

    [[nodiscard]] bool try_lock() noexcept {
        // TTAS: 先只读检测，避免无谓的缓存行独占
        return !flag_.test(std::memory_order_relaxed) &&
               !flag_.test_and_set(std::memory_order_acquire);
    }

    void unlock() noexcept {
        flag_.clear(std::memory_order_release);
    }

private:
    [[gnu::noinline]] void lock_slow() noexcept {
        // 阶段1：TTAS 纯自旋
        for (unsigned i = 0; i < kSpinPhase1; ++i) {
            while (flag_.test(std::memory_order_relaxed)) {
                cpu_relax();
            }
            if (!flag_.test_and_set(std::memory_order_acquire)) {
                return;
            }
        }

        // 阶段2：TTAS + 周期性 yield
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

template <typename Mutex>
class [[nodiscard]] ScopedLock {
    Mutex& mtx_;

public:
    explicit ScopedLock(Mutex& m) noexcept : mtx_(m) { mtx_.lock(); }
    ~ScopedLock() { mtx_.unlock(); }

    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
};

template <typename Mutex>
ScopedLock(Mutex&) -> ScopedLock<Mutex>;

}  // namespace tfl
