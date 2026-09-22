/// @file spin_mutex.hpp
/// @brief 基于 atomic_flag 的轻量自旋互斥锁。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-09-18
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <atomic>
#include <cstddef>
#include <thread>

#include "macros.hpp"
#include "utility.hpp"

namespace tfl {

/// @brief 面向短临界区的轻量自旋互斥锁。
///
/// SpinMutex 使用 `std::atomic_flag` 保存互斥状态。无竞争时通过一次
/// test_and_set 直接取得锁；发生竞争后先执行有限次数的只读轮询，
/// 若锁持续被占用则主动 yield 当前线程，避免长时间占用 CPU。
///
/// 接口满足标准 mutex 风格的 `lock()`、`try_lock()` 和 `unlock()`，
/// 因而可直接配合 `std::lock_guard`、`std::unique_lock` 等标准 RAII 锁使用。
///
/// @note 适用于临界区非常短且竞争持续时间通常较小的内部同步场景。
/// @note 不保证等待线程之间的公平性，也不提供递归加锁语义。
/// @warning 同一线程重复调用 `lock()` 而未先 `unlock()` 将导致永久等待。
class SpinMutex : public Immovable<SpinMutex> {
public:
    SpinMutex() noexcept = default;

    /// @brief 等待直到成功取得互斥锁。
    ///
    /// 无竞争时直接取得锁；竞争时先进行有限次数只读轮询，
    /// 持续无法取得时通过 `std::this_thread::yield()` 主动让出执行权。
    TFL_FORCE_INLINE void lock() noexcept;

    /// @brief 尝试立即取得互斥锁。
    /// @return 成功取得锁时返回 true，锁当前已被占用时返回 false。
    ///
    /// @note 本函数不会等待，也不会执行 yield。
    [[nodiscard]] TFL_FORCE_INLINE bool try_lock() noexcept;

    /// @brief 释放当前持有的互斥锁。
    /// @pre 当前线程已经成功取得该锁。
    TFL_FORCE_INLINE void unlock() noexcept;

private:
    std::atomic_flag m_flag = ATOMIC_FLAG_INIT;
};

inline void SpinMutex::lock() noexcept {
    if (!m_flag.test_and_set(std::memory_order_acquire)) [[likely]] {
        return;
    }

    std::size_t spin = 1;

    for (;;) {
        while (m_flag.test(std::memory_order_relaxed)) {
            for (std::size_t i = 0; i < spin; ++i) {
                TFL_CPU_RELAX();
            }

            if (spin < 64) [[likely]] {
                spin <<= 1;
            } else {
                std::this_thread::yield();
            }
        }

        if (!m_flag.test_and_set(std::memory_order_acquire)) {
            return;
        }
    }
}

inline bool SpinMutex::try_lock() noexcept {
    return !m_flag.test_and_set(std::memory_order_acquire);
}

inline void SpinMutex::unlock() noexcept {
    m_flag.clear(std::memory_order_release);
}

}  // namespace tfl
