/// @file macros.hpp
/// @brief 提供 taskflowlite 框架底层的编译器指令封装、宏元编程工具与断言诊断体系。
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include <cassert>

// ============================================================================
//  内联控制指令
// ============================================================================

/// @brief 强制编译器对目标函数进行内联展开，忽略编译器的常规启发式阈值。
#if defined(_MSC_VER)
#define TFL_FORCE_INLINE __forceinline
#elif defined(__GNUC__) && __GNUC__ > 3
#define TFL_FORCE_INLINE __attribute__((__always_inline__)) inline
#else
#define TFL_FORCE_INLINE inline
#endif

/// @brief 显式禁止编译器对目标函数进行内联展开。
/// @note 适用于冷路径（如异常抛出、极少执行的回退逻辑），以减小热路径的指令缓存压力。
#if defined(_MSC_VER)
#define TFL_NO_INLINE __declspec(noinline)
#elif defined(__GNUC__) && __GNUC__ > 3
#define TFL_NO_INLINE __attribute__((__noinline__))
#else
#define TFL_NO_INLINE
#endif

// ============================================================================
//  分支预测优化
// ============================================================================

/// @brief 向编译器暗示该条件分支极大概率成立。
/// @param x 待评估的布尔表达式。
/// @note 编译器会将该分支的汇编指令排布在紧接条件跳转之后的直通路径上，优化指令流水线。
#if defined(__GNUC__)
#define TFL_LIKELY(x) (__builtin_expect((x), 1))
#else
#define TFL_LIKELY(x) (x)
#endif

/// @brief 向编译器暗示该条件分支极小概率成立（如异常检查、越界检查）。
/// @param x 待评估的布尔表达式。
#if defined(__GNUC__)
#define TFL_UNLIKELY(x) (__builtin_expect((x), 0))
#else
#define TFL_UNLIKELY(x) (x)
#endif

// ============================================================================
//  元编程与语言特性辅助
// ============================================================================

/// @brief 完美转发的简写宏。
// Why: 减少泛型编程中冗长且容易拼写错误的 std::forward<decltype(x)>(x) 样板代码。
#define TFL_FWD(T, x) std::forward<T>(x)

/// @brief 针对 C++23 `static operator()` 特性提供的条件修饰宏。
#ifdef __cpp_static_call_operator
#define TFL_STATIC_CALL static
#else
#define TFL_STATIC_CALL
#endif

/// @brief 针对 C++23 `static operator()` 配合使用的 const 修饰宏。
/// @note 当支持静态调用操作符时，该宏展开为空；否则展开为 `const` 以保证向下兼容。
#ifdef __cpp_static_call_operator
#define TFL_STATIC_CONST
#else
#define TFL_STATIC_CONST const
#endif

// clang-format off
/// @brief 辅助推导无状态 Lambda 或高阶函数的完整尾随返回类型与 noexcept 约束。
/// @note 内部运用 C++20 requires 表达式以确保 SFINAE 友好。
// Why: 避免在高度泛型的高阶函数中重复编写繁琐的 noexcept(...) -> decltype(...) requires ... 约束块。
#define TFL_HOF_RETURNS(...) noexcept(noexcept(__VA_ARGS__)) -> decltype(__VA_ARGS__) requires requires { __VA_ARGS__; } { return __VA_ARGS__;}
// clang-format on

// ============================================================================
//  异常安全抽象
// ============================================================================

/// @brief 检测当前编译环境是否启用了 C++ 异常机制。
/// @note 可在编译参数中通过显式定义 TFL_COMPILER_EXCEPTIONS 来覆盖自动探测。
#ifndef TFL_COMPILER_EXCEPTIONS
#if defined(__cpp_exceptions) || (defined(_MSC_VER) && defined(_CPPUNWIND)) || defined(__EXCEPTIONS)
#define TFL_COMPILER_EXCEPTIONS 1
#else
#define TFL_COMPILER_EXCEPTIONS 0
#endif
#endif

#if TFL_COMPILER_EXCEPTIONS || defined(TFL_DOXYGEN_SHOULD_SKIP_THIS)
#define TFL_TRY try
#define TFL_CATCH_ALL catch (...)
#define TFL_THROW(X) throw X
#define TFL_RETHROW throw
#else
#include <exception>
// Why: 在禁用异常 (-fno-exceptions) 的环境中，将 try-catch 块抹除，并将 throw 降级为断言或无条件终止。
#define TFL_TRY if constexpr (true)
#define TFL_CATCH_ALL if constexpr (false)
#ifndef NDEBUG
#define TFL_THROW(X) assert(false && "Tried to throw: " #X)
#else
#define TFL_THROW(X) std::terminate()
#endif
#ifndef NDEBUG
#define TFL_RETHROW assert(false && "Tried to rethrow without compiler exceptions")
#else
#define TFL_RETHROW std::terminate()
#endif
#endif

