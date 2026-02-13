#pragma once

#include <atomic>
#include <source_location>
#include <stacktrace>
#include <limits>
#include <type_traits>
#include <string>
#include <string_view>
#include <typeinfo>
#include <vector>
#include <utility>
#include "macros.hpp"

namespace tfl {
/**
 * @brief 将两个枚举值打包成一个 64 位无符号整数键。
 *
 * 该函数适用于枚举底层类型总大小不超过 64 位的情况。
 * 支持 signed 和 unsigned 枚举，包括负值处理（通过掩码防止符号扩展污染）。
 * 返回值可逆，便于后续解包。
 *
 * @tparam E1 第一个枚举类型，必须是 enum 或 enum class。
 * @tparam E2 第二个枚举类型，必须是 enum 或 enum class。
 * @param e1 第一个枚举值。
 * @param e2 第二个枚举值。
 * @return std::uint64_t 打包后的键值（E1 值移到高位，E2 值放低位）。
 */
template<typename E1, typename E2>
    requires (std::is_enum_v<E1> && std::is_enum_v<E2> &&
             (sizeof(std::underlying_type_t<E1>) + sizeof(std::underlying_type_t<E2>) <= sizeof(std::uint64_t)))
inline constexpr auto make_key(E1 e1, E2 e2) noexcept ->std::uint64_t {

    // 获取 E1 和 E2 的底层整数类型（如 int8_t 或 unsigned int）
    using U1 = std::underlying_type_t<E1>;
    using U2 = std::underlying_type_t<E2>;

    // 计算底层类型的位宽（字节数 * 8）
    constexpr unsigned bits1 = sizeof(U1) * 8;
    constexpr unsigned bits2 = sizeof(U2) * 8;

    // 将枚举值转换为底层类型，再转换为 uint64_t（处理 signed 到 unsigned 的模 2^64 转换）
    const auto v1 = static_cast<std::uint64_t>(static_cast<U1>(e1));
    const auto v2 = static_cast<std::uint64_t>(static_cast<U2>(e2));

    // 生成掩码：低 bits1/bits2 位全 1，用于清除符号扩展的高位 1（负值情况）
    constexpr std::uint64_t mask1 = (1ULL << bits1) - 1ULL;
    constexpr std::uint64_t mask2 = (1ULL << bits2) - 1ULL;

    // 打包：v1 掩码后左移 bits2 位（置于高位），OR v2 掩码后（置于低位）
    return ((v1 & mask1) << bits2) | (v2 & mask2);
}




// -------------------------------- //

/**
 * @brief An empty base class that is not copyable or movable.
 *
 * The template parameter prevents multiple empty bases when inheriting multiple classes.
 * 一个不能复制或移动的空基类。
 * 当继承多个类时，template参数防止出现多个空基。
*/
template <typename CRTP>
struct Immovable {
    static_assert(!requires { sizeof(CRTP); }, "sizeof(CRTP) must not be a complete type");
    constexpr Immovable() = default;
    constexpr ~Immovable() = default;

    constexpr Immovable(const Immovable&) = delete;
    constexpr Immovable& operator=(const Immovable&) = delete;

    constexpr Immovable(Immovable &&) noexcept = delete;
    constexpr Immovable& operator=(Immovable &&) noexcept = delete;
};

static_assert(std::is_empty_v<Immovable<void>>);

template <typename CRTP>
struct MoveOnly {
    static_assert(!requires { sizeof(CRTP); }, "sizeof(CRTP) must not be a complete type");
    constexpr MoveOnly() = default;
    constexpr ~MoveOnly() noexcept = default;

    // 禁用拷贝
    constexpr MoveOnly(const MoveOnly&) = delete;
    constexpr MoveOnly& operator=(const MoveOnly&) = delete;

