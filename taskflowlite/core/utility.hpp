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
#include <cstring>
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

} // namespace tfl
