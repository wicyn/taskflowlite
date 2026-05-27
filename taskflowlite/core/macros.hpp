/// @file macros.hpp
/// @brief 提供 taskflowlite 框架底层的编译器指令封装、宏元编程工具与断言诊断体系。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cassert>


// ============================================================================
//  对象布局优化
// ============================================================================

/// @brief 跨编译器封装 `[[no_unique_address]]` / `[[msvc::no_unique_address]]`。
/// @details
/// 该属性用于告知编译器:若成员对象为空类型 (empty type),则可允许其
/// 不额外占用对象存储空间,从而尽可能压缩类对象体积。
/// @note 该宏只影响对象布局优化机会,不改变语义正确性。
#if defined(__has_cpp_attribute)
#   if __has_cpp_attribute(msvc::no_unique_address)
#       define TFL_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#   elif __has_cpp_attribute(no_unique_address) >= 201803L
#       define TFL_NO_UNIQUE_ADDRESS [[no_unique_address]]
#   else
#       define TFL_NO_UNIQUE_ADDRESS
#   endif
#else
#   define TFL_NO_UNIQUE_ADDRESS
#endif


// ============================================================================
//  内联控制指令
// ============================================================================

/// @brief 强制编译器对目标函数进行内联展开,忽略常规启发式阈值。
/// @note 形式带 `inline` 关键字,可直接用于在头文件中定义的函数。
#if defined(_MSC_VER)
#   define TFL_FORCE_INLINE __forceinline
#elif defined(__GNUC__) && __GNUC__ > 3
#   define TFL_FORCE_INLINE __attribute__((__always_inline__)) inline
#else
#   define TFL_FORCE_INLINE inline
#endif

/// @brief 显式禁止编译器对目标函数进行内联展开。
/// @note 适用于冷路径 (异常抛出、回退逻辑等),减小热路径 i-cache 压力。
#if defined(_MSC_VER)
#   define TFL_NO_INLINE __declspec(noinline)
#elif defined(__GNUC__) && __GNUC__ > 3
#   define TFL_NO_INLINE __attribute__((__noinline__))
#else
#   define TFL_NO_INLINE
#endif

// 历史别名:TFL_FORCEINLINE / TFL_NOINLINE
// Why: 与 TFL_FORCE_INLINE / TFL_NO_INLINE 语义一致,但 *不带* inline 关键字 ——
//      用于已显式写出 inline 或本身不需要 inline 的成员函数声明。
#if !defined(TFL_FORCEINLINE)
#   if defined(_MSC_VER) && !defined(__clang__)
#       define TFL_FORCEINLINE __forceinline
#   elif defined(__GNUC__) && __GNUC__ > 3
#       define TFL_FORCEINLINE __attribute__((__always_inline__))
#   else
#       define TFL_FORCEINLINE
#   endif
#endif

#if !defined(TFL_NOINLINE)
#   if defined(_MSC_VER) && !defined(__clang__)
#       define TFL_NOINLINE __declspec(noinline)
#   elif defined(__GNUC__) && __GNUC__ > 3
#       if defined(__CUDACC__) || defined(__HIP__)
#           define TFL_NOINLINE __attribute__((noinline))
#       else
#           define TFL_NOINLINE __attribute__((__noinline__))
#       endif
#   else
#       define TFL_NOINLINE
#   endif
#endif

/// @brief 专门针对 Clang 的非内联强制宏。
/// @note 规避 LLVM issue #63022:特定 Clang 版本下 TLS 变量内联后可能链接错误。
#if defined(__clang__)
#   if defined(__apple_build_version__) || __clang_major__ <= 16
#       define TFL_CLANG_TLS_NOINLINE TFL_NOINLINE
#   else
#       define TFL_CLANG_TLS_NOINLINE
#   endif
#else
#   define TFL_CLANG_TLS_NOINLINE
#endif


// ============================================================================
//  热 / 冷路径属性
// ============================================================================

/// @brief 标记函数为热路径,提示编译器优先优化、更激进地内联。
#if defined(__GNUC__) || defined(__clang__)
#   define TFL_HOT  __attribute__((hot))
#   define TFL_COLD __attribute__((cold))
#else
#   define TFL_HOT
#   define TFL_COLD
#endif


// ============================================================================
//  硬件预取指令
// ============================================================================

/// @brief 读预取:将 ptr 所在缓存行预取到 L1 (locality=3,最高驻留)。
/// @note 在执行当前任务期间提前加载下一个 Work 节点,隐藏内存延迟。
#if defined(__GNUC__) || defined(__clang__)
#   define TFL_PREFETCH_R(ptr) __builtin_prefetch((ptr), 0, 3)
#   define TFL_PREFETCH_W(ptr) __builtin_prefetch((ptr), 1, 1)
#elif defined(_MSC_VER)
#   include <immintrin.h>
#   define TFL_PREFETCH_R(ptr) _mm_prefetch(reinterpret_cast<const char*>(ptr), _MM_HINT_T0)
#   define TFL_PREFETCH_W(ptr) _mm_prefetch(reinterpret_cast<const char*>(ptr), _MM_HINT_T1)
#else
#   define TFL_PREFETCH_R(ptr) ((void)(ptr))
#   define TFL_PREFETCH_W(ptr) ((void)(ptr))
#endif