    // 启用移动（默认实现转移成员）
    constexpr MoveOnly(MoveOnly&&) noexcept = default;
    constexpr MoveOnly& operator=(MoveOnly&&) noexcept = default;
};


static_assert(std::is_empty_v<MoveOnly<void>>);

/**
 * @brief Safe integral cast, will terminate if the cast would overflow in debug.
 * 安全整型强制转换，如果强制转换在调试中溢出，将终止。
 */
template <std::integral To, std::integral From>
constexpr auto checked_cast(From val) noexcept -> To {

    constexpr auto to_min = std::numeric_limits<To>::min();
    constexpr auto to_max = std::numeric_limits<To>::max();

    constexpr auto from_min = std::numeric_limits<From>::min();
    constexpr auto from_max = std::numeric_limits<From>::max();

    /**
   *    [   from    ]
   *     [   to   ]
   */

    if constexpr (std::cmp_greater(to_min, from_min)) {
        LF_ASSERT(val >= static_cast<From>(to_min) && "Underflow");
    }

    if constexpr (std::cmp_less(to_max, from_max)) {
        LF_ASSERT(val <= static_cast<From>(to_max) && "Overflow");
    }

    return static_cast<To>(val);
}

/**
 * @brief Transform `[a, b, c] -> [f(a), f(b), f(c)]`.
 */
template <typename T, typename F>
constexpr auto map(std::vector<T> const &from, F &&func) -> std::vector<std::invoke_result_t<F &, T const &>> {

    std::vector<std::invoke_result_t<F &, T const &>> out;

    out.reserve(from.size());

    for (auto &&item : from) {
        out.emplace_back(std::invoke(func, item));
    }

    return out;
}

/**
 * @brief Transform `[a, b, c] -> [f(a), f(b), f(c)]`.
 */
template <typename T, typename F>
constexpr auto map(std::vector<T> &&from, F &&func) -> std::vector<std::invoke_result_t<F &, T>> {

    std::vector<std::invoke_result_t<F &, T>> out;

    out.reserve(from.size());

    for (auto &&item : from) {
        out.emplace_back(std::invoke(func, std::move(item)));
    }

    return out;
}
/**
 * @brief Returns ``ptr`` and asserts it is non-null in debug builds.
 */
template <typename T>
    requires requires (T &&ptr) {
        { ptr == nullptr } -> std::convertible_to<bool>;
    }
constexpr auto non_null(T &&val, [[maybe_unused]] std::source_location loc
                                 = std::source_location::current()) noexcept -> T && {
#ifndef NDEBUG
    if (val == nullptr) {
        // NOLINTNEXTLINE
        std::fprintf(stderr,
                     "%s:%u: Null check failed: %s\n",
                     loc.file_name(),
                     checked_cast<unsigned>(loc.line()),
                     loc.function_name());
        std::terminate();
    }
#endif
    return std::forward<T>(val);
}




// 获取解码后的类型名称（跨平台支持）
template <typename T>
constexpr auto readable_type_name() noexcept -> const char* {
    return typeid(T).name();
}

// 主接口，仅移除 tfl:: 前缀，保留其余命名空间信息
template <typename T>
constexpr auto type_name() noexcept -> std::string_view{
    static const char* demangled = readable_type_name<T>();

    std::string_view name = demangled;

    constexpr std::string_view class_prefix = "class ";
    constexpr std::string_view struct_prefix = "struct ";

    if (name.starts_with(class_prefix)) {
        name.remove_prefix(class_prefix.size());
    } else if (name.starts_with(struct_prefix)) {
        name.remove_prefix(struct_prefix.size());
    }

    // 移除 tfl:: 命名空间前缀（保留嵌套）
    constexpr std::string_view tg_prefix = "tfl::";
    if (name.starts_with(tg_prefix)) {
        name.remove_prefix(tg_prefix.size());
    }

    return name;
}

//用于模板实参 ，非类型参数 字符串
// 使用字符串字面量作为模板参数的辅助类
template<std::size_t N>
struct StringLiteral
{
    //不能传递字面量，不能传递指针，因为指针也会被推导为字面量。
    constexpr StringLiteral(const char(&str)[N])
    {
        //通过拷贝来赋值
        std::copy_n(str, N, value);
    }
    char value[N]{};
};


template <class T>
struct Located {
private:
    T m_inner;
    std::source_location m_loc;

public:
    template <class U, class Loc = std::source_location>
        requires std::constructible_from<T, U> &&
                     std::constructible_from<std::source_location, Loc>
    consteval Located(U&& inner, Loc&& loc = std::source_location::current()) noexcept
        : m_inner{std::forward<U>(inner)}
        , m_loc{std::forward<Loc>(loc)}
    {}

    constexpr const T& format() const noexcept { return m_inner; }
    constexpr const std::source_location& location() const noexcept { return m_loc; }
};

template <class T>
struct Traced : Located<T> {
private:
    std::stacktrace m_trace;

public:
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

    constexpr const std::stacktrace& stacktrace() const noexcept { return m_trace; }
};
}
