/// @file random.hpp
/// @brief 提供轻量级随机数生成，用于 Work-Stealing 调度中的受害者选择。
/// @details
///   - **引擎**: SplitMix64（单 uint64_t 状态，通过 BigCrush 全项）
///   - **映射**: 64×32 → 128 位乘法取高位，零分支，偏差 < n/2^64 ≈ 0
///   - **跨平台**: GCC/Clang 用 __int128，MSVC 用 _umul128/__umulh
///
/// @code
///   tfl::SplitMix64 rng;
///   rng.seed(hash_value, num_queues);  // 一次初始化
///
///   auto victim = rng();               // [0, num_queues)  预绑定范围，热路径零参数
///   auto val    = rng(100);            // [0, 100)         临时指定范围
///   auto raw    = rng.next();          // [0, UINT64_MAX]  原始 64 位
/// @endcode
///
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-22
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cstdint>
#include <limits>

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

namespace tfl {

#if defined(__SIZEOF_INT128__)
#  define TFL_MULHI_CONSTEXPR constexpr
#else
#  define TFL_MULHI_CONSTEXPR inline
#endif

namespace detail {

/// @brief 计算 a * b 的高 64 位（跨平台）。
[[nodiscard]] TFL_MULHI_CONSTEXPR auto
mulhi64(std::uint64_t a, std::uint64_t b) noexcept -> std::uint64_t {
#if defined(__SIZEOF_INT128__)
    return static_cast<std::uint64_t>(
        (static_cast<unsigned __int128>(a) * b) >> 64
        );
#elif defined(_MSC_VER) && defined(_M_X64)
    std::uint64_t hi;
    (void)_umul128(a, b, &hi);
    return hi;
#elif defined(_MSC_VER) && defined(_M_ARM64)
    return __umulh(a, b);
#else
    auto const a_lo = static_cast<std::uint32_t>(a);
    auto const a_hi = static_cast<std::uint32_t>(a >> 32);
    auto const b_lo = static_cast<std::uint32_t>(b);
    auto const b_hi = static_cast<std::uint32_t>(b >> 32);
    std::uint64_t const lo_lo = static_cast<std::uint64_t>(a_lo) * b_lo;
    std::uint64_t const lo_hi = static_cast<std::uint64_t>(a_lo) * b_hi;
    std::uint64_t const hi_lo = static_cast<std::uint64_t>(a_hi) * b_lo;
    std::uint64_t const hi_hi = static_cast<std::uint64_t>(a_hi) * b_hi;
    std::uint64_t const cross =
        (lo_lo >> 32) +
        static_cast<std::uint32_t>(lo_hi) +
        static_cast<std::uint32_t>(hi_lo);
    return hi_hi + (lo_hi >> 32) + (hi_lo >> 32) + (cross >> 32);
#endif
}

}  // namespace detail


/// @brief Work-Stealing 专用随机数生成器。
///
/// SplitMix64 引擎 + mulhi64 零分支映射。
/// 预绑定 bound，无参 operator() 直接返回 [0, bound)——
/// 热路径零参数传递、零分支、零除法。
///
/// @par 内存布局（12 字节，无 padding）
/// @code
///   offset 0: m_state  (uint64_t, 8B)
///   offset 8: m_bound  (uint32_t, 4B)
/// @endcode
///
/// @note 非线程安全，每个 Worker 持有独立实例。
class SplitMix64 {
public:
    constexpr SplitMix64() noexcept = default;

    /// @brief 一次性初始化：种子 + 预绑定范围。
    /// @param s  种子值（通常是 hash(thread_id)）
    /// @param bound 预绑定上界（通常是 num_queues），operator() 返回 [0, bound)
    constexpr void seed(std::uint64_t s, std::uint32_t bound) noexcept {
        m_state = s;
        m_bound = bound;
    }

    /// @brief 只更新种子，保留已绑定的 bound。
    constexpr void seed(std::uint64_t s) noexcept { m_state = s; }

    /// @brief 只更新预绑定范围。
    constexpr void bind(std::uint32_t bound) noexcept { m_bound = bound; }

    // ════════════════════════════════════════════════
    //  生成接口
    // ════════════════════════════════════════════════

    /// @brief 生成 [0, bound) 的随机数（使用预绑定范围，热路径零参数）。
    ///
    /// Work-Stealing 受害者选择的主调用。编译为：
    ///   SplitMix64 混淆 → mulq → movl %edx → ret
    [[nodiscard]] TFL_MULHI_CONSTEXPR auto operator()() noexcept -> std::uint32_t {
        return static_cast<std::uint32_t>(detail::mulhi64(next(), m_bound));
    }

    /// @brief 生成 [0, n) 的随机数（临时指定范围）。
    [[nodiscard]] TFL_MULHI_CONSTEXPR auto operator()(std::uint32_t n) noexcept -> std::uint32_t {
        return static_cast<std::uint32_t>(detail::mulhi64(next(), n));
    }

    /// @brief 生成 [lo, hi] 闭区间随机数。
    [[nodiscard]] TFL_MULHI_CONSTEXPR auto operator()(std::uint32_t lo, std::uint32_t hi) noexcept -> std::uint32_t {
        auto const range = hi - lo + 1;
        if (range == 0) [[unlikely]] {
            return static_cast<std::uint32_t>(next());
        }
        return lo + static_cast<std::uint32_t>(detail::mulhi64(next(), range));
    }

    /// @brief 生成原始 64 位随机数 [0, UINT64_MAX]。
    [[nodiscard]] constexpr auto next() noexcept -> std::uint64_t {
        m_state += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = m_state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

private:
    std::uint64_t m_state = 0x8D0B73B52EA17D89ULL;  ///< 引擎状态
    std::uint32_t m_bound = 1;                        ///< 预绑定上界
};

#undef TFL_MULHI_CONSTEXPR

}  // namespace tfl
