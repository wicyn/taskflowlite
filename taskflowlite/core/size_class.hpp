/// @file size_class.hpp
/// @brief Size-class 分档方案 trait 与内置方案（PowerOfTwo / Subdivided / Hybrid / Table）。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <type_traits>

namespace tfl {

namespace detail {

// ============================================================================
//  type traits
// ============================================================================

/// @brief 检测 T 是否为 std::array<std::size_t, N> 特化。
template <typename T>
struct is_size_array : std::false_type {};

template <std::size_t N>
struct is_size_array<std::array<std::size_t, N>> : std::true_type {};

/// @brief is_size_array 的变量模板别名，自动剥离 cvref 限定符后判定是否为 std::array<std::size_t, N>。
template <typename T>
inline constexpr bool is_size_array_v = is_size_array<std::remove_cvref_t<T>>::value;


// ============================================================================
//  concepts
// ============================================================================

/// @brief Scheme 通过 class_sizes() 静态成员返回 std::array<std::size_t, N>。
template <typename S>
concept HasClassSizes = requires {
    requires is_size_array_v<decltype(S::class_sizes())>;
};

/// @brief Scheme 提供 class_count() -> std::size_t 编译期查询。
template <typename S>
concept HasClassCount = requires {
    { S::class_count() } noexcept -> std::same_as<std::size_t>;
};

/// @brief Scheme 提供 class_size(idx) -> std::size_t 按索引查大小。
template <typename S>
concept HasClassSize = requires(std::size_t idx) {
    { S::class_size(idx) } noexcept -> std::same_as<std::size_t>;
};

/// @brief Scheme 提供 class_index(bytes) -> std::size_t 按字节数查档位。
template <typename S>
concept HasClassIndex = requires(std::size_t bytes) {
    { S::class_index(bytes) } noexcept -> std::same_as<std::size_t>;
};

/// @brief Scheme 同时提供 class_count、class_size、class_index 完整 API。
template <typename S>
concept HasFullClassApi = HasClassCount<S> && HasClassSize<S> && HasClassIndex<S>;


}  // namespace detail


// ============================================================================
//  SizeClassScheme
// ============================================================================

/// @brief 任意合法 size-class scheme。
template <typename S>
concept SizeClassScheme = detail::HasClassSizes<S> || detail::HasFullClassApi<S>;


// ============================================================================
//  SizeClassTraits
// ============================================================================

/// @brief 统一 scheme traits，自动补全两种风格（class_sizes() / 完整 API）之间的差异。
///
/// @tparam S 任意 SizeClassScheme，通过 constexpr if 分支选择调用路径。
template <SizeClassScheme S>
struct SizeClassTraits {
private:
    /// @brief 低分支预测干扰的 lower_bound: 通过算术乘法替代 if-else,减少分支预测失败。
    template <typename Arr>
    [[nodiscard]] static constexpr std::size_t lb_pos(const Arr& arr, std::size_t bytes) noexcept {
        std::size_t lo  = 0;
        std::size_t len = arr.size();
        while (len > 1) {
            const std::size_t half = len / 2;
            lo  += (arr[lo + half - 1] < bytes) * half;
            len -= half;
        }
        return lo;
    }

public:
    /// @brief 返回 size-class 方案中的总档位数（编译期常量）。
    /// @return 若方案提供 class_count() 则直接委托，否则从 class_sizes() 数组长度推导。
    [[nodiscard]] static constexpr std::size_t class_count() noexcept {
        if constexpr (detail::HasClassCount<S>) {
            return S::class_count();
        } else {
            return S::class_sizes().size();
        }
    }

    /// @brief 根据字节数查找对应的 size-class 索引。
    /// @param bytes 待分配的对象字节数。
    /// @return 方案中 >= bytes 的最小档位索引。
    /// @pre bytes 必须在 [min_size(), max_size()] 范围内。
    [[nodiscard]] static constexpr std::size_t class_index(std::size_t bytes) noexcept {
        if constexpr (detail::HasClassIndex<S>) {
            return S::class_index(bytes);
        } else {
            constexpr auto arr = S::class_sizes();
            return lb_pos(arr, bytes);
        }
    }