// ============================================================================
//  自旋等待 PAUSE 指令
// ============================================================================

/// @brief 在自旋等待循环中发出 CPU PAUSE 提示,降低超线程竞争与功耗。
/// @note x86: pause;ARM64: yield;其他:signal fence 兜底。
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#   ifdef _MSC_VER
#       include <immintrin.h>
#       define TFL_PAUSE() _mm_pause()
#   else
#       define TFL_PAUSE() __asm__ volatile("pause" ::: "memory")
#   endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#   define TFL_PAUSE() __asm__ volatile("yield" ::: "memory")
#else
#   include <atomic>
#   define TFL_PAUSE() std::atomic_signal_fence(std::memory_order_seq_cst)
#endif



// ============================================================================
//  未定义行为 (UB) 与编译器假设
// ============================================================================

#ifdef __cpp_lib_unreachable
#   include <utility>
using std::unreachable;
#else
/// @brief 触发硬性未定义行为 (UB),C++23 `std::unreachable()` 的 polyfill。
/// @note 帮助编译器消除永远不会执行到的分支 (如已穷举枚举值的 switch default)。
[[noreturn]] inline void unreachable() {
    // Why: 在 C++23 标准库可用前,利用编译器特定的内置扩展来削减死代码分支。
#   if defined(_MSC_VER) && !defined(__clang__)
    __assume(false);
#   else
    __builtin_unreachable();
#   endif
    // Why: 即使编译器不支持上述扩展,无限空循环也构成合法 C++ UB,起到类似提示作用。
    while (true) {
    }
}
#endif

/// @brief 断言指定表达式为 true,否则主动触发 UB 供编译器优化。
/// @param expr 必须评估为 true 的布尔表达式。
/// @warning 与 C++23 `[[assume(expr)]]` 不同,此宏在运行时必然求值,
///          仅适用于无副作用且开销极低的表达式!
#define TFL_ASSUME(expr)            \
do {                            \
        if (!(expr)) {              \
            unreachable();          \
    }                           \
} while (false)

/// @brief 仅保留断言侧作用的检测宏。
/// @note 在 NDEBUG (Release) 模式下直接消除,且不会退化为优化器假设。
#ifndef NDEBUG
#   define TFL_ASSERT_NO_ASSUME(expr) assert(expr)
#else
#   define TFL_ASSERT_NO_ASSUME(expr) ((void)0)
#endif

/// @brief 强断言宏:Debug 模式执行常规检查,Release 模式退化为编译器假设。
/// @warning 使用此宏包裹的表达式应当绝对纯粹 (无副作用),
///          否则 Release 模式下逻辑会发生改变!
#ifndef NDEBUG
#   define TFL_ASSERT(expr) assert(expr)
#else
#   define TFL_ASSERT(expr) TFL_ASSUME(expr)
#endif

/// @brief 复合断言：同时要求表达式在编译期（noexcept）与运行期均合法。
// Note: 内部 assert 实现透传 noexcept 检查后，再做运行期检测。
#define STATIC_ASSERT(_XP)              \
    do {                                \
        static_assert(noexcept(_XP));   \
        assert(_XP);                    \
} while (false)


// ============================================================================
//  协程特定属性 (Clang)
// ============================================================================

#if defined(__clang__) && defined(__has_attribute)
#   if __has_attribute(coro_return_type)
#       define TFL_CORO_RETURN_TYPE [[clang::coro_return_type]]
#   else
#       define TFL_CORO_RETURN_TYPE
#   endif
#   if __has_attribute(coro_only_destroy_when_complete)
#       define TFL_CORO_ONLY_DESTROY_WHEN_COMPLETE [[clang::coro_only_destroy_when_complete]]
#   else
#       define TFL_CORO_ONLY_DESTROY_WHEN_COMPLETE
#   endif
#   if __has_attribute(coro_wrapper)
#       define TFL_CORO_WRAPPER [[clang::coro_wrapper]]
#   else
#       define TFL_CORO_WRAPPER
#   endif
#   define TFL_CORO_ATTRIBUTES TFL_CORO_RETURN_TYPE TFL_CORO_ONLY_DESTROY_WHEN_COMPLETE
#else
#   define TFL_CORO_RETURN_TYPE
#   define TFL_CORO_ONLY_DESTROY_WHEN_COMPLETE
#   define TFL_CORO_WRAPPER
#   define TFL_CORO_ATTRIBUTES
#endif


