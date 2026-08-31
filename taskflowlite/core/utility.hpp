/// @file utility.hpp
/// @brief 框架使用的基础类型、策略基类和源码位置包装工具。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <atomic>
#include <source_location>
#include <limits>
#include <type_traits>
#include <string>
#include <string_view>
#include <typeinfo>
#include <vector>
#include <utility>
#include <optional>
#include <bit>
#include <cstddef>
#include <type_traits>

#include <cstddef>
#include <type_traits>
#include <version>


#if defined(__cpp_lib_stacktrace) && __cpp_lib_stacktrace >= 202011L
#  include <stacktrace>
#  define TFL_HAS_STACKTRACE 1
#else
#  define TFL_HAS_STACKTRACE 0
#endif

#include "macros.hpp"

namespace tfl {

/// @brief 编译目标使用的缓存行大小估计值。
#if defined(__cpp_lib_hardware_interference_size) && !defined(__GNUC__)
    // 优先使用标准库提供的硬件干扰大小。
inline constexpr std::size_t cache_line_size = std::hardware_destructive_interference_size;
#else
    // 标准库常量不可用时按目标架构选择。
#if defined(__APPLE__) && defined(__aarch64__)
    // Apple Silicon 使用 128 字节。
inline constexpr std::size_t cache_line_size = 128;
#elif defined(__powerpc64__)
    // PowerPC64 使用 128 字节。
inline constexpr std::size_t cache_line_size = 128;
#elif defined(__s390x__)
    // IBM z/Architecture 使用 256 字节。
inline constexpr std::size_t cache_line_size = 256;
#elif defined(__arm__)
    // 32 位 ARM 按架构版本选择。
#if defined(__ARM_ARCH_5T__)
inline constexpr std::size_t cache_line_size = 32;
#else
inline constexpr std::size_t cache_line_size = 64;
#endif
#elif defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    // x86 与 x64 使用 64 字节。
inline constexpr std::size_t cache_line_size = 64;
#else
    // 未识别的架构使用 64 字节。
inline constexpr std::size_t cache_line_size = 64;
#endif
#endif


/// @brief 每字节的二进制位数，等价于 C 的 CHAR_BIT。
inline constexpr unsigned char_bits = std::numeric_limits<unsigned char>::digits;


/// @brief 为需要稳定对象地址的 CRTP 派生类型统一禁用复制和移动。
///
/// 该空基类不持有资源，只通过删除四个特殊成员限制派生类型的值语义。
///
/// @tparam CRTP 尚未完成定义的派生类型。
template <typename CRTP>
struct Immovable {
    // 拒绝将完整类型作为 CRTP 参数。
    static_assert(!requires { sizeof(CRTP); }, "sizeof(CRTP) must not be a complete type");
    /// @brief 允许派生类默认构造不可移动基类。
    constexpr Immovable() = default;

    /// @brief 基类析构不持有资源。
    constexpr ~Immovable() = default;

    /// @brief 禁止复制构造。
    constexpr Immovable(const Immovable&) = delete;

    /// @brief 禁止复制赋值。
    constexpr Immovable& operator=(const Immovable&) = delete;

    /// @brief 禁止移动构造，以保持派生对象地址稳定。
    constexpr Immovable(Immovable &&) noexcept = delete;

    /// @brief 禁止移动赋值。
    constexpr Immovable& operator=(Immovable &&) noexcept = delete;
};

static_assert(std::is_empty_v<Immovable<void>>);

/// @brief 为独占资源的 CRTP 派生类型禁用复制并保留默认移动语义。
///
/// 该空基类不持有资源；实际移动是否可用及其效果仍由派生类型的成员决定。
///
/// @tparam CRTP 尚未完成定义的派生类型。
template <typename CRTP>
struct MoveOnly {
    static_assert(!requires { sizeof(CRTP); }, "sizeof(CRTP) must not be a complete type");
    /// @brief 允许派生类默认构造移动专用基类。
    constexpr MoveOnly() = default;

    /// @brief 基类析构不持有资源。
    constexpr ~MoveOnly() noexcept = default;

    /// @brief 禁止复制构造。
    constexpr MoveOnly(const MoveOnly&) = delete;

    /// @brief 禁止复制赋值。
    constexpr MoveOnly& operator=(const MoveOnly&) = delete;

