/// @file random.hpp
/// @brief SplitMix64 伪随机数生成器 —— 用于 Work-Stealing 的受害者选择。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

namespace tfl {
namespace detail {

// ============================================================================
// 64 位乘法高半部
// ============================================================================

/// @brief 纯算术实现：4 次 32×32→64 乘法 + 进位传播。
/// @note 不依赖平台扩展，可在 C++20 常量求值中使用。
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

/// @brief 计算 `a × b` 的高 64 位；接口可参与常量求值。
/// @note 可用于将 `[0, 2^64)` 映射到 `[0, n)`；任意两个输出桶对应的输入数量最多相差 1。
/// @param a 乘数（通常是 SplitMix64 输出）。
/// @param b 乘数（通常是目标范围上界）。
/// @return (a * b) >> 64，即 128 位乘积的高 64 位。
[[nodiscard]] constexpr auto mulhi64(std::uint64_t a, std::uint64_t b) noexcept -> std::uint64_t {

    // `if consteval` 可用时直接区分常量求值；C++20 使用 is_constant_evaluated。
    // false 分支仅在运行时执行，可安全使用非 constexpr 的 intrinsic。
#if defined(__cpp_if_consteval) && __cpp_if_consteval >= 202106L
    if consteval {
#else
    if (std::is_constant_evaluated()) {
#endif
        return mulhi64_soft(a, b);
    } else {
#if defined(__SIZEOF_INT128__)
        // GCC / Clang: 编译器内建 128 位整型
        // __extension__ 将该 typedef 标记为已知的编译器扩展，避免 -Wpedantic 警告，
        //      该设置只作用于此 typedef，无需 pragma push/pop。
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


/// @brief 保存 SplitMix64 状态并为工作窃取生成指定整数区间内的随机索引。
///
/// 对象按值拥有种子和可选预绑定上界；每次生成都会推进内部状态，不分配内存。
///
/// @note 实例只供一个 Worker 线程使用，不提供同步，也不适用于密码学或安全令牌。
class SplitMix64 {
public:
    /// @brief 使用固定非零种子和上界 1 构造生成器。
    constexpr SplitMix64() noexcept = default;

    /// @brief 一次性初始化：种子 + 预绑定范围。
    /// @param s  种子值（通常是 hash(thread_id)）。
    /// @param bound 预绑定上界（通常是 num_queues），operator() 返回 [0, bound)。
    /// @pre `bound > 0`。
    constexpr void seed(std::uint64_t s, std::uint32_t bound) noexcept {
        m_state = s;
        m_bound = bound;
    }

    /// @brief 只更新种子，保留已绑定的范围。
    /// @param s 新的 64 位种子。
    constexpr void seed(std::uint64_t s) noexcept { m_state = s; }

    /// @brief 只更新无参生成接口使用的预绑定范围。
    /// @param bound 不包含的上界。
    /// @pre `bound > 0`。
    constexpr void bind(std::uint32_t bound) noexcept { m_bound = bound; }

    // ════════════════════════════════════════════════
    //  生成接口
    // ════════════════════════════════════════════════

    /// @brief 使用预绑定范围生成 `[0, bound)` 的随机数。
    ///
    /// Work-Stealing 受害者选择的主调用。
    /// @return `[0, m_bound)` 内的近似均匀随机数。
    /// @pre 预绑定上界 `m_bound > 0`。
    [[nodiscard]] constexpr std::uint32_t operator()() noexcept {
        return static_cast<std::uint32_t>(detail::mulhi64(next(), m_bound));
    }

    /// @brief 生成 [0, n) 的半开区间随机数，允许临时覆盖预绑定范围。
    /// @param n 不包含的上界，实际返回范围为 [0, n)。
    /// @return `[0, n)` 内的近似均匀随机数。
    /// @pre `n > 0`。
    [[nodiscard]] constexpr std::uint32_t operator()(std::uint32_t n) noexcept {
        return static_cast<std::uint32_t>(detail::mulhi64(next(), n));
    }

    /// @brief 生成 [lo, hi] 闭区间随机数。
    /// @param lo 下界（包含）。
    /// @param hi 上界（包含）。当 lo==0 && hi==UINT32_MAX 时直接截断 next() 低位，
    ///            避免 range = hi - lo + 1 溢出归零。
    /// @return `[lo, hi]` 内的近似均匀随机数。
    /// @pre `lo <= hi`。
    [[nodiscard]] constexpr std::uint32_t operator()(std::uint32_t lo, std::uint32_t hi) noexcept {
        auto const range = hi - lo + 1;
        if (range == 0) [[unlikely]] {
            // hi == UINT32_MAX && lo == 0 → 全范围，直接截断
            return static_cast<std::uint32_t>(next());
        }
        return lo + static_cast<std::uint32_t>(detail::mulhi64(next(), range));
    }

    /// @brief 推进 SplitMix64 状态并生成原始 64 位随机数。
    /// @return `[0, UINT64_MAX]` 范围内的下一输出值。
    [[nodiscard]] constexpr std::uint64_t next() noexcept {
        // SplitMix64: Guy L. Steele Jr., Doug Lea, Christine H. Flood (2014)
        // SplitMix64 的标准状态推进与混合步骤。
        m_state += 0x9E3779B97F4A7C15ULL;          // 黄金比例常数
        std::uint64_t z = m_state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

private:
    std::uint64_t m_state = 0x8D0B73B52EA17D89ULL;  ///< 引擎状态（非零默认种子）。
    std::uint32_t m_bound = 1;                        ///< 预绑定上界。
};

static_assert(tfl::detail::mulhi64(0xFFFFFFFF'FFFFFFFFULL, 2) == 1);
// (2^64 - 1) * (2^64 - 1) 的高 64 位应该是 2^64 - 2
static_assert(tfl::detail::mulhi64(0xFFFFFFFF'FFFFFFFFULL, 0xFFFFFFFF'FFFFFFFFULL) == 0xFFFFFFFF'FFFFFFFEULL);
}  // namespace tfl