// ============================================================================
//  未定义行为 (UB) 与编译器假设
// ============================================================================

#ifdef __cpp_lib_unreachable
#include <utility>
using std::unreachable;
#else
/// @brief 触发硬性未定义行为 (UB)，相当于 C++23 `std::unreachable()` 的 polyfill。
/// @note 帮助编译器消除永远不会执行到的分支（如覆盖了所有枚举值的 switch 的 default 分支）。
[[noreturn]] inline void unreachable() {
// Why: 在 C++23 标准库可用前，利用编译器特定的内置扩展来削减死代码分支。
#if defined(_MSC_VER) && !defined(__clang__)
    __assume(false);
#else
    __builtin_unreachable();
#endif
    // Why: 即使编译器不支持上述内置扩展，无限空循环同样构成合法 C++ UB，能起到相似的提示作用。
    for (;;) {
    }
}
#endif

/// @brief 断言指定表达式评估为 true，否则主动触发 UB 供编译器优化。
/// @param expr 必须评估为 true 的布尔表达式。
/// @note 警告：与 C++23 `[[assume(expr)]]` 不同，此宏在运行时必然求值，仅适用于无副作用且开销极低的表达式！
#define TFL_ASSUME(expr) \
do { \
        if (!(expr)) { \
            unreachable(); \
    } \
} while (false)

/// @brief 仅保留断言侧作用的检测宏。
/// @note 在 NDEBUG (Release) 模式下直接消除，且不会退化为优化器假设 (ASSUME)。
#ifndef NDEBUG
#define TFL_ASSERT_NO_ASSUME(expr) assert(expr)
#else
#define TFL_ASSERT_NO_ASSUME(expr) \
    do { \
} while (false)
#endif

/// @brief 强断言宏：在 Debug 模式下执行常规检查，在 Release 模式下退化为编译器假设 (ASSUME)。
/// @note 使用此宏包裹的表达式应当绝对纯粹（无副作用），否则 Release 模式下逻辑会发生改变！
#ifndef NDEBUG
#define TFL_ASSERT(expr) assert(expr)
#else
#define TFL_ASSERT(expr) TFL_ASSUME(expr)
#endif

/// @brief 复合断言：同时要求表达式在编译期 (noexcept/静态) 和运行期均合法。
#define STATIC_ASSERT(_XP) \
    do { \
        static_assert(noexcept(_XP)); \
        STDEXEC_ASSERT_FN(_XP); \
} while (false)

// ============================================================================
//  特定平台规避指令
// ============================================================================

#if !defined(TFL_NOINLINE)
#if defined(_MSC_VER) && !defined(__clang__)
#define TFL_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) && __GNUC__ > 3
#if defined(__CUDACC__) || defined(__HIP__)
#define TFL_NOINLINE __attribute__((noinline))
#else
#define TFL_NOINLINE __attribute__((__noinline__))
#endif
#else
#define TFL_NOINLINE
#endif
#endif

/// @brief 专门针对 Clang 的非内联强制宏。
/// @note 规避 LLVM issue #63022：特定 Clang 版本下 Thread Local Storage (TLS) 变量在内联后可能导致链接错误。
#if defined(__clang__)
#if defined(__apple_build_version__) || __clang_major__ <= 16
#define TFL_CLANG_TLS_NOINLINE TFL_NOINLINE
#else
#define TFL_CLANG_TLS_NOINLINE
#endif
#else
#define TFL_CLANG_TLS_NOINLINE
#endif

#if !defined(TFL_FORCEINLINE)
#if defined(_MSC_VER) && !defined(__clang__)
#define TFL_FORCEINLINE __forceinline
#elif defined(__GNUC__) && __GNUC__ > 3
#define TFL_FORCEINLINE __attribute__((__always_inline__))
#else
#define TFL_FORCEINLINE
#endif
#endif

// ============================================================================
//  协程特定属性 (针对 Clang)
// ============================================================================

#if defined(__clang__) && defined(__has_attribute)
#if __has_attribute(coro_return_type)
#define TFL_CORO_RETURN_TYPE [[clang::coro_return_type]]
#else
#define TFL_CORO_RETURN_TYPE
#endif

#if __has_attribute(coro_only_destroy_when_complete)
#define TFL_CORO_ONLY_DESTROY_WHEN_COMPLETE [[clang::coro_only_destroy_when_complete]]
#else
#define TFL_CORO_ONLY_DESTROY_WHEN_COMPLETE
#endif

#define TFL_CORO_ATTRIBUTES TFL_CORO_RETURN_TYPE TFL_CORO_ONLY_DESTROY_WHEN_COMPLETE
#else
#define TFL_CORO_ATTRIBUTES
#endif