    /// @brief 根据档位索引查询该档对应的实际分配字节数。
    /// @param idx 档位索引，必须 < class_count()。
    /// @return 该档位的实际分配大小（chunk size）。
    /// @pre idx < class_count()
    [[nodiscard]] static constexpr std::size_t class_size(std::size_t idx) noexcept {
        if constexpr (detail::HasClassSize<S>) {
            return S::class_size(idx);
        } else {
            return S::class_sizes()[idx];
        }
    }

    /// @brief 获取方案中最小档位的 chunk 大小（编译期常量）。
    /// @return class_size(0) 的值。
    [[nodiscard]] static constexpr std::size_t min_size() noexcept {
        return class_size(0);
    }

    /// @brief 获取方案中最大档位的 chunk 大小（编译期常量）。
    /// @return class_size(class_count() - 1) 的值。
    [[nodiscard]] static constexpr std::size_t max_size() noexcept {
        return class_size(class_count() - 1);
    }
};

// ============================================================================
//  TableScheme
// ============================================================================
/// @brief 表驱动 size-class scheme —— 用户自定义 std::array<std::size_t, N> 引用传入。
///
/// @tparam Sizes 全局 inline constexpr std::array，需有 linkage、严格单调递增、首元素 >= sizeof(void*)。
template <auto& Sizes>
struct TableScheme {
    [[nodiscard]] static constexpr auto class_sizes() noexcept { return Sizes; }
};

namespace detail {
/// @brief 仅用于 concept 自检的探针数组,外部勿用。
inline constexpr std::array<std::size_t, 4> kProbeSizes{16, 32, 64, 128};
}  // namespace detail

static_assert(SizeClassScheme<TableScheme<detail::kProbeSizes>>, "TableScheme must satisfy SizeClassScheme");

// ============================================================================
//  PowerOfTwoScheme
// ============================================================================

/// @brief 2^N size-class scheme。
///
/// @tparam MinSize 最小类大小（向下对齐到 bit_floor），默认 16
/// @tparam MaxSize 最大类大小（向上对齐到 bit_ceil），默认 4096
///
/// class 序列: MinSize, MinSize*2, MinSize*4, ..., MaxSize。class_index/class_size 全部位运算。
template <std::size_t MinSize = 16, std::size_t MaxSize = 4096>
    requires (MinSize >= sizeof(void*)) && (MinSize < MaxSize)
struct PowerOfTwoScheme {
private:
    /// @brief 对齐后的最小类大小。向下对齐 (bit_floor),保证 k_min <= MinSize。
    /// 用户传入的 MinSize 不必是 2 的幂,内部统一按 2^N 处理以维持位运算闭环。
    static constexpr std::size_t k_min = std::bit_floor(MinSize);

    /// @brief 对齐后的最大类大小。向上对齐 (bit_ceil),保证 k_max >= MaxSize。
    /// 必须覆盖用户请求的 MaxSize,所以向上取整。
    static constexpr std::size_t k_max = std::bit_ceil(MaxSize);

    static constexpr std::size_t k_min_shift = static_cast<std::size_t>(std::countr_zero(k_min));
    static constexpr std::size_t k_max_shift = static_cast<std::size_t>(std::countr_zero(k_max));
    static constexpr std::size_t k_count     = k_max_shift - k_min_shift + 1;

public:
    static constexpr std::size_t min_size = k_min;
    static constexpr std::size_t max_size = k_max;

    [[nodiscard]] static constexpr std::size_t class_count() noexcept {
        return k_count;
    }

    [[nodiscard]] static constexpr std::size_t class_index(std::size_t bytes) noexcept {
        return static_cast<std::size_t>(std::countr_zero(std::bit_ceil(bytes))) - k_min_shift;
    }

