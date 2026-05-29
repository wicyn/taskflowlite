/// @file  random.hpp
/// @brief SplitMix64 伪随机数生成器 —— Work-Stealing 受害者选择的零开销 PRNG。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cstdint>
#include <limits>

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

namespace tfl {
namespace detail {

// ════════════════════════════════════════════════════════════════════
//  mulhi64: (a × b) >> 64
//
//  设计策略（利用 C++23 if consteval）：
//    编译期 → mulhi64_soft: 纯 32×32 算术，全平台 constexpr
//    运行期 → 按平台选最优路径:
//             GCC/Clang  : unsigned __int128 → 单条 mul 指令
//             MSVC x64   : __umulh intrinsic
//             MSVC ARM64 : __umulh intrinsic
//             其余       : mulhi64_soft fallback
//
//  TFL_MULHI_CONSTEXPR 宏彻底退役，所有调用点统一 constexpr。
// ════════════════════════════════════════════════════════════════════

/// @brief 纯算术实现：4 次 32×32→64 乘法 + 进位传播。
///
/// @details 作为 constexpr 可用的 fallback，同时也是编译期求值的唯一路径。
///   无平台扩展依赖，可在任意 C++23 编译器上常量求值。
[[nodiscard]] constexpr auto mulhi64_soft(std::uint64_t a, std::uint64_t b) noexcept -> std::uint64_t {

    auto const a_lo = static_cast<std::uint64_t>(static_cast<std::uint32_t>(a));
    auto const a_hi = a >> 32;
    auto const b_lo = static_cast<std::uint64_t>(static_cast<std::uint32_t>(b));
    auto const b_hi = b >> 32;

    auto const hi   = a_hi * b_hi;      // 高×高
    auto const mid1 = a_hi * b_lo;      // 高×低
    auto const mid2 = a_lo * b_hi;      // 低×高
    auto const lo   = a_lo * b_lo;      // 低×低

    // 进位：mid1 低 32 位 + mid2 低 32 位 + lo 高 32 位，取溢出到高 64 位
    auto const carry = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(mid1)) +
                        static_cast<std::uint64_t>(static_cast<std::uint32_t>(mid2)) +
                        (lo >> 32)) >> 32;

    return hi + (mid1 >> 32) + (mid2 >> 32) + carry;
}

/// @brief 计算 a × b 的高 64 位（全平台 constexpr，运行期零开销）。
///
/// @details Lemire 的 nearly-divisionless 映射的核心：
///   将 [0, 2^64) 的均匀随机数映射到 [0, n)，只需一次乘法 + 取高位。
///   偏差 < n / 2^64，对 n ≤ 2^32 实际为零。
///
/// @param a 乘数（通常是 SplitMix64 输出）。
/// @param b 乘数（通常是目标范围上界）。
/// @return (a * b) >> 64，即 128 位乘积的高 64 位。
[[nodiscard]] constexpr auto mulhi64(std::uint64_t a, std::uint64_t b) noexcept -> std::uint64_t {

    // C++23 if consteval：编译器保证编译期只走 true 分支，
    // false 分支仅在运行时执行，可安全使用非 constexpr 的 intrinsic。
    if consteval {
        return mulhi64_soft(a, b);
    } else {
#if defined(__SIZEOF_INT128__)
        // GCC / Clang: 编译器内建 128 位整型
        // Why: __extension__ 告诉编译器"我知道这是扩展"，精准消除 -Wpedantic 警告，
        //      作用域天然限制在这一个 typedef 上，无需 pragma push/pop。
        __extension__ typedef unsigned __int128 uint128_t;
        return static_cast<std::uint64_t>((static_cast<uint128_t>(a) * b) >> 64);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
        // MSVC x64 / ARM64: 单条 intrinsic，Release 下编译为 mul / umulh 指令
        return __umulh(a, b);
#else \
    // 兜底：无硬件加速的平台（32 位编译器、WebAssembly 等）
        return mulhi64_soft(a, b);
#endif
    }
}

}  // namespace detail


/// @brief 工作窃取专用 PRNG —— SplitMix64 引擎 + mulhi64 零分支映射。
///
/// 状态仅 8 字节，全周期 2^64，无分支。配合 mulhi64 把 [0, 2^64) 映射到 [0, n)
/// 无需除法/取模/拒绝采样。m_bound 预绑定使热路径零参数调用，汇编约 3 条指令。
/// @note 非线程安全 —— 每个 Worker 独占一份实例，非密码学安全。
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
    //  生成接口（全部 constexpr，热路径运行期走硬件指令）
    // ════════════════════════════════════════════════

    /// @brief 生成 [0, bound) 的随机数（使用预绑定范围，热路径零参数）。
    ///
    /// Work-Stealing 受害者选择的主调用。编译为：
    ///   SplitMix64 混淆 → mulq → movl %edx → ret
    [[nodiscard]] constexpr std::uint32_t operator()() noexcept {
        return static_cast<std::uint32_t>(detail::mulhi64(next(), m_bound));
    }

    /// @brief 生成 [0, n) 的半开区间随机数，允许临时覆盖预绑定范围。
    /// @param n 不包含的上界，实际返回范围为 [0, n)。
    /// @return [0, n) 均匀分布的 uint32_t 随机数。
    [[nodiscard]] constexpr std::uint32_t operator()(std::uint32_t n) noexcept {
        return static_cast<std::uint32_t>(detail::mulhi64(next(), n));
    }

    /// @brief 生成 [lo, hi] 闭区间随机数。
    /// @param lo 下界（包含）。
    /// @param hi 上界（包含）。当 lo==0 && hi==UINT32_MAX 时直接截断 next() 低位，
    ///            避免 range = hi - lo + 1 溢出归零。
    /// @return [lo, hi] 均匀分布的 uint32_t 随机数。
    [[nodiscard]] constexpr std::uint32_t operator()(std::uint32_t lo, std::uint32_t hi) noexcept {
        auto const range = hi - lo + 1;
        if (range == 0) [[unlikely]] {
            // hi == UINT32_MAX && lo == 0 → 全范围，直接截断
            return static_cast<std::uint32_t>(next());
        }
        return lo + static_cast<std::uint32_t>(detail::mulhi64(next(), range));
    }

    /// @brief 生成原始 64 位随机数 [0, UINT64_MAX]。
    [[nodiscard]] constexpr std::uint64_t next() noexcept {
        // SplitMix64: Guy L. Steele Jr., Doug Lea, Christine H. Flood (2014)
        // 单状态、全周期 2^64、通过 BigCrush/PractRand 全项
        m_state += 0x9E3779B97F4A7C15ULL;          // 黄金比例常数
        std::uint64_t z = m_state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

private:
    std::uint64_t m_state = 0x8D0B73B52EA17D89ULL;  ///< 引擎状态（非零默认种子）
    std::uint32_t m_bound = 1;                        ///< 预绑定上界
};

static_assert(tfl::detail::mulhi64(0xFFFFFFFF'FFFFFFFFULL, 2) == 1);
// (2^64 - 1) * (2^64 - 1) 的高 64 位应该是 2^64 - 2
static_assert(tfl::detail::mulhi64(0xFFFFFFFF'FFFFFFFFULL, 0xFFFFFFFF'FFFFFFFFULL) == 0xFFFFFFFF'FFFFFFFEULL);
}  // namespace tfl
