/// @file utility.hpp
/// @brief taskflowlite 框架通用工具集，提供类型安全转换、CRTP 基类与反射包装器等基础支持。
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

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

/// @brief 将两个枚举值无损压缩打包成一个 64 位无符号整数键。
///
/// 常用于状态机的 (当前状态, 事件) 查找表，或类型分派映射。
/// E1 占据高位，E2 占据低位，通过位操作确保有符号枚举处理时不发生意外的符号扩展。
///
/// @tparam E1 第一个枚举类型。
/// @tparam E2 第二个枚举类型。
/// @param e1 第一个枚举值。
/// @param e2 第二个枚举值。
/// @return 打包后的 64 位键值。
/// @pre `E1` 和 `E2` 的底层类型总字节数不能超过 8 字节（64 位）。
/// @post 返回值完全可逆，可通过位移还原双枚举。
template<typename E1, typename E2>
    requires (std::is_enum_v<E1> && std::is_enum_v<E2> &&
             (sizeof(std::underlying_type_t<E1>) + sizeof(std::underlying_type_t<E2>) <= sizeof(std::uint64_t)))
inline constexpr auto make_key(E1 e1, E2 e2) noexcept ->std::uint64_t {

    using U1 = std::underlying_type_t<E1>;
    using U2 = std::underlying_type_t<E2>;

    constexpr unsigned bits1 = sizeof(U1) * 8;
    constexpr unsigned bits2 = sizeof(U2) * 8;

    // 利用 C++ 规范处理负值转换的模 2^64 特性
    const auto v1 = static_cast<std::uint64_t>(static_cast<U1>(e1));
    const auto v2 = static_cast<std::uint64_t>(static_cast<U2>(e2));

    // 生成掩码以清除由于负值强转可能带来的高位符号扩展
    constexpr std::uint64_t mask1 = (1ULL << bits1) - 1ULL;
    constexpr std::uint64_t mask2 = (1ULL << bits2) - 1ULL;

    return ((v1 & mask1) << bits2) | (v2 & mask2);
}

/// @brief 不可拷贝、不可移动的 CRTP 空基类。
///
/// 采用 CRTP 避免多重继承下的空基类歧义问题，适用于需要固定内存地址的底层组件
/// （如 Executor、Worker、Topology）。
///
/// @tparam CRTP 派生类类型。
/// @note EBO (空基类优化) 会确保该基类不增加派生类的内存开销。
template <typename CRTP>
struct Immovable {
    // CRTP 模式在基类实例化时派生类必定为不完整类型，此断言防止非 CRTP 误用
    static_assert(!requires { sizeof(CRTP); }, "sizeof(CRTP) must not be a complete type");
    constexpr Immovable() = default;
    constexpr ~Immovable() = default;

    constexpr Immovable(const Immovable&) = delete;
    constexpr Immovable& operator=(const Immovable&) = delete;

    constexpr Immovable(Immovable &&) noexcept = delete;
    constexpr Immovable& operator=(Immovable &&) noexcept = delete;
};

static_assert(std::is_empty_v<Immovable<void>>);

/// @brief 仅可移动的 CRTP 空基类。
///
/// 允许移动语义但禁止拷贝，适用于需要转移所有权的对象（如 Flow 或 AsyncTask）。
///
/// @tparam CRTP 派生类类型。
template <typename CRTP>
struct MoveOnly {
    static_assert(!requires { sizeof(CRTP); }, "sizeof(CRTP) must not be a complete type");
    constexpr MoveOnly() = default;
    constexpr ~MoveOnly() noexcept = default;

    constexpr MoveOnly(const MoveOnly&) = delete;
    constexpr MoveOnly& operator=(const MoveOnly&) = delete;

    constexpr MoveOnly(MoveOnly&&) noexcept = default;
    constexpr MoveOnly& operator=(MoveOnly&&) noexcept = default;
};

static_assert(std::is_empty_v<MoveOnly<void>>);

/// @brief 安全整型强制转换。
///
/// 在调试模式下检测整型转换是否会导致溢出或下溢，利用 C++20 `std::cmp_greater`/`std::cmp_less`
/// 规避隐式转换陷阱。在 Release 模式下等价于零开销的 `static_cast`。
///
/// @tparam To 目标整型。
/// @tparam From 源整型。
/// @param val 待转换的值。
/// @return 转换后的目标类型值。
/// @pre 目标类型必须能够容纳源值，否则在 NDEBUG 未定义时触发断言。
template <std::integral To, std::integral From>
constexpr auto checked_cast(From val) noexcept -> To {

    constexpr auto to_min = std::numeric_limits<To>::min();
    constexpr auto to_max = std::numeric_limits<To>::max();

    constexpr auto from_min = std::numeric_limits<From>::min();
    constexpr auto from_max = std::numeric_limits<From>::max();

    // 编译期优化：仅当 To 无法完全覆盖 From 的下限时才生成下溢检查
    if constexpr (std::cmp_greater(to_min, from_min)) {
        TFL_ASSERT(val >= static_cast<From>(to_min) && "Underflow");
    }

    // 编译期优化：仅当 To 无法完全覆盖 From 的上限时才生成上溢检查
    if constexpr (std::cmp_less(to_max, from_max)) {
        TFL_ASSERT(val <= static_cast<From>(to_max) && "Overflow");
    }

    return static_cast<To>(val);
}

