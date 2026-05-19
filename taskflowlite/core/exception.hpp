/// @file exception.hpp
/// @brief 框架异常类 Exception / TraceException —— 自动捕获源码位置与调用栈
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <exception>
#include <string>
#include <source_location>
#include <format>
#include "utility.hpp"

namespace tfl {

/// @brief 框架基础异常类 —— 自动捕获源码位置 + std::format 风格消息。
///
/// @details
/// `Exception` 把 C++20 的 `std::source_location` 与 `std::format` 缝合到一起：
/// 抛出时 **零样板代码**，编译期完成格式串校验，运行期完成消息拼接 + 位置捕获。
///
/// ============================================================================
///  用法对比 —— 比 std::runtime_error 强在哪
/// ============================================================================
/// @code
///   // 传统：手写位置和拼接
///   throw std::runtime_error(
///       std::string{"foo failed at "} + __FILE__ + ":" + std::to_string(__LINE__) +
///       ", value=" + std::to_string(v));
///
///   // 本类：编译期校验 + 自动位置
///   throw Exception("foo failed, value={}", v);
///   //              ^^^^^^^^^^^^^^^^^^^^^^^^^
///   //              consteval 检查 {} 与 v 类型是否匹配
///   //              source_location 自动捕获
/// @endcode
///
/// ============================================================================
///  Located<T> 包装的设计动机
/// ============================================================================
/// 第一个参数是 `Located<std::format_string<Args...>>`：
/// - `format_string` 走 consteval 检查 {} 与 Args 匹配（不匹配 → 编译错）；
/// - `Located<T>` 用默认参数 `std::source_location::current()` 在调用方代码处
///   捕获位置 —— 每个 throw 点的位置都被准确记录。
///
/// 这是 C++20 元编程的优雅运用：模板包装把"必须显式传位置"改成"自动捕获 + 类型
/// 安全"。
///
/// ============================================================================
///  what() / where() 双访问
/// ============================================================================
/// - `what()` 实现 `std::exception` 接口，返回拼好的消息；
/// - `where()` 返回 `source_location`，可继续提取 file_name / line / function。
///
/// 既兼容标准 catch 链，又允许调试器 / 日志框架做更精细的诊断。
///
/// @see TraceException  附加 stacktrace 的重型版
/// @see Located         源码位置自动捕获的核心包装
/// @see TFL_THROW       常用便捷宏
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

/// @brief 重型异常 —— 在 Exception 基础上额外捕获完整调用栈。
///
/// @details
/// 当 source_location 不够（比如要追查"这个 throw 是从哪条递归路径触发的"），
/// `TraceException` 用 `std::stacktrace::current()` 抓整条调用链。
///
/// ============================================================================
///  代价
/// ============================================================================
/// `std::stacktrace::current()` 实测开销：
/// - Linux glibc：1-10 微秒（栈回溯 + 符号解析）；
/// - 带调试符号时更慢（要读 .debug_info）；
/// - 内存：每帧约 50-200 字节（depends on demangler）。
///
/// 这是 **几个数量级** 高于 `Exception` 的成本。所以本类的定位是"致命错误兜底
/// 用"，**不应进入热路径**。框架内部抛异常以 `Exception` 为主。
///
/// ============================================================================
///  设计决策：通过 #if TFL_HAS_STACKTRACE 条件编译
/// ============================================================================
/// `<stacktrace>` 是 C++23 标准库，编译器 / libstdc++ 支持参差。本类用宏护卫，
/// 不支持的编译器上自动消失，保证框架在无 stacktrace 环境也能编译通过。
///
/// @see Exception    轻量基类
/// @see Traced<T>    堆栈 + 位置自动捕获的核心包装
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
