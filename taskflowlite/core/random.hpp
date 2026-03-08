/// @file random.hpp
/// @brief 提供高性能随机数生成器，用于 Work-Stealing 调度中的任务窃取策略。
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include <array>
#include <bit>         // std::rotl (C++20)
#include <concepts>
#include <cstdint>
#include <limits>
#include <random>      // std::uniform_random_bit_generator
#include <type_traits>
#include <utility>

namespace tfl {

// ============================================================================
//  Concepts - 类型约束
// ============================================================================

namespace impl {
/// @brief 内部空标记类型，用于函数重载
struct seed_t {};

/// @brief 检查是否为合格的随机位生成器
template <typename G>
concept uniform_random_bit_generator =
    std::uniform_random_bit_generator<std::remove_cvref_t<G>> &&
    requires { typename std::remove_cvref_t<G>::result_type; } &&
    std::same_as<std::invoke_result_t<std::remove_cvref_t<G>&>,
                 typename std::remove_cvref_t<G>::result_type>;
}  // namespace impl

/// @brief 公开的种子标记
inline constexpr impl::seed_t seed = {};

/// @brief 随机数引擎约束
template <typename G>
concept uniform_random_bit_generator = impl::uniform_random_bit_generator<G>;


// ============================================================================
//  Xoshiro256** PRNG (高性能随机数生成器)
// ============================================================================

/// @brief Xoshiro256** 高性能伪随机数生成器
///
/// 用于 Work-Stealing 调度中的任务窃取策略。
/// 支持大跨步跳跃，避免多线程产生相同的随机序列。
class Xoshiro {
public:
    using result_type = std::uint64_t;

    /// @brief 默认构造函数
    constexpr Xoshiro() = default;

    /// @brief 使用指定种子构造
    /// @param s 包含 4 个 64 位数字的数组
    /// @pre 数组不能全为 0
    explicit constexpr Xoshiro(std::array<result_type, 4> const& s) noexcept
        : m_state{s} {}

    /// @brief 使用随机数引擎初始化
    /// @param dev 随机数引擎
    template <uniform_random_bit_generator PRNG>
        requires (!std::is_const_v<std::remove_reference_t<PRNG>>)
    constexpr Xoshiro(impl::seed_t, PRNG&& dev) noexcept {
        for (std::uniform_int_distribution<result_type> dist{min(), max()}; auto &elem : m_state) {
            elem = static_cast<result_type>(dist(dev));
        }
    }

    [[nodiscard]] static constexpr auto min() noexcept -> result_type { return 0; }

    [[nodiscard]] static constexpr auto max() noexcept -> result_type {
        return std::numeric_limits<result_type>::max();
    }

    /// @brief 摇一次号，给你吐出一个 64 位的随机数字。
    [[nodiscard]] constexpr auto operator()() noexcept -> result_type {
        // Why: 这是 xoshiro256** 算法的灵魂秘方。
        // 全部都是最底层的位移、异或、循环左移操作。CPU 执行这些指令跟喝水一样简单，
        // 既能保证速度飞快，摇出来的数字又足够乱（通过了最严苛的随机性测试）。
        result_type const result = rotl(m_state[1] * 5, 7) * 9;
        result_type const temp = m_state[1] << 17;

        m_state[2] ^= m_state[0];
        m_state[3] ^= m_state[1];
        m_state[1] ^= m_state[2];
        m_state[0] ^= m_state[3];

        m_state[2] ^= temp;
        m_state[3] = rotl(m_state[3], 45);

        return result;
    }

    /// @brief 把状态机往后猛推 2^128 步。
    /// @note 一般用来给不同的线程分配不同的“起始点”，保证每个线程摇出来的数列这辈子都不会重合。
    constexpr void jump() noexcept {
        jump_impl({0x180ec6d33cfd0aba, 0xd5a61266f0c9392c,
                   0xa9582618e03fc9aa, 0x39abdc4529b1661c});
    }

    /// @brief 把状态机往后猛推 2^192 步（比上面的跨度更大）。
    /// @note 一般在搞几千台机器的分布式集群时，给不同的机器分配起始点用。
    constexpr void long_jump() noexcept {
        jump_impl({0x76e15d3efefdcbbf, 0xc5004e441c522fb3,
                   0x77710069854ee241, 0x39109bb02acbe635});
    }

    /// @brief 把现在的四张底牌亮出来看看（可以用来保存进度，下次接着摇）。
    [[nodiscard]] constexpr auto state() const noexcept -> std::array<result_type, 4> const& {
        return m_state;
    }

private:
    // Why: 强行让这 4 个 64 位的数字拼成一个 32 字节的大块，并且在内存里对齐。
    // 这样 CPU 一口吃下去（读取缓存）刚好能把它全吞了，不会发生跨缓存行读取那种坑爹的性能损耗。
    alignas(32) std::array<result_type, 4> m_state = {
        0x8D0B73B52EA17D89, 0x2AA426A407C2B04F,
        0xF513614E4798928A, 0xA65E479EC5B49D41,
    };

    /// @brief 循环左移。其实就是把 C++20 里的 std::rotl 包装了一下。
    [[nodiscard]] static constexpr auto rotl(result_type const val, int const bits) noexcept -> result_type {
        return (val << bits) | (val >> (64 - bits));
    }