    [[nodiscard]] static constexpr std::size_t class_size(std::size_t idx) noexcept {
        return k_min << idx;
    }

};
static_assert(SizeClassScheme<PowerOfTwoScheme<>>, "PowerOfTwoScheme must satisfy SizeClassScheme");

namespace tfl::detail::pow2_test {

using P = PowerOfTwoScheme<16, 4096>;

// ---- 元信息 ----
static_assert(P::min_size == 16);
static_assert(P::max_size == 4096);
static_assert(P::class_count() == 9);

// ---- class_size 表 ----
static_assert(P::class_size(0) == 16);
static_assert(P::class_size(1) == 32);
static_assert(P::class_size(2) == 64);
static_assert(P::class_size(3) == 128);
static_assert(P::class_size(4) == 256);
static_assert(P::class_size(5) == 512);
static_assert(P::class_size(6) == 1024);
static_assert(P::class_size(7) == 2048);
static_assert(P::class_size(8) == 4096);

// ---- class_index: 精确 2 幂 ----
static_assert(P::class_index(16)   == 0);
static_assert(P::class_index(32)   == 1);
static_assert(P::class_index(64)   == 2);
static_assert(P::class_index(128)  == 3);
static_assert(P::class_index(1024) == 6);
static_assert(P::class_index(4096) == 8);

// ---- class_index: 任意 bytes in [min_size, max_size] -> >=bytes 最小 2 幂 ----
static_assert(P::class_index(17)   == 1);   // -> 32
static_assert(P::class_index(33)   == 2);   // -> 64
static_assert(P::class_index(1000) == 6);   // -> 1024
static_assert(P::class_index(1025) == 7);   // -> 2048
static_assert(P::class_index(4095) == 8);   // -> 4096

// ---- idx <-> size 闭环 ----
constexpr bool roundtrip() {
    for (std::size_t i = 0; i < P::class_count(); ++i)
        if (P::class_index(P::class_size(i)) != i) return false;
    return true;
}
static_assert(roundtrip());

// ---- 严格 2 倍递增 ----
constexpr bool monotonic() {
    for (std::size_t i = 1; i < P::class_count(); ++i)
        if (P::class_size(i) != P::class_size(i - 1) * 2) return false;
    return true;
}
static_assert(monotonic());

// ---- 全量暴力: forall b in [min_size, max_size], class_index(b) 是 >=b 的最小 2 幂档位 ----
constexpr bool minimality() {
    for (std::size_t b = P::min_size; b <= P::max_size; ++b) {
        const auto idx = P::class_index(b);
        if (idx >= P::class_count())                  return false;
        if (P::class_size(idx) < b)                   return false;
        if (idx > 0 && P::class_size(idx - 1) >= b)   return false;
    }
    return true;
}
static_assert(minimality());

// ---- 非幂参数自动对齐 ----
using P2 = PowerOfTwoScheme<24, 3000>;            // -> 16, 4096
static_assert(P2::min_size == 16 && P2::max_size == 4096);
static_assert(P2::class_count() == 9);
static_assert(P2::class_index(3000) == 8);        // -> 4096

// ---- 相邻 2 幂 (最小配置) ----
using P3 = PowerOfTwoScheme<16, 32>;
static_assert(P3::class_count() == 2);
static_assert(P3::class_size(0) == 16 && P3::class_size(1) == 32);
static_assert(P3::class_index(16) == 0);
static_assert(P3::class_index(17) == 1);
static_assert(P3::class_index(32) == 1);

// ---- 大范围 ----
using P4 = PowerOfTwoScheme<8, 1u << 20>;         // 8 .. 1MB, 18 档
static_assert(P4::class_count() == 18);
static_assert(P4::class_size(17) == (1u << 20));
static_assert(P4::class_index(1u << 20) == 17);

}  // namespace tfl::detail::pow2_test

// ============================================================================
//  SubdividedScheme
// ============================================================================

/// @brief Subdivision size-class scheme —— 每个 octave 再切 Subdivisions 等份。
///
/// @tparam MinSize      最小类大小（向下对齐到 bit_floor），默认 16
/// @tparam MaxSize      最大类大小（向上对齐到 bit_ceil），默认 4096
/// @tparam Subdivisions 每 octave 切分数（必须为 2^N），默认 4
///
/// 内碎片 <= 1/Subdivisions，class_index/class_size 全部位运算化，消除整数除法。
template <std::size_t MinSize = 16, std::size_t MaxSize = 4096, std::size_t Subdivisions = 4>
    requires (MinSize >= sizeof(void*))
            && (MinSize < MaxSize)
            && (Subdivisions > 0)
            && (std::has_single_bit(Subdivisions))
struct SubdividedScheme {
private:
    static constexpr std::size_t k_min       = std::bit_floor(MinSize);
    static constexpr std::size_t k_max       = std::bit_ceil(MaxSize);
    static constexpr std::size_t k_min_shift = static_cast<std::size_t>(std::countr_zero(k_min));
    static constexpr std::size_t k_max_shift = static_cast<std::size_t>(std::countr_zero(k_max));
    static constexpr std::size_t k_sub_shift = static_cast<std::size_t>(std::countr_zero(Subdivisions));

