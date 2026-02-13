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
//                              Concepts
// ============================================================================

namespace impl {
struct seed_t {};

template <typename G>
concept uniform_random_bit_generator =
    std::uniform_random_bit_generator<std::remove_cvref_t<G>> &&
    requires { typename std::remove_cvref_t<G>::result_type; } &&
    std::same_as<std::invoke_result_t<std::remove_cvref_t<G>&>,
                 typename std::remove_cvref_t<G>::result_type>;
}  // namespace impl

/// 用于区分种子构造的标签
inline constexpr impl::seed_t seed = {};

template <typename G>
concept uniform_random_bit_generator = impl::uniform_random_bit_generator<G>;

// ============================================================================
//                           Xoshiro256** PRNG
// ============================================================================

/**
 * @brief Xoshiro256** 1.0 - 高性能伪随机数生成器
 *
 * 特点：
 *   - 速度：亚纳秒级（~1-2 ns/call）
 *   - 状态：256-bit（适合任何并行应用）
 *   - 质量：通过所有已知统计测试
 *   - 周期：2^256 - 1
 *
 * @see https://prng.di.unimi.it/
 */
class Xoshiro {
public:
    using result_type = std::uint64_t;

    constexpr Xoshiro() = default;

    /// 从 4 个 uint64_t 种子构造（不能全为 0）
    explicit constexpr Xoshiro(std::array<result_type, 4> const& s) noexcept
        : m_state{s} {}

    /// 从另一个 PRNG 种子化
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

    /// 生成下一个随机数
    // [[nodiscard]] constexpr auto operator()() noexcept -> result_type {
    //     auto s0 = m_state[0], s1 = m_state[1], s2 = m_state[2], s3 = m_state[3];

    //     result_type const result = std::rotl(s1 * 5, 7) * 9;
    //     result_type const t = s1 << 17;

    //     s2 ^= s0;
    //     s3 ^= s1;
    //     s1 ^= s2;
    //     s0 ^= s3;
    //     s2 ^= t;
    //     s3 = std::rotl(s3, 45);

    //     m_state = {s0, s1, s2, s3};
    //     return result;
    // }


    [[nodiscard]] constexpr auto operator()() noexcept -> result_type {

        result_type const result = rotl(m_state[1] * 5, 7) * 9;
        result_type const temp = m_state[1] << 17; // NOLINT

        m_state[2] ^= m_state[0];
        m_state[3] ^= m_state[1];
        m_state[1] ^= m_state[2];
        m_state[0] ^= m_state[3];

        m_state[2] ^= temp;

        m_state[3] = rotl(m_state[3], 45); // NOLINT (magic-numbers)

        return result;
    }

    /// 跳跃 2^128 步（用于生成并行子序列）
    constexpr void jump() noexcept {
        jump_impl({0x180ec6d33cfd0aba, 0xd5a61266f0c9392c,
                   0xa9582618e03fc9aa, 0x39abdc4529b1661c});
    }

    /// 跳跃 2^192 步（用于分布式计算）
    constexpr void long_jump() noexcept {
        jump_impl({0x76e15d3efefdcbbf, 0xc5004e441c522fb3,
                   0x77710069854ee241, 0x39109bb02acbe635});
    }

    /// 获取当前状态（用于序列化）
    [[nodiscard]] constexpr auto state() const noexcept -> std::array<result_type, 4> const& {
        return m_state;
    }

private:
    alignas(32) std::array<result_type, 4> m_state = {
        0x8D0B73B52EA17D89, 0x2AA426A407C2B04F,
        0xF513614E4798928A, 0xA65E479EC5B49D41,
    };


    [[nodiscard]] static constexpr auto rotl(result_type const val, int const bits) noexcept -> result_type {
        return (val << bits) | (val >> (64 - bits)); // NOLINT
    }

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
//                    uniform_int_distribution
// ============================================================================

/**
 * @brief 高性能均匀整数分布 [min, max]
 *
 * 算法：Lemire's Nearly Divisionless (2019)
 *   - 大多数调用零拒绝（>99.9999%）
 *   - 用 128-bit 乘法代替除法
 *   - 2^k 范围使用位掩码快速路径
 *
 * 优势：
 *   - noexcept 保证
 *   - constexpr 支持
 *   - 比传统拒绝采样快 ~20%
 *
 * @see https://arxiv.org/abs/1805.10941
 */
template <std::unsigned_integral T = std::uint64_t>
class uniform_int_distribution {
public:
    using result_type = T;

    /// 默认构造：全范围 [0, MAX]
    constexpr uniform_int_distribution() noexcept
        : _min(0), _range(0) {}

    /// 构造 [min_, max_] 范围的分布
    constexpr uniform_int_distribution(T min_, T max_) noexcept { reset(min_, max_); }

    /// 生成随机数
    template <typename URBG>
        requires std::same_as<std::invoke_result_t<URBG&>, result_type>
    [[nodiscard]] constexpr result_type operator()(URBG& rng) const noexcept {
        if (_range == 0) [[unlikely]] {
            return rng();  // 全范围
        }
        return _min + bounded(rng(), _range, rng);
    }

    [[nodiscard]] constexpr result_type a() const noexcept { return _min; }
    [[nodiscard]] constexpr result_type b() const noexcept {
        return _range == 0 ? std::numeric_limits<T>::max() : _min + _range - 1;
    }
    [[nodiscard]] constexpr result_type min() const noexcept { return a(); }
    [[nodiscard]] constexpr result_type max() const noexcept { return b(); }

    /// 重置范围
    constexpr void reset(T min_, T max_) noexcept {
        if (min_ > max_) std::swap(min_, max_);
        _min = min_;
        _range = max_ - min_ + 1;  // 溢出时为 0
    }

private:
    result_type _min;
    result_type _range;

    /// Lemire's nearly divisionless bounded random
    template <typename URBG>
    [[nodiscard]] static constexpr result_type
    bounded(result_type x, result_type s, URBG& rng) noexcept {
        // 2^k 快速路径（概率较低，但值得优化）
        if ((s & (s - 1)) == 0) [[unlikely]] {
            return x & (s - 1);
        }

#if defined(__SIZEOF_INT128__)
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

#else \
        // Fallback: 传统拒绝采样
        result_type threshold = -s % s;
        while (x < threshold) {
            x = rng();
        }
        return x % s;
#endif
    }
};

// 类型别名
using uniform_uint64_distribution = uniform_int_distribution<std::uint64_t>;
using uniform_uint32_distribution = uniform_int_distribution<std::uint32_t>;

}  // namespace tfl

