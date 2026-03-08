/// @file exception.hpp
/// @brief 提供框架专用异常类，支持源码位置记录与格式化错误消息。
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include <exception>
#include <string>
#include <source_location>
#include <stacktrace>
#include <format>
#include "utility.hpp"

namespace tfl {

/// @brief 能自动记录报错位置（哪个文件、哪一行）的异常基类。
///
/// 继承了标准库的 `std::exception`，所以可以直接被 `catch (const std::exception&)` 接住。
/// 它巧妙利用了 C++20 的 `std::source_location`，你在抛出异常时什么都不用写，
/// 它就能自动记住是哪行代码抛的。另外它还支持像 `std::format` 那样用 `{}` 占位符来拼报错文本。
class Exception : public std::exception {
public:
    /// @brief 像 std::format 一样拼报错字符串的构造函数。
    /// @tparam Args 变长参数类型。
    /// @param fmt 带有格式化文本（含 {}）和源码位置信息的隐藏包装器。
    /// @param args 填进 {} 里的具体变量。
    template <typename... Args>
    explicit Exception(Located<std::format_string<Args...>> fmt, Args&&... args)
        // Why: 靠着 consteval 魔法，编译器会在编译的时候就帮你检查 {} 和参数配不配得上，
        // 同时顺手把当前代码在第几行给记录下来。然后在运行时再把字符串拼好。这样既安全又高效。
        : m_message{std::vformat(fmt.format().get(), std::make_format_args(args...))}
        , m_location{fmt.location()}
    {}

    /// @brief 最简单的直接传字符串的构造函数。
    /// @param message 报错的文字信息。
    /// @param loc 报错的位置，编译器会自动填，你不用管。
    explicit Exception(std::string_view message,
                       std::source_location loc = std::source_location::current())
        // Why: 有时候报错信息很简单，不需要拼字符串。提供这个构造函数能省去解析 format 的开销。
        // 靠着默认参数，编译器在 throw 的那一瞬间就会自动把文件名和行号塞进去。
        : m_message{message}
        , m_location{loc}
    {}

    /// @brief 拿到具体的报错文字。
    [[nodiscard]] const char* what() const noexcept override {
        return m_message.c_str();
    }

    /// @brief 拿到报错到底发生在哪一行代码。
    [[nodiscard]] const std::source_location& where() const noexcept {
        return m_location;
    }

protected:
    std::string m_message;            ///< 拼好的报错字符串
    std::source_location m_location;  ///< 记下了哪个文件、哪一行、哪个函数抛的错
};

/// @brief 除了报错位置，还能顺藤摸瓜把整个函数调用栈（堆栈）都揪出来的异常类。
///
/// 除了 `Exception` 原本的功能，它里面还多存了一个 `std::stacktrace`。
/// @note 注意：抓取调用栈是个比较费 CPU 的操作。一般只有在遇到那种死活查不出原因的致命错误时才用它。
/// 平时普通的业务报错，用上面那个轻量级的 `Exception` 就够了。
class TraceException : public Exception {
public:
    /// @brief 像 std::format 一样拼字符串，并且抓取调用栈的构造函数。
    /// @tparam Args 变长参数类型。
    /// @param fmt 藏着源码位置、调用栈信息和格式化文本的隐藏包装器。
    /// @param args 填进 {} 里的变量。
    template <typename... Args>
    explicit TraceException(Traced<std::format_string<Args...>> fmt, Args&&... args)
        // Why: Traced 是 Located 的子类，这里用 static_cast 把它当成父类传给 Exception，
        // 这样就能直接白嫖父类拼字符串的逻辑。同时把藏在里面的调用栈数据抠出来存到自己这儿。
        : Exception{static_cast<const Located<std::format_string<Args...>>&>(fmt), std::forward<Args>(args)...}
        , m_stacktrace{fmt.stacktrace()}
    {}

    /// @brief 最简单的直接传字符串，并且抓取调用栈的构造函数。
    /// @param message 报错的文字信息。
    /// @param loc 报错的位置。
    /// @param trace 报错那一瞬间的函数调用链，编译器默认会帮你抓。
    explicit TraceException(std::string_view message,
                            std::source_location loc = std::source_location::current(),
                            std::stacktrace trace = std::stacktrace::current())
        // Why: 函数调用栈里面的数据可能很多（好几十层函数），直接用 std::move 把所有权抢过来，避免产生昂贵的深拷贝。
        : Exception{message, loc}
        , m_stacktrace{std::move(trace)}
    {}

    /// @brief 拿到报错那一瞬间的完整调用链（谁调用了谁）。
    [[nodiscard]] const std::stacktrace& trace() const noexcept {
        return m_stacktrace;
    }

private:
    std::stacktrace m_stacktrace; ///< 存着完整的调用栈数据
};

}  // namespace tfl