    /// @brief 执行跳跃的核心黑魔法。
    constexpr void jump_impl(std::array<result_type, 4> const& jump_array) noexcept {
        result_type s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        for (result_type const jmp : jump_array) {
            for (int bit = 0; bit < 64; ++bit) {
                if (jmp & (result_type{1} << bit)) {
                    s0 ^= m_state[0];
                    s1 ^= m_state[1];
                    s2 ^= m_state[2];
                    s3 ^= m_state[3];
                }
                (void)(*this)();
            }
        }
        m_state = {s0, s1, s2, s3};
    }
};

static_assert(uniform_random_bit_generator<Xoshiro>);


// ============================================================================
//  uniform_int_distribution (把随机数塞进你想要的范围里)
// ============================================================================

/// @brief 极速均匀分配器。
///
/// 它的作用是：上面的 Xoshiro 摇出来的数字巨大无比，你如果只想摇个 1 到 10 之间的数，就得靠它来转换。
/// 这里用的是大神 Lemire 搞出来的“几乎不用除法”的映射算法。大多数情况下根本不用做慢吞吞的除法运算，
/// 比 C++ 标准库自带的那个分配器要快将近 20%。
///
/// @tparam T 你想要的整数类型（必须是没有负号的 unsigned 类型）。
template <std::unsigned_integral T = std::uint64_t>
class uniform_int_distribution {
public:
    using result_type = T;

    /// @brief 默认构造，不限制范围，摇出来多大就多大。
    constexpr uniform_int_distribution() noexcept
        : _min(0), _range(0) {}

    /// @brief 告诉它你想要的范围，比如 [1, 100]。
    constexpr uniform_int_distribution(T min_, T max_) noexcept { reset(min_, max_); }

    /// @brief 塞个随机数引擎给它，让它吐出一个在你规定范围内的数字。
    template <typename URBG>
        requires std::same_as<std::invoke_result_t<URBG&>, result_type>
    [[nodiscard]] constexpr result_type operator()(URBG& rng) const noexcept {
        if (_range == 0) [[unlikely]] {
            return rng();
        }
        return _min + bounded(rng(), _range, rng);
    }

    [[nodiscard]] constexpr result_type a() const noexcept { return _min; }
    [[nodiscard]] constexpr result_type b() const noexcept {
        return _range == 0 ? std::numeric_limits<T>::max() : _min + _range - 1;
    }
    [[nodiscard]] constexpr result_type min() const noexcept { return a(); }
    [[nodiscard]] constexpr result_type max() const noexcept { return b(); }

    /// @brief 临时换个新范围。
    constexpr void reset(T min_, T max_) noexcept {
        if (min_ > max_) std::swap(min_, max_);
        _min = min_;
        _range = max_ - min_ + 1; // 算出来的跨度。如果是 0 就代表全量程。
    }

private:
    result_type _min;
    result_type _range;

    /// @brief Lemire 大神的“无除法”映射黑魔法。
    template <typename URBG>
    [[nodiscard]] static constexpr result_type
    bounded(result_type x, result_type s, URBG& rng) noexcept {
        // Why: 抄近道。如果你的范围刚好是 2 的次方（比如 2, 4, 8, 1024），
        // 那根本不用算，直接用位运算的“与”操作切一刀就行了，速度快到飞起。
        // [[unlikely]] 是告诉编译器，这种情况虽然爽，但不常有，别为了它打乱主线逻辑。
        if ((s & (s - 1)) == 0) [[unlikely]] {
            return x & (s - 1);
        }

#if defined(__SIZEOF_INT128__)
        // Why: 在 Linux/GCC 下，直接用 128 位超宽寄存器做乘法。
        // 乘出来的结果，上半截直接当商（结果），下半截当余数。
        // 只要余数不掉进坑里（小于拒绝阈值），就根本不用做任何除法，直接拿着上半截跑路。
        using Wide = unsigned __int128;
        Wide m = Wide(x) * s;
        auto lo = static_cast<result_type>(m);

        if (lo < s) [[unlikely]] {
            result_type threshold = -s % s;
            while (lo < threshold) {
                x = rng();
                m = Wide(x) * s;
                lo = static_cast<result_type>(m);
            }
        }
        return static_cast<result_type>(m >> 64);

#elif defined(_MSC_VER) && defined(_M_X64)
        // Why: 在 Windows/MSVC 下，没法直接写 128 位变量。
        // 但我们可以调用微软提供的 _umul128 汇编特权指令，效果跟上面一模一样。
        unsigned __int64 hi, lo;
        lo = _umul128(x, s, &hi);

        if (lo < s) [[unlikely]] {
            result_type threshold = -s % s;
            while (lo < threshold) {
                x = rng();
                lo = _umul128(x, s, &hi);
            }
        }
        return static_cast<result_type>(hi);

#else  \
        // Why: 要是不幸跑在了 32 位老爷机或者稀奇古怪的平台上， \
        // 那只能老老实实退回去用经典的求模取余法（慢是慢了点，但安全）。
        result_type threshold = -s % s;
        while (x < threshold) {
            x = rng();
        }
        return x % s;
#endif
    }
};

using uniform_uint64_distribution = uniform_int_distribution<std::uint64_t>;
using uniform_uint32_distribution = uniform_int_distribution<std::uint32_t>;

}  // namespace tfl