    /// @brief 允许派生类使用默认移动构造。
    constexpr MoveOnly(MoveOnly&&) noexcept = default;

    /// @brief 允许派生类使用默认移动赋值。
    constexpr MoveOnly& operator=(MoveOnly&&) noexcept = default;
};

static_assert(std::is_empty_v<MoveOnly<void>>);

/// @brief 按值保存一个输入对象，并在常量求值构造时记录调用点源码位置。
///
/// 包装器拥有 `T` 和 `source_location` 快照，主要用于让异常构造同时接收格式串
/// 与其来源位置；访问器返回的引用随包装器销毁而失效。
///
/// @tparam T 被包装并按值拥有的类型。
template <class T>
struct Located {
private:
    T m_inner;                    ///< 实际存储的值。
    std::source_location m_loc;   ///< 对象构造时的源码位置。

public:
    /// @brief 构造包装值并捕获调用点源码位置。
    /// @tparam U 用于构造 T 的输入类型。
    /// @tparam Loc 可构造 `std::source_location` 的位置类型。
    /// @param inner 要保存的值。
    /// @param loc 源码位置，默认在调用点由 `current()` 生成。
    template <class U, class Loc = std::source_location>
        requires std::constructible_from<T, U> &&
                     std::constructible_from<std::source_location, Loc>
    consteval Located(U&& inner, Loc&& loc = std::source_location::current()) noexcept
        : m_inner{std::forward<U>(inner)}
        , m_loc{std::forward<Loc>(loc)}
    {}

    /// @brief 获取被包装的基础值，即构造时传入的格式串或消息 payload。
    /// @return 对内部存储 T 的常量引用。
    constexpr const T& format() const noexcept { return m_inner; }

    /// @brief 获取该包装器构造时自动捕获的源码位置（文件、行号、函数名）。
    /// @return std::source_location 结构体的常量引用。
    constexpr const std::source_location& location() const noexcept { return m_loc; }
};

#if TFL_HAS_STACKTRACE
/// @brief 在 `Located<T>` 的值和源码位置之外按值保存调用栈快照。
///
/// 该类型仅在平台支持 `std::stacktrace` 时定义，捕获到的帧受平台和优化设置影响。
///
/// @tparam T 被包装并按值拥有的类型。
template <class T>
struct Traced : Located<T> {
private:
    std::stacktrace m_trace;   ///< 构造时的调用栈信息。

public:
    /// @brief 构造包装值并捕获调用点源码位置与堆栈信息。
    /// @tparam U 用于构造 T 的输入类型。
    /// @tparam Loc 源码位置类型。
    /// @tparam Trace 堆栈快照类型。
    /// @param inner 要保存的值。
    /// @param loc 调用点源码位置。
    /// @param trace 调用栈快照。
    template <class U, class Loc = std::source_location, class Trace = std::stacktrace>
        requires std::constructible_from<T, U> &&
                     std::constructible_from<std::source_location, Loc> &&
                     std::constructible_from<std::stacktrace, Trace>
    consteval Traced(
        U&& inner,
        Loc&& loc = std::source_location::current(),
        Trace&& trace = std::stacktrace::current()
        ) noexcept
        : Located<T>{std::forward<U>(inner), std::forward<Loc>(loc)}
        , m_trace{std::forward<Trace>(trace)}
    {}

    /// @brief 获取该包装器构造时捕获的调用栈快照。
    /// @return 实现捕获到的 `std::stacktrace` 快照；优化和平台限制可能省略帧。
    constexpr const std::stacktrace& stacktrace() const noexcept { return m_trace; }
};
#endif



/// @brief 将整数向上舍入到指定 2 的幂对齐边界。
/// @param n 原始大小或偏移。
/// @param alignment 对齐值，必须是非零的 2 的幂。
/// @return 不小于 n 的最小 alignment 倍数。
///
/// @pre alignment != 0。
/// @pre std::has_single_bit(alignment)。
/// @pre n <= SIZE_MAX - (alignment - 1)。
[[nodiscard]] inline constexpr std::size_t align_up(std::size_t n, std::size_t alignment) noexcept {
    return (n + alignment - 1) & ~(alignment - 1);
}

/// @brief 默认前缀及其后 payload 的基础对齐。
inline constexpr std::size_t k_default_prefix_alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
static_assert(std::has_single_bit(k_default_prefix_alignment));
/// @brief 描述“固定前缀值 + 对齐 payload”的原始内存布局和地址转换。
///
/// 该无状态工具通过 `memcpy` 读写前缀，不分配、不构造也不释放 payload；调用方
/// 必须提供满足大小、对齐和来源配对要求的原始存储。
///
/// @tparam T 写入前缀的平凡可复制值类型。
/// @tparam Alignment payload 的非零二次幂对齐值。
template <typename T, std::size_t Alignment = k_default_prefix_alignment>
struct ValuePrefix {
    using value_type = T;