    /// @brief k_min << k_base_shift = Subdivisions; 等价于 k_min/Subdivisions 的 shift 形式。
    /// class_size 用它把 mul 吸收进 shl。
    static constexpr std::size_t k_base_shift = k_min_shift - k_sub_shift;

    static constexpr std::size_t k_groups    = k_max_shift - k_min_shift + 1;

    static_assert(k_min_shift >= k_sub_shift, "k_min must be >= Subdivisions");

public:
    /// @brief 实际生效的最小/最大类大小 (与模板参数可能不同)。
    static constexpr std::size_t min_size = k_min;
    static constexpr std::size_t max_size = k_max;

    /// @brief class 总数。
    ///
    /// 前 (k_groups - 1) 个 octave 各占 Subdivisions 个 class;
    /// 最后一个 octave 仅保留 sub 0 (=k_max)。
    [[nodiscard]] static constexpr std::size_t class_count() noexcept {
        return (k_groups - 1) * Subdivisions + 1;
    }

    /// @pre idx < class_count()
    [[nodiscard]] static constexpr std::size_t class_size(std::size_t idx) noexcept {
        const auto group = idx >> k_sub_shift;
        const auto sub   = idx & (Subdivisions - 1);
        return (Subdivisions | sub) << (group + k_base_shift);
    }

