/// @file exception.hpp
/// @brief 框架异常类 Exception / TraceException —— 自动捕获源码位置与调用栈。
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

/// @brief 表示携带错误消息和发生位置的 TaskflowLite 基础异常。
///
/// 异常对象按值拥有格式化后的消息和 `source_location` 快照；格式化构造在编译期
/// 校验参数，`what()` 与 `where()` 返回的视图只在异常对象存活期间有效。
class Exception : public std::exception {
public:
    /// @brief 类似 std::format 风格：接收格式字符串和参数，自动捕获源码位置。
    /// @tparam Args 变长参数类型。
    /// @param fmt 包含格式串与源码位置的 Located 包装器。
    /// @param args 填充到格式串 {} 中的参数。
    template <typename... Args>
    explicit Exception(Located<std::format_string<Args...>> fmt, Args&&... args)
        // Located 在构造期校验格式参数并保存调用位置。
        // 同时以 std::source_location 嵌入调用点的文件/行号，
        // 运行时仍会格式化并分配消息字符串。
        : m_message{std::vformat(fmt.format().get(), std::make_format_args(args...))}
        , m_location{fmt.location()}
    {}

    /// @brief 直接传错误字符串的构造函数。
    /// @param message 错误描述文本。
    /// @param loc 错误发生的源码位置（默认实参自动捕获）。
    explicit Exception(std::string_view message,
                       std::source_location loc = std::source_location::current())
        // 完整消息直接保存，不再执行格式解析。
        // 默认实参在 throw 点由编译器自动注入调用位置。
        : m_message{message}
        , m_location{loc}
    {}

    /// @brief 返回错误描述文本。
    /// @return 以空字符结尾的内部消息指针；有效期到异常对象销毁或消息被修改。
    [[nodiscard]] const char* what() const noexcept override {
        return m_message.c_str();
    }

    /// @brief 返回错误发生的源码位置。
    /// @return 构造异常时保存的 source_location 引用。
    [[nodiscard]] const std::source_location& where() const noexcept {
        return m_location;
    }

protected:
    std::string             m_message;       ///< 格式化后的错误字符串。
    std::source_location    m_location;      ///< 错误发生的文件/行号/函数名。
};

#if TFL_HAS_STACKTRACE
/// @brief 在基础错误消息和源码位置之外保存构造时的调用栈快照。
///
/// 类型按值拥有 `std::stacktrace`；帧的可用性和完整程度取决于标准库、平台和编译优化。
/// 仅在项目检测到 `std::stacktrace` 支持时定义。
class TraceException : public Exception {
public:
    /// @brief 类似 std::format 风格：接收格式字符串和参数，同时捕获源码位置与调用栈。
    /// @tparam Args 变长参数类型。
    /// @param fmt 包含格式串、源码位置与调用栈的 Traced 包装器。
    /// @param args 填充到格式串 {} 中的参数。
    template <typename... Args>
    explicit TraceException(Traced<std::format_string<Args...>> fmt, Args&&... args)
        // 复用 Exception 的消息格式化和源码位置保存逻辑；调用栈由本类独立保存。
        : Exception{static_cast<const Located<std::format_string<Args...>>&>(fmt), std::forward<Args>(args)...}
        , m_stacktrace{fmt.stacktrace()}
    {}

    /// @brief 直接传错误字符串并捕获调用栈的构造函数。
    /// @param message 错误描述文本。
    /// @param loc     throw 点的源码位置（默认实参自动捕获）。
    /// @param trace   捕获时的调用栈快照（默认实参自动抓取）。
    explicit TraceException(std::string_view message,
                            std::source_location loc = std::source_location::current(),
                            std::stacktrace trace = std::stacktrace::current())
        // 移动保存调用栈，避免复制 trace 对象。
        : Exception{message, loc}
        , m_stacktrace{std::move(trace)}
    {}

    /// @brief 获取构造异常时捕获的实现相关调用栈快照。
    /// @return 内部 stacktrace 的只读引用；帧完整性取决于平台和优化设置。
    [[nodiscard]] const std::stacktrace& trace() const noexcept {
        return m_stacktrace;
    }

private:
    std::stacktrace m_stacktrace; ///< 构造异常时捕获的调用栈快照。
};
#endif
}  // namespace tfl