    static_assert(std::is_trivially_copyable_v<value_type>);
    static_assert(Alignment != 0);
    static_assert(std::has_single_bit(Alignment));

    // raw 必须至少能够安全存放 value_type。
    static_assert(Alignment >= alignof(value_type));
    static_assert(Alignment % alignof(value_type) == 0);

    // 防止下面 align_up() 在常量求值时溢出。
    static_assert(sizeof(value_type) <= std::numeric_limits<std::size_t>::max() - (Alignment - 1));

    /// @brief 前缀对齐。
    static constexpr std::size_t alignment = Alignment;

    /// @brief 包含 padding 的前缀大小。
    static constexpr std::size_t size = align_up(sizeof(value_type), alignment);

    /// @brief 从原始分配地址越过前缀得到可修改 payload 地址。
    /// @param raw 指向至少 size 字节前缀及后续 payload 的地址。
    /// @return `raw + size`。
    /// @pre raw 不得为 nullptr。
    [[nodiscard]] static void* payload_from_raw(void* raw) noexcept {
        return static_cast<std::byte*>(raw) + size;
    }

    /// @brief 从只读原始分配地址得到只读 payload 地址。
    /// @param raw 原始分配地址。
    /// @return `raw + size`。
    /// @pre raw 不得为 nullptr。
    [[nodiscard]] static const void* payload_from_raw(const void* raw) noexcept {
        return static_cast<const std::byte*>(raw) + size;
    }

    /// @brief 从可修改 payload 地址恢复原始分配地址。
    /// @param payload 先前由 `payload_from_raw()` 得到的地址。
    /// @return `payload - size`。
    /// @pre payload 不得为 nullptr。
    [[nodiscard]] static void* raw_from_payload(void* payload) noexcept {
        return static_cast<std::byte*>(payload) - size;
    }

    /// @brief 从只读 payload 地址恢复只读原始分配地址。
    /// @param payload 先前由 `payload_from_raw()` 得到的地址。
    /// @return `payload - size`。
    /// @pre payload 不得为 nullptr。
    [[nodiscard]] static const void* raw_from_payload(const void* payload) noexcept {
        return static_cast<const std::byte*>(payload) - size;
    }

    /// @brief 通过 memcpy 把平凡可复制值写入前缀。
    /// @param raw 可写原始分配地址。
    /// @param value 要保存的前缀值。
    /// @pre raw 至少提供 `sizeof(value_type)` 个可写字节。
    static void store(void* raw, const value_type& value) noexcept {
        std::memcpy(raw, &value, sizeof(value_type));
    }

    /// @brief 通过 memcpy 从原始地址读取前缀值。
    /// @param raw 先前写入前缀值的原始地址。
    /// @return 按值复制出的前缀内容。
    [[nodiscard]] static value_type load_from_raw(const void* raw) noexcept {
        value_type value;
        std::memcpy(&value, raw, sizeof(value_type));
        return value;
    }

    /// @brief 从 payload 地址回退到 raw 并读取前缀值。
    /// @param payload 对应原始分配的 payload 地址。
    /// @return 按值复制出的前缀内容。
    [[nodiscard]] static value_type load_from_payload(const void* payload) noexcept {
        return load_from_raw(raw_from_payload(payload));
    }
};

/// @brief 保存一个 owner 指针的前缀。
template <typename T>
using PointerPrefix = ValuePrefix<T*>;

/// @brief 保存真实内存容量的前缀。
using SizePrefix = ValuePrefix<std::size_t>;

using Int32Prefix = ValuePrefix<std::int32_t>;

using Int64Prefix = ValuePrefix<std::int64_t>;

} // namespace tfl