/// @brief 对 std::vector 执行函数式映射（常量引用重载）。
///
/// @tparam T 输入 vector 的元素类型。
/// @tparam F 映射函数的类型。
/// @param from 源 vector，作为常量传入，不被修改。
/// @param func 可调用对象，接收 `T const&` 并返回映射结果。
/// @return 返回由映射结果组装的新 std::vector。
/// @note 内部预先 reserve 内存以避免重复分配。
template <typename T, typename F>
constexpr auto map(std::vector<T> const &from, F &&func) -> std::vector<std::invoke_result_t<F &, T const &>> {

    std::vector<std::invoke_result_t<F &, T const &>> out;

    out.reserve(from.size());

    for (auto &&item : from) {
        out.emplace_back(std::invoke(func, item));
    }

    return out;
}

/// @brief 对 std::vector 执行函数式映射（右值引用重载）。
///
/// 适用于源数据不再需要或包含 move-only 类型的场景。
///
/// @tparam T 输入 vector 的元素类型。
/// @tparam F 映射函数的类型。
/// @param from 源 vector，其元素将被移动。
/// @param func 可调用对象，接收 `T&&` 并返回映射结果。
/// @return 返回由映射结果组装的新 std::vector。
template <typename T, typename F>
constexpr auto map(std::vector<T> &&from, F &&func) -> std::vector<std::invoke_result_t<F &, T>> {

    std::vector<std::invoke_result_t<F &, T>> out;

    out.reserve(from.size());

    for (auto &&item : from) {
        out.emplace_back(std::invoke(func, std::move(item)));
    }

    return out;
}

/// @brief 断言指针或智能指针非空。
///
/// 自动捕获调用处的文件名、行号和函数名。在非 NDEBUG 模式下若传入 null 则终止程序；
/// 在 Release 模式下等价于完全零开销的完美转发。
///
/// @tparam T 支持与 `nullptr` 比较的类型（如裸指针、智能指针）。
/// @param val 待检查的指针对象。
/// @param loc 自动捕获的源码位置，无需手动传入。
/// @return 完美转发原对象，支持链式调用。
/// @pre `val` != nullptr。
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

/// @brief 获取底层的 mangled 类型名称。
template <typename T>
constexpr auto readable_type_name() noexcept -> const char* {
    return typeid(T).name();
}

/// @brief 跨平台获取清晰可读的类型名称。
///
/// 自动剔除 MSVC 的前缀以及框架内部的命名空间前缀，保留嵌套命名空间。
///
/// @tparam T 需要查询的类型。
/// @return 清理后的类型字符串视图。
/// @note 结果被静态缓存以避免重复的字符串处理开销。
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

    constexpr std::string_view tg_prefix = "tfl::";
    if (name.starts_with(tg_prefix)) {
        name.remove_prefix(tg_prefix.size());
    }

    return name;
}

/// @brief 允许将字符串字面量作为非类型模板参数 (NTTP) 传递的包装类。
///
/// @tparam N 字符串数组长度（由编译器自动推导，包含末尾的 '\0'）。
template<std::size_t N>
struct StringLiteral
{
    /// @brief 从字符数组引用构造包装，避免退化为指针。
    constexpr StringLiteral(const char(&str)[N])
    {
        std::copy_n(str, N, value);
    }
    char value[N]{}; ///< 内部存储的字符数组
};

/// @brief 自动绑定源码位置的值包装器。
///
/// 用于在编译期捕获对象构造（即调用点）的源文件和行号信息，常用于异常处理或日志。
///
/// @tparam T 被包装的基础类型。
template <class T>
struct Located {
private:
    T m_inner;                    ///< 实际存储的值
    std::source_location m_loc;   ///< 对象构造时的源码位置

public:
    /// @brief 构造并自动捕获调用点的源码位置。
    template <class U, class Loc = std::source_location>
        requires std::constructible_from<T, U> &&
                     std::constructible_from<std::source_location, Loc>
    consteval Located(U&& inner, Loc&& loc = std::source_location::current()) noexcept
        : m_inner{std::forward<U>(inner)}
        , m_loc{std::forward<Loc>(loc)}
    {}

    /// @brief 获取被包装的值。
    constexpr const T& format() const noexcept { return m_inner; }

    /// @brief 获取捕获的源码位置。
    constexpr const std::source_location& location() const noexcept { return m_loc; }
};

/// @brief 带有堆栈跟踪与源码位置的值包装器。
///
/// 继承自 `Located` 并额外附加 `std::stacktrace`。适用于需要完整调用链诊断信息的场景。
///
/// @tparam T 被包装的基础类型。
/// @note 捕获堆栈涉及运行时栈回溯开销，仅应在异常或诊断错误路径使用。
template <class T>
struct Traced : Located<T> {
private:
    std::stacktrace m_trace;   ///< 构造时的调用栈信息

public:
    /// @brief 构造并自动捕获调用点源码位置与堆栈信息。
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

    /// @brief 获取捕获的堆栈跟踪。
    constexpr const std::stacktrace& stacktrace() const noexcept { return m_trace; }
};

} // namespace tfl