    [[nodiscard]] static constexpr std::size_t class_index(std::size_t bytes) noexcept {
        const auto w          = static_cast<std::size_t>(std::bit_width(bytes)) - 1;
        const auto step_shift = w - k_sub_shift;
        const auto step_m1    = (std::size_t{1} << step_shift) - 1;
        const auto group      = w - k_min_shift;
        return (group << k_sub_shift) + (((bytes + step_m1) >> step_shift) - Subdivisions);
    }

};

static_assert(SizeClassScheme<SubdividedScheme<>>, "SubdividedScheme must satisfy SizeClassScheme");

namespace tfl::detail::subdivided_test {

using S = SubdividedScheme<16, 4096, 4>;

// ---- 元信息 ----
static_assert(S::min_size == 16);
static_assert(S::max_size == 4096);
static_assert(S::class_count() == 33);

// ---- class_size 表 (每个 octave 首尾) ----
static_assert(S::class_size(0)  == 16);    static_assert(S::class_size(3)  == 28);
static_assert(S::class_size(4)  == 32);    static_assert(S::class_size(7)  == 56);
static_assert(S::class_size(8)  == 64);    static_assert(S::class_size(11) == 112);
static_assert(S::class_size(12) == 128);   static_assert(S::class_size(15) == 224);
static_assert(S::class_size(16) == 256);   static_assert(S::class_size(23) == 896);
static_assert(S::class_size(24) == 1024);  static_assert(S::class_size(31) == 3584);
static_assert(S::class_size(32) == 4096);

// ---- class_index: 合法档位精确反向 ----
static_assert(S::class_index(16)   == 0);
static_assert(S::class_index(20)   == 1);
static_assert(S::class_index(28)   == 3);
static_assert(S::class_index(32)   == 4);
static_assert(S::class_index(56)   == 7);
static_assert(S::class_index(64)   == 8);
static_assert(S::class_index(128)  == 12);
static_assert(S::class_index(1024) == 24);
static_assert(S::class_index(3584) == 31);
static_assert(S::class_index(4096) == 32);

// ---- class_index: 任意 bytes -> >=bytes 的最小档位 idx ----
static_assert(S::class_index(17)   == 1);    // -> 20
static_assert(S::class_index(19)   == 1);
static_assert(S::class_index(21)   == 2);    // -> 24
static_assert(S::class_index(25)   == 3);    // -> 28
static_assert(S::class_index(29)   == 4);    // octave 0 空隙溢出到 octave 1 起点 (32)
static_assert(S::class_index(31)   == 4);
static_assert(S::class_index(33)   == 5);    // -> 40
static_assert(S::class_index(57)   == 8);    // -> 64
static_assert(S::class_index(63)   == 8);
static_assert(S::class_index(65)   == 9);    // -> 80
static_assert(S::class_index(127)  == 12);   // -> 128
static_assert(S::class_index(1023) == 24);   // -> 1024
static_assert(S::class_index(1025) == 25);   // -> 1280
static_assert(S::class_index(3585) == 32);   // -> 4096
static_assert(S::class_index(4095) == 32);

// ---- idx <-> size 闭环 ----
constexpr bool roundtrip() {
    for (std::size_t i = 0; i < S::class_count(); ++i)
        if (S::class_index(S::class_size(i)) != i) return false;
    return true;
}
static_assert(roundtrip());

// ---- class_size 严格单调 ----
constexpr bool monotonic() {
    for (std::size_t i = 1; i < S::class_count(); ++i)
        if (S::class_size(i) <= S::class_size(i - 1)) return false;
    return true;
}
static_assert(monotonic());

// ---- 全量暴力: forall bytes in [k_min, k_max] ----
//      class_index(b) 给出 >=b 的最小合法档位,且前一档无法容纳
constexpr bool minimality() {
    for (std::size_t b = S::min_size; b <= S::max_size; ++b) {
        const auto idx = S::class_index(b);
        if (idx >= S::class_count())                  return false;
        if (S::class_size(idx) < b)                   return false;
        if (idx > 0 && S::class_size(idx - 1) >= b)   return false;
    }
    return true;
}
static_assert(minimality());

// ---- 非幂参数自动对齐 ----
using S2 = SubdividedScheme<24, 3000, 4>;
static_assert(S2::min_size == 16 && S2::max_size == 4096);
static_assert(S2::class_index(3000) == S2::class_index(3072));

// ---- Subdivisions=1 (纯 2 幂分桶) ----
using S1 = SubdividedScheme<16, 256, 1>;
static_assert(S1::class_count() == 5);
static_assert(S1::class_size(0) == 16 && S1::class_size(4) == 256);
static_assert(S1::class_index(16) == 0);
static_assert(S1::class_index(17) == 1);     // -> 32
static_assert(S1::class_index(32) == 1);
static_assert(S1::class_index(33) == 2);     // -> 64

// ---- Subdivisions=8 ----
using S8 = SubdividedScheme<16, 1024, 8>;
static_assert(S8::class_count() == 49);
static_assert(S8::class_size(1) == 18);
static_assert(S8::class_index(17) == 1);
static_assert(S8::class_index(18) == 1);
static_assert(S8::class_index(19) == 2);

// ---- k_min_shift == k_sub_shift (step=1, 最细粒度) ----
using S_eq = SubdividedScheme<8, 64, 8>;
static_assert(S_eq::class_size(0) == 8);
static_assert(S_eq::class_size(1) == 9);
static_assert(S_eq::class_index(8)  == 0);
static_assert(S_eq::class_index(9)  == 1);
static_assert(S_eq::class_index(10) == 2);
static_assert(S_eq::class_index(16) == 8);

// ---- 全 Scheme 联合闭环 ----
constexpr bool roundtrip_all() {
    auto check = []<typename Sx>() {
        for (std::size_t i = 0; i < Sx::class_count(); ++i)
            if (Sx::class_index(Sx::class_size(i)) != i) return false;
        for (std::size_t b = Sx::min_size; b <= Sx::max_size; ++b) {
            const auto idx = Sx::class_index(b);
            if (idx >= Sx::class_count())                return false;
            if (Sx::class_size(idx) < b)                 return false;
            if (idx > 0 && Sx::class_size(idx - 1) >= b) return false;
        }
        return true;
    };
    return check.template operator()<S>()
           && check.template operator()<S2>()
           && check.template operator()<S1>()
           && check.template operator()<S8>()
           && check.template operator()<S_eq>();
}
static_assert(roundtrip_all());

}  // namespace


// ============================================================================
//  HybridScheme
// ============================================================================
/// @brief 混合 scheme: 小对象线性分级，大对象几何分级（mimalloc 风格）。
///
/// @tparam LinearStep   线性段步长（必须为 2^N），默认 8
/// @tparam LinearMax    线性段上限（必须为 2^N），默认 64
/// @tparam MaxSize      最大类大小（必须为 2^N），默认 4096
/// @tparam Subdivisions 几何段每 octave 切分数（必须为 2^N），默认 4
///
/// 小对象 (<= LinearMax) 内碎片 <= LinearStep，大对象内碎片 <= 1/Subdivisions，分桶不爆炸。
template <std::size_t LinearStep   = 8,
         std::size_t LinearMax    = 64,
         std::size_t MaxSize      = 4096,
         std::size_t Subdivisions = 4>
    requires (LinearStep >= sizeof(void*))
            && (std::has_single_bit(LinearStep))
            && (std::has_single_bit(LinearMax))
            && (std::has_single_bit(MaxSize))
            && (std::has_single_bit(Subdivisions))
            && (LinearStep < LinearMax)
            && (LinearMax  < MaxSize)
struct HybridScheme {
private:
    static constexpr std::size_t k_step_shift   = static_cast<std::size_t>(std::countr_zero(LinearStep));
    static constexpr std::size_t k_linmax_shift = static_cast<std::size_t>(std::countr_zero(LinearMax));
    static constexpr std::size_t k_max_shift    = static_cast<std::size_t>(std::countr_zero(MaxSize));
    static constexpr std::size_t k_sub_shift    = static_cast<std::size_t>(std::countr_zero(Subdivisions));