// ============================================================================
//  格式化与日志输出
// ============================================================================

/// @brief 可插拔的轻量级格式化日志宏。
/// @note 仅在定义 TFL_DEFAULT_LOGGING 时生效;支持 std::format 风格参数插值,
///       并借 osyncstream 确保多线程输出不乱序。
#ifndef TFL_LOG
#   ifdef TFL_DEFAULT_LOGGING
#       include <iostream>
#       include <thread>
#       include <type_traits>
#       ifdef __cpp_lib_format
#           include <format>
#           define TFL_FORMAT(message, ...) std::format((message)__VA_OPT__(, ) __VA_ARGS__)
#       else
#           define TFL_FORMAT(message, ...) (message)
#       endif
#       ifdef __cpp_lib_syncbuf
#           include <syncstream>
#           define TFL_SYNC_COUT std::osyncstream(std::cout) << std::this_thread::get_id()
#       else
#           define TFL_SYNC_COUT std::cout << std::this_thread::get_id()
#       endif
#       define TFL_LOG(message, ...)                                                            \
    do {                                                                                \
            if (!std::is_constant_evaluated()) {                                            \
                TFL_SYNC_COUT << ": " << TFL_FORMAT(message __VA_OPT__(, ) __VA_ARGS__)     \
                << '\n';                                                      \
        }                                                                               \
    } while (false)
#   else
#       define TFL_LOG(head, ...)
#   endif
#endif


// ============================================================================
//  预处理器元编程工具
// ============================================================================

/// @brief 宏名二次求值拼接 (外层强制展开)。
#define TFL_CONCAT_OUTER(a, b) TFL_CONCAT_INNER(a, b)

/// @brief 宏名直接拼接底层实现。
#define TFL_CONCAT_INNER(a, b) a##b

/// @brief C++23 多维下标特性可用时,将 operator() 标记为弃用以鼓励迁移 operator[]。
#if defined(__cpp_multidimensional_subscript) && __cpp_multidimensional_subscript >= 202211L
#   define TFL_DEPRECATE_CALL [[deprecated("Use operator[] instead of operator()")]]
#else
#   define TFL_DEPRECATE_CALL
#endif

/// @brief 将宏参数转译为 _Pragma 指令的辅助宏。
#define TFL_AS_PRAGMA(x) _Pragma(#x)

/// @brief 循环展开的编译器指令。
/// @param n 期望展开的循环次数。
#ifdef __clang__
#   define TFL_PRAGMA_UNROLL(n) TFL_AS_PRAGMA(unroll n)
#elif defined(__GNUC__)
#   define TFL_PRAGMA_UNROLL(n) TFL_AS_PRAGMA(GCC unroll n)
#else
#   define TFL_PRAGMA_UNROLL(n)
#endif


// ============================================================================
//  系统全局常量
// ============================================================================

#define TFL_DEFAULT_QUEUE_SIZE 1024

        // Why: 强制队列尺寸为 2 的次幂 —— 无锁编程的常用技巧。
        //      允许将取模 (index % size) 替换为位与 (index & (size - 1)),极大提升吞吐量。
        static_assert((TFL_DEFAULT_QUEUE_SIZE & (TFL_DEFAULT_QUEUE_SIZE - 1)) == 0, "TFL_DEFAULT_QUEUE_SIZE must be power of 2");


// ============================================================================
//  附带现场转储的诊断断言
// ============================================================================

/// @brief 附带精确崩溃现场转储的强断言检测。
/// @note Debug 启用,失败时利用 C++20 std::source_location 输出文件、行号与函数名,
///       随后 abort。
#ifndef NDEBUG
#   include <iostream>
#   include <source_location>
#   include <cstdlib>
#   define TFL_CHECK(expr)                                              \
do {                                                            \
        if (!(expr)) {                                              \
            auto loc = std::source_location::current();             \
            std::cerr << "CHECK failed: " << #expr                  \
            << "\n at " << loc.file_name() << ":"         \
            << loc.line() << " in " << loc.function_name()\
            << "\n";                                      \
            std::abort();                                           \
    }                                                           \
} while (0)

/// @brief 附带自定义说明与崩溃现场转储的强断言检测。
#   define TFL_CHECK_EX(expr, msg)                                      \
    do {                                                            \
        if (!(expr)) {                                              \
            auto loc = std::source_location::current();             \
            std::cerr << "CHECK failed: " << #expr                  \
            << "\n message: " << (msg)                    \
            << "\n at " << loc.file_name() << ":"         \
            << loc.line() << " in " << loc.function_name()\
            << "\n";                                      \
            std::abort();                                           \
    }                                                           \
} while (0)
#else
#   define TFL_CHECK(expr)         ((void)0)
#   define TFL_CHECK_EX(expr, msg) ((void)0)
#endif
