/// @file  exception.hpp
/// @brief 框架异常类 Exception / TraceException —— 自动捕获源码位置与调用栈。
///
/// Exception 捕获 source_location，零开销；TraceException 额外捕获 stacktrace，
/// 成本高 1-3 个数量级，仅用于致命错误。两者均支持 std::format 风格消息。
///
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <exception>
#include <string>
#include <source_location>
#include <format>
#include "utility.hpp"

namespace tfl {

/// @brief 框架基础异常 —— 自动捕获 source_location + std::format 风格消息。
///
/// 把 std::source_location 与 std::format 缝合：抛出时零样板代码，编译期校验
/// 格式串参数。Located 包装器用默认参数在调用点自动捕获位置。what() 返回格式化
/// 消息兼容标准 catch 链，where() 返回 source_location 供进一步诊断。
class Exception : public std::exception {
public:
    /// @brief 类似 std::format 风格：接收格式字符串和参数，自动捕获源码位置。
    /// @tparam Args 变长参数类型
    /// @param fmt 包含格式串与源码位置的 Located 包装器
    /// @param args 填充到格式串 {} 中的参数
    template <typename... Args>
    explicit Exception(Located<std::format_string<Args...>> fmt, Args&&... args)
        // Why: Located 包装器在构造期通过 consteval 校验格式串参数匹配，
        // 同时以 std::source_location 嵌入调用点的文件/行号，
        // 运行时仅需拼字符串，无额外开销。
        : m_message{std::vformat(fmt.format().get(), std::make_format_args(args...))}
        , m_location{fmt.location()}
    {}

    /// @brief 直接传错误字符串的构造函数。
    /// @param message 错误描述文本
    /// @param loc 错误发生的源码位置（默认实参自动捕获）
    explicit Exception(std::string_view message,
                       std::source_location loc = std::source_location::current())
        // Why: 消息已完整时绕过 std::format 解析，节省运行时开销。
        // 默认实参在 throw 点由编译器自动注入调用位置。
        : m_message{message}
        , m_location{loc}
    {}

    /// @brief 返回错误描述文本（C 字符串）。
    [[nodiscard]] const char* what() const noexcept override {
        return m_message.c_str();
    }

    /// @brief 返回错误发生的源码位置。
    [[nodiscard]] const std::source_location& where() const noexcept {
        return m_location;
    }

protected:
    std::string             m_message;       ///< 格式化后的错误字符串
    std::source_location    m_location;      ///< 错误发生的文件/行号/函数名
};

/// @brief 重型异常 —— 在 Exception 基础上额外捕获 std::stacktrace 调用栈。
///
/// 用于 source_location 不够的排查场景（如递归路径追踪）。开销比 Exception
/// 高 1-3 个数量级，仅用于致命错误兜底，不应进入热路径。
/// @note 通过 #if TFL_HAS_STACKTRACE 条件编译，不支持的环境自动消失。
#if TFL_HAS_STACKTRACE
class TraceException : public Exception {
public:
    /// @brief 类似 std::format 风格：接收格式字符串和参数，同时捕获源码位置与调用栈。
    /// @tparam Args 变长参数类型
    /// @param fmt 包含格式串、源码位置与调用栈的 Traced 包装器
    /// @param args 填充到格式串 {} 中的参数
    template <typename... Args>
    explicit TraceException(Traced<std::format_string<Args...>> fmt, Args&&... args)
        // Why: Traced 是 Located 的子类，通过 static_cast 切片回 Located 传递给
        // Exception 构造函数，复用其格式化逻辑；同时通过 fmt.stacktrace() 提取
        // 调用栈独立存储。
        : Exception{static_cast<const Located<std::format_string<Args...>>&>(fmt), std::forward<Args>(args)...}
        , m_stacktrace{fmt.stacktrace()}
    {}

    /// @brief 直接传错误字符串并捕获调用栈的构造函数。
    /// @param message 错误描述文本
    /// @param loc     throw 点的源码位置（默认实参自动捕获）
    /// @param trace   捕获时的调用栈快照（默认实参自动抓取）
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
#endif
}  // namespace tfl