#if defined(__clang__) && defined(__has_attribute)
#if __has_attribute(coro_wrapper)
#define TFL_CORO_WRAPPER [[clang::coro_wrapper]]
#else
#define TFL_CORO_WRAPPER
#endif
#else
#define TFL_CORO_WRAPPER
#endif

// ============================================================================
//  格式化与日志输出
// ============================================================================

/// @brief 可插拔的轻量级格式化日志宏。
/// @note 只有在定义了 TFL_DEFAULT_LOGGING 时才生效。支持 std::format 风格的参数插值，并确保多线程输出不乱序（osyncstream）。
#ifndef TFL_LOG
#ifdef TFL_DEFAULT_LOGGING
#include <iostream>
#include <thread>
#include <type_traits>
#ifdef __cpp_lib_format
#include <format>
#define TFL_FORMAT(message, ...) std::format((message)__VA_OPT__(, ) __VA_ARGS__)
#else
#define TFL_FORMAT(message, ...) (message)
#endif
#ifdef __cpp_lib_syncbuf
#include <syncstream>
#define TFL_SYNC_COUT std::osyncstream(std::cout) << std::this_thread::get_id()
#else
#define TFL_SYNC_COUT std::cout << std::this_thread::get_id()
#endif
#define TFL_LOG(message, ...) \
    do { \
            if (!std::is_constant_evaluated()) { \
                TFL_SYNC_COUT << ": " << TFL_FORMAT(message __VA_OPT__(, ) __VA_ARGS__) << '\n'; \
        } \
    } while (false)
#else
#define TFL_LOG(head, ...)
#endif
#endif

// ============================================================================
//  预处理器元编程工具
// ============================================================================

/// @brief 宏名二次求值拼接。
#define TFL_CONCAT_OUTER(a, b) TFL_CONCAT_INNER(a, b)

/// @brief 宏名直接拼接底层实现。
#define TFL_CONCAT_INNER(a, b) a##b

/// @brief 针对 C++23 多维下标特性可用时，将 operator() 标记为弃用以鼓励迁移至 operator[]。
#if defined(__cpp_multidimensional_subscript) && __cpp_multidimensional_subscript >= 202211L
#define TFL_DEPRECATE_CALL [[deprecated("Use operator[] instead of operator()")]]
#else
#define TFL_DEPRECATE_CALL
#endif

/// @brief 将宏参数转译为 _Pragma 指令的辅助宏。
#define TFL_AS_PRAGMA(x) _Pragma(#x)

/// @brief 循环展开的编译器指令。
/// @param n 期望展开的循环次数。
#ifdef __clang__
#define TFL_PRAGMA_UNROLL(n) TFL_AS_PRAGMA(unroll n)
#elif defined(__GNUC__)
#define TFL_PRAGMA_UNROLL(n) TFL_AS_PRAGMA(GCC unroll n)
#else
#define TFL_PRAGMA_UNROLL(n)
#endif

// ============================================================================
//  系统全局常量与诊断模块
// ============================================================================

#define TFL_DEFAULT_QUEUE_SIZE 1024

        // Why: 强制队列尺寸为 2 的次幂，是无锁编程中最常用的技巧。
        // 它允许我们将昂贵的取模运算 (index % size) 替换为极低开销的位与运算 (index & (size - 1))，极大提升队列吞吐量。
        static_assert((TFL_DEFAULT_QUEUE_SIZE & (TFL_DEFAULT_QUEUE_SIZE - 1)) == 0,
                      "TFL_DEFAULT_QUEUE_SIZE must be power of 2");

/// @brief 附带精确崩溃现场转储的强断言检测。
/// @note 在 Debug 模式下启用，失败时利用 C++20 std::source_location 输出确切的文件、行号与函数名，随后中断进程。
#ifndef NDEBUG
#define TFL_CHECK(expr) \
do { \
        if (!(expr)) { \
            auto loc = std::source_location::current(); \
            std::cerr << "CHECK failed: " << #expr \
            << "\n at " << loc.file_name() << ":" << loc.line() \
            << " in " << loc.function_name() << "\n"; \
            std::abort(); \
    } \
} while (0)

/// @brief 附带自定义说明与崩溃现场转储的强断言检测。
#define TFL_CHECK_EX(expr, msg) \
    do { \
        if (!(expr)) { \
            auto loc = std::source_location::current(); \
            std::cerr << "CHECK failed: " << #expr \
            << "\n message: " << msg \
            << "\n at " << loc.file_name() << ":" << loc.line() \
            << " in " << loc.function_name() << "\n"; \
            std::abort(); \
    } \
} while (0)
#else
// Release 模式下完全剔除代码生成
#define TFL_CHECK(expr) ((void)0)
#define TFL_CHECK_EX(expr, msg) ((void)0)
#endif