    static constexpr std::size_t k_linear_count = LinearMax / LinearStep;
    static constexpr std::size_t k_geo_groups   = k_max_shift - k_linmax_shift;

    static_assert(k_linmax_shift >= k_sub_shift, "LinearMax must be >= Subdivisions");

public:
    static constexpr std::size_t min_size = LinearStep;
    static constexpr std::size_t max_size = MaxSize;

    [[nodiscard]] static constexpr std::size_t class_count() noexcept {
        return k_linear_count + k_geo_groups * Subdivisions;
    }

    /// @pre idx < class_count()
    [[nodiscard]] static constexpr std::size_t class_size(std::size_t idx) noexcept {
        if (idx < k_linear_count) {
            return (idx + 1) << k_step_shift;
        }
        const auto rel   = idx - k_linear_count;
        const auto group = rel >> k_sub_shift;
        const auto sub   = rel & (Subdivisions - 1);
        const auto base  = LinearMax << group;
        return base + (base >> k_sub_shift) * (sub + 1);
    }

    /// @pre min_size <= bytes <= max_size
    [[nodiscard]] static constexpr std::size_t class_index(std::size_t bytes) noexcept {
        if (bytes <= LinearMax) {
            // 线性段: ceil(bytes / LinearStep) - 1
            return ((bytes + LinearStep - 1) >> k_step_shift) - 1;
        }
        // 几何段: 复用 SubdividedScheme 算法,起点 LinearMax 而非 MinSize
        const auto w          = static_cast<std::size_t>(std::bit_width(bytes - 1)) - 1;
        const auto step_shift = w - k_sub_shift;
        const auto step_m1    = (std::size_t{1} << step_shift) - 1;
        const auto group      = w - k_linmax_shift;
        const auto local      = (group << k_sub_shift)
                           + (((bytes + step_m1) >> step_shift) - Subdivisions - 1);
        return k_linear_count + local;
    }
};

static_assert(SizeClassScheme<HybridScheme<>>, "HybridScheme must satisfy SizeClassScheme");

// ============================================================================
//  静态断言测试
// ============================================================================
namespace tfl::detail::hybrid_test {

using H = HybridScheme<8, 64, 4096, 4>;

// ---- 元信息 ----
static_assert(H::min_size == 8);
static_assert(H::max_size == 4096);
static_assert(H::class_count() == 32);     // 8 (linear) + 6*4 (geo)

// ---- 线性段 class_size ----
static_assert(H::class_size(0) == 8);
static_assert(H::class_size(1) == 16);
static_assert(H::class_size(2) == 24);
static_assert(H::class_size(3) == 32);
static_assert(H::class_size(4) == 40);
static_assert(H::class_size(5) == 48);
static_assert(H::class_size(6) == 56);
static_assert(H::class_size(7) == 64);      // 线性末档

// ---- 几何段 class_size ----
static_assert(H::class_size(8)  == 80);     // 几何首档 (LinearMax + step)
static_assert(H::class_size(9)  == 96);
static_assert(H::class_size(10) == 112);
static_assert(H::class_size(11) == 128);    // octave 0 末
static_assert(H::class_size(12) == 160);    // octave 1 首
static_assert(H::class_size(15) == 256);    // octave 1 末
static_assert(H::class_size(16) == 320);
static_assert(H::class_size(23) == 1024);
static_assert(H::class_size(27) == 2048);
static_assert(H::class_size(31) == 4096);   // 最末档

// ---- class_index: 线性段 ----
static_assert(H::class_index(1)  == 0);
static_assert(H::class_index(8)  == 0);
static_assert(H::class_index(9)  == 1);     // -> 16
static_assert(H::class_index(16) == 1);
static_assert(H::class_index(17) == 2);     // -> 24
static_assert(H::class_index(63) == 7);     // -> 64
static_assert(H::class_index(64) == 7);     // 线性末

// ---- class_index: 几何段 ----
static_assert(H::class_index(65)   == 8);   // -> 80
static_assert(H::class_index(80)   == 8);
static_assert(H::class_index(81)   == 9);   // -> 96
static_assert(H::class_index(128)  == 11);  // octave 0 末
static_assert(H::class_index(129)  == 12);  // octave 1 首 (-> 160)
static_assert(H::class_index(256)  == 15);  // octave 1 末
static_assert(H::class_index(1024) == 23);
static_assert(H::class_index(4096) == 31);

// ---- idx <-> size 闭环 ----
constexpr bool roundtrip() {
    for (std::size_t i = 0; i < H::class_count(); ++i)
        if (H::class_index(H::class_size(i)) != i) return false;
    return true;
}
static_assert(roundtrip());

// ---- 单调 ----
constexpr bool monotonic() {
    for (std::size_t i = 1; i < H::class_count(); ++i)
        if (H::class_size(i) <= H::class_size(i - 1)) return false;
    return true;
}
static_assert(monotonic());

// ---- 全量暴力: class_index(b) 是 >=b 的最小档位 ----
constexpr bool minimality() {
    for (std::size_t b = H::min_size; b <= H::max_size; ++b) {
        const auto idx = H::class_index(b);
        if (idx >= H::class_count())                  return false;
        if (H::class_size(idx) < b)                   return false;
        if (idx > 0 && H::class_size(idx - 1) >= b)   return false;
    }
    return true;
}
static_assert(minimality());

// ---- 其他配置 ----
using H2 = HybridScheme<8, 128, 1024, 4>;
static_assert(H2::class_count() == 16 + 3*4);  // 28
static_assert(H2::class_size(15) == 128);      // linear 末
static_assert(H2::class_size(16) == 160);      // geo 首

using H3 = HybridScheme<16, 256, 8192, 8>;
static_assert(H3::class_count() == 16 + 5*8);  // 56
static_assert(H3::class_size(0)  == 16);
static_assert(H3::class_size(15) == 256);
static_assert(H3::class_size(16) == 288);      // 256 + 256/8

}  // namespace tfl::detail::hybrid_test

}  // namespace tfl
