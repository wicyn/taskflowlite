#pragma once
#include <cassert>
// ============================================================================
// 内联和非内联
// ============================================================================
#if defined(_MSC_VER)
#define TFL_FORCE_INLINE __forceinline
#elif defined(__GNUC__) && __GNUC__ > 3
#define TFL_FORCE_INLINE __attribute__((__always_inline__)) inline
#else
#define TFL_FORCE_INLINE inline
#endif
#if defined(_MSC_VER)
#define TFL_NO_INLINE __declspec(noinline)
#elif defined(__GNUC__) && __GNUC__ > 3
#define TFL_NO_INLINE __attribute__((__noinline__))
#else
#define TFL_NO_INLINE
#endif
// ============================================================================
// 可能和不可能
// ============================================================================
#if defined(__GNUC__)
#define TFL_LIKELY(x) (__builtin_expect((x), 1))
#define TFL_UNLIKELY(x) (__builtin_expect((x), 0))
#else
#define TFL_LIKELY(x) (x)
#define TFL_UNLIKELY(x) (x)
#endif
// ----------------------------------------------------------------------------
#define TFL_FWD(T, x) std::forward<T>(x)
/**
 * @brief 用于有条件地装饰 lambda 和 ``operator()``（与 ``TFL_STATIC_CONST`` 一起使用）以
 * ``static``。
 */
#ifdef __cpp_static_call_operator
#define TFL_STATIC_CALL static
#else
#define TFL_STATIC_CALL
#endif
/**
 * @brief 与 ``TFL_STATIC_CALL`` 一起使用，以有条件地装饰 ``operator()`` 为 ``const``。
 */
#ifdef __cpp_static_call_operator
#define TFL_STATIC_CONST
#else
#define TFL_STATIC_CONST const
#endif
// clang-format off
/**
 * @brief 类似于 `BOOST_HOF_RETURNS`，用于定义带有所有 noexcept/requires/decltype 说明符的函数/lambda。
 *
 * 此宏并非真正可变参数，但 ``...`` 允许宏参数中包含逗号。
 */
#define TFL_HOF_RETURNS(...) noexcept(noexcept(__VA_ARGS__)) -> decltype(__VA_ARGS__) requires requires { __VA_ARGS__; } { return __VA_ARGS__;}
// clang-format on
/**
 * @brief __[public]__ 检测编译器是否启用了异常。
 *
 * 可通过定义 ``TFL_COMPILER_EXCEPTIONS`` 覆盖。
 */
#ifndef TFL_COMPILER_EXCEPTIONS
#if defined(__cpp_exceptions) || (defined(_MSC_VER) && defined(_CPPUNWIND)) || defined(__EXCEPTIONS)
#define TFL_COMPILER_EXCEPTIONS 1
#else
#define TFL_COMPILER_EXCEPTIONS 0
#endif
#endif
#if TFL_COMPILER_EXCEPTIONS || defined(TFL_DOXYGEN_SHOULD_SKIP_THIS)
/**
   * @brief 如果启用了异常，则展开为 ``try``，否则展开为 ``if constexpr (true)``。
   */
#define TFL_TRY try
/**
   * @brief 如果启用了异常，则展开为 ``catch (...)``，否则展开为 ``if constexpr
   * (false)``。
   */
#define TFL_CATCH_ALL catch (...)
/**
   * @brief 如果启用了异常，则展开为 ``throw X``，否则终止程序。
   */
#define TFL_THROW(X) throw X
/**
   * @brief 如果启用了异常，则展开为 ``throw``，否则终止程序。
   */
#define TFL_RETHROW throw
#else
#include <exception>
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
#ifdef __cpp_lib_unreachable
#include <utility>
#endif
#ifdef __cpp_lib_unreachable
using std::unreachable;
#else
/**
 * @brief `std::unreachable` 的自制版本，参见 https://en.cppreference.com/w/cpp/utility/unreachable
 */
[[noreturn]] inline void unreachable() {
// 如果可能，使用编译器特定扩展。
#if defined(_MSC_VER) && !defined(__clang__) // MSVC
    __assume(false);
#else // GCC, Clang
    __builtin_unreachable();
#endif
    // 即使未使用扩展，无限循环仍会引发未定义行为。
    for (;;) {
    }
}
#endif
/**
 * @brief 如果 ``expr`` 评估为 `false`，则调用未定义行为。
 *
 * \rst
 *
 * .. warning::
 *
 * 这与 ``[[assume(expr)]]`` 有不同的语义，因为它 WILL 在运行时评估
 * 表达式。因此，您应保守地仅使用此宏，如果 ``expr`` 无副作用且评估成本低。
 *
 * \endrst
 */
#define TFL_ASSUME(expr) \
do { \
        if (!(expr)) { \
            unreachable(); \
    } \
} while (false)
/**
 * @brief 如果定义了 ``NDEBUG``，则 ``TFL_ASSERT(expr)`` 为 `` ``，否则为 ``assert(expr)``。
 *
 * 这适用于带有副作用的表达式。
 */
#ifndef NDEBUG
#define TFL_ASSERT_NO_ASSUME(expr) assert(expr)
#else
#define TFL_ASSERT_NO_ASSUME(expr) \
    do { \
} while (false)
#endif
/**
 * @brief 如果定义了 ``NDEBUG``，则 ``TFL_ASSERT(expr)`` 为 ``TFL_ASSUME(expr)``，否则
 * 为 ``assert(expr)``。
 */
#ifndef NDEBUG
#define TFL_ASSERT(expr) assert(expr)
#else
#define TFL_ASSERT(expr) TFL_ASSUME(expr)
#endif
#define STATIC_ASSERT(_XP) \
    do { \
        static_assert(noexcept(_XP)); \
        STDEXEC_ASSERT_FN(_XP); \
} while (false)
/**
 * @brief 阻止函数内联的宏。
 */
#if !defined(TFL_NOINLINE)
#if defined(_MSC_VER) && !defined(__clang__)
#define TFL_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) && __GNUC__ > 3
// Clang 也定义了 __GNUC__ (为 4)
#if defined(__CUDACC__)
// nvcc 不总是解析 __noinline__，参见: https://svn.boost.org/trac/boost/ticket/9392
#define TFL_NOINLINE __attribute__((noinline))
#elif defined(__HIP__)
// 参见 https://github.com/boostorg/config/issues/392
#define TFL_NOINLINE __attribute__((noinline))
#else
#define TFL_NOINLINE __attribute__((__noinline__))
#endif
#else
#define TFL_NOINLINE
#endif
#endif
/**
 * @brief 为 Clang 强制非内联，绕过 https://github.com/llvm/llvm-project/issues/63022。
 *
 * TODO: 在发布 Xcode 16 时检查 __apple_build_version__。
 */
#if defined(__clang__)
#if defined(__apple_build_version__) || __clang_major__ <= 16
#define TFL_CLANG_TLS_NOINLINE TFL_NOINLINE
#else
#define TFL_CLANG_TLS_NOINLINE
#endif
#else
#define TFL_CLANG_TLS_NOINLINE
#endif
/**
 * @brief 在 'inline' 旁边使用的宏，以强制函数内联。
 *
 * \rst
 *
 * .. note::
 *
 * 这并不暗示 C++ 的 `inline` 关键字，该关键字还会影响链接。
 *
 * \endrst
 */
#if !defined(TFL_FORCEINLINE)
#if defined(_MSC_VER) && !defined(__clang__)
#define TFL_FORCEINLINE __forceinline
#elif defined(__GNUC__) && __GNUC__ > 3
// Clang 也定义了 __GNUC__ (为 4)
#define TFL_FORCEINLINE __attribute__((__always_inline__))
#else
#define TFL_FORCEINLINE
#endif
#endif
#if defined(__clang__) && defined(__has_attribute)
/**
   * @brief 编译器特定属性。
   */
#if __has_attribute(coro_return_type)
#define TFL_CORO_RETURN_TYPE [[clang::coro_return_type]]
#else
#define TFL_CORO_RETURN_TYPE
#endif
/**
   * @brief 编译器特定属性。
   */
#if __has_attribute(coro_only_destroy_when_complete)
#define TFL_CORO_ONLY_DESTROY_WHEN_COMPLETE [[clang::coro_only_destroy_when_complete]]
#else
#define TFL_CORO_ONLY_DESTROY_WHEN_COMPLETE
#endif
/**
   * @brief libfork 用于其协程类型的编译器特定属性。
   */
#define TFL_CORO_ATTRIBUTES TFL_CORO_RETURN_TYPE TFL_CORO_ONLY_DESTROY_WHEN_COMPLETE
#else
/**
   * @brief libfork 用于其协程类型的编译器特定属性。
   */
#define TFL_CORO_ATTRIBUTES
#endif
/**
 * @brief libfork 用于其协程类型的编译器特定属性。
 */
#if defined(__clang__) && defined(__has_attribute)
#if __has_attribute(coro_wrapper)
#define TFL_CORO_WRAPPER [[clang::coro_wrapper]]
#else
#define TFL_CORO_WRAPPER
#endif
#else
#define TFL_CORO_WRAPPER
#endif
/**
 * @brief __[public]__ 可自定义的日志记录宏。
 *
 * 默认情况下这是一个空操作。定义 ``TFL_DEFAULT_LOGGING`` 将启用默认
 * 日志实现，该实现打印到 ``std::cout``。可通过定义自己的 ``TFL_LOG`` 宏覆盖，该宏的 API 类似于 ``std::format()``。
 */
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
/**
 * @brief 连接宏
 */
#define TFL_CONCAT_OUTER(a, b) TFL_CONCAT_INNER(a, b)
/**
 * @brief 内部连接宏（使用 TFL_CONCAT_OUTER）
 */
#define TFL_CONCAT_INNER(a, b) a##b
/**
 * @brief 如果可用多维下标，则弃用 operator() 以利于 operator[]。
 */
#if defined(__cpp_multidimensional_subscript) && __cpp_multidimensional_subscript >= 202211L
#define TFL_DEPRECATE_CALL [[deprecated("Use operator[] instead of operator()")]]
#else
#define TFL_DEPRECATE_CALL
#endif
/**
 * @brief 展开为 ``_Pragma(#x)``。
 */
#define TFL_AS_PRAGMA(x) _Pragma(#x)
/**
 * @brief 如果编译器支持，则展开为 `#pragma unroll n` 或等效形式。
 */
#ifdef __clang__
#define TFL_PRAGMA_UNROLL(n) TFL_AS_PRAGMA(unroll n)
#elif defined(__GNUC__)
#define TFL_PRAGMA_UNROLL(n) TFL_AS_PRAGMA(GCC unroll n)
#else
#define TFL_PRAGMA_UNROLL(n)
#endif
#define TFL_DEFAULT_QUEUE_SIZE 1024
        // 必须为 2 的幂，例如 256, 512, 1024, 2048 ...
        static_assert((TFL_DEFAULT_QUEUE_SIZE & (TFL_DEFAULT_QUEUE_SIZE - 1)) == 0,"TFL_DEFAULT_QUEUE_SIZE must be power of 2");
// Debug 下启用
#ifndef NDEBUG
#define CHECK(expr) \
do { \
        if (!(expr)) { \
            auto loc = std::source_location::current(); \
            std::cerr << "CHECK failed: " << #expr \
            << "\n at " << loc.file_name() << ":" << loc.line() \
            << " in " << loc.function_name() << "\n"; \
            std::abort(); \
    } \
} while (0)
#define CHECK_EX(expr, msg) \
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
// Release 下禁用（什么都不做）
#define CHECK(expr) ((void)0)
#define CHECK_EX(expr, msg) ((void)0)
#endif
















// #pragma once
// #include <cassert>

// // ============================================================================
// // C++ Versions
// // ============================================================================
// #define TFL_CPP98 199711L
// #define TFL_CPP11 201103L
// #define TFL_CPP14 201402L
// #define TFL_CPP17 201703L
// #define TFL_CPP20 202002L

// // ============================================================================
// // inline and no-inline
// // ============================================================================

// #if defined(_MSC_VER)
//   #define TFL_FORCE_INLINE __forceinline
// #elif defined(__GNUC__) && __GNUC__ > 3
//   #define TFL_FORCE_INLINE __attribute__((__always_inline__)) inline
// #else
//   #define TFL_FORCE_INLINE inline
// #endif

// #if defined(_MSC_VER)
//   #define TFL_NO_INLINE __declspec(noinline)
// #elif defined(__GNUC__) && __GNUC__ > 3
//   #define TFL_NO_INLINE __attribute__((__noinline__))
// #else
//   #define TFL_NO_INLINE
// #endif

// // ============================================================================
// // likely and unlikely
// // ============================================================================

// #if defined(__GNUC__)
//   #define TFL_LIKELY(x) (__builtin_expect((x), 1))
//   #define TFL_UNLIKELY(x) (__builtin_expect((x), 0))
// #else
//   #define TFL_LIKELY(x) (x)
//   #define TFL_UNLIKELY(x) (x)
// #endif



// // ----------------------------------------------------------------------------

// #define TFL_FWD(T, x) std::forward<T>(x)


// /**
//  * @brief Use to conditionally decorate lambdas and ``operator()`` (alongside ``TFL_STATIC_CONST``) with
//  * ``static``.
//  */
// #ifdef __cpp_static_call_operator
// #define TFL_STATIC_CALL static
// #else
// #define TFL_STATIC_CALL
// #endif

// /**
//  * @brief Use with ``TFL_STATIC_CALL`` to conditionally decorate ``operator()`` with ``const``.
//  */
// #ifdef __cpp_static_call_operator
// #define TFL_STATIC_CONST
// #else
// #define TFL_STATIC_CONST const
// #endif

// // clang-format off

// /**
//  * @brief Use like `BOOST_HOF_RETURNS` to define a function/lambda with all the noexcept/requires/decltype specifiers.
//  *
//  * This macro is not truly variadic but the ``...`` allows commas in the macro argument.
//  */
// #define TFL_HOF_RETURNS(...) noexcept(noexcept(__VA_ARGS__)) -> decltype(__VA_ARGS__) requires requires { __VA_ARGS__; } { return __VA_ARGS__;}

// // clang-format on

// /**
//  * @brief __[public]__ Detects if the compiler has exceptions enabled.
//  *
//  * Overridable by defining ``TFL_COMPILER_EXCEPTIONS``.
//  */
// #ifndef TFL_COMPILER_EXCEPTIONS
// #if defined(__cpp_exceptions) || (defined(_MSC_VER) && defined(_CPPUNWIND)) || defined(__EXCEPTIONS)
// #define TFL_COMPILER_EXCEPTIONS 1
// #else
// #define TFL_COMPILER_EXCEPTIONS 0
// #endif
// #endif


// #if TFL_COMPILER_EXCEPTIONS || defined(TFL_DOXYGEN_SHOULD_SKIP_THIS)
// /**
//    * @brief Expands to ``try`` if exceptions are enabled, otherwise expands to ``if constexpr (true)``.
//    */
// #define TFL_TRY try
// /**
//    * @brief Expands to ``catch (...)`` if exceptions are enabled, otherwise expands to ``if constexpr
//    * (false)``.
//    */
// #define TFL_CATCH_ALL catch (...)
// /**
//    * @brief Expands to ``throw X`` if exceptions are enabled, otherwise terminates the program.
//    */
// #define TFL_THROW(X) throw X
// /**
//    * @brief Expands to ``throw`` if exceptions are enabled, otherwise terminates the program.
//    */
// #define TFL_RETHROW throw
// #else

// #include <exception>

// #define TFL_TRY if constexpr (true)
// #define TFL_CATCH_ALL if constexpr (false)
// #ifndef NDEBUG
// #define TFL_THROW(X) assert(false && "Tried to throw: " #X)
// #else
// #define TFL_THROW(X) std::terminate()
// #endif
// #ifndef NDEBUG
// #define TFL_RETHROW assert(false && "Tried to rethrow without compiler exceptions")
// #else
// #define TFL_RETHROW std::terminate()
// #endif
// #endif

// #ifdef __cpp_lib_unreachable
// #include <utility>
// #endif


// #ifdef __cpp_lib_unreachable
// using std::unreachable;
// #else
// /**
//  * @brief A homebrew version of `std::unreachable`, see https://en.cppreference.com/w/cpp/utility/unreachable
//  */
// [[noreturn]] inline void unreachable() {
// // Uses compiler specific extensions if possible.
// #if defined(_MSC_VER) && !defined(__clang__) // MSVC
//     __assume(false);
// #else                                        // GCC, Clang
//     __builtin_unreachable();
// #endif
//     // Even if no extension is used, undefined behavior is still raised by infinite loop.
//     for (;;) {
//     }
// }
// #endif

// /**
//  * @brief Invokes undefined behavior if ``expr`` evaluates to `false`.
//  *
//  * \rst
//  *
//  *  .. warning::
//  *
//  *    This has different semantics than ``[[assume(expr)]]`` as it WILL evaluate the
//  *    expression at runtime. Hence you should conservatively only use this macro
//  *    if ``expr`` is side-effect free and cheap to evaluate.
//  *
//  * \endrst
//  */

// #define TFL_ASSUME(expr)                                                                                      \
// do {                                                                                                       \
//         if (!(expr)) {                                                                                           \
//             unreachable();                                                                             \
//     }                                                                                                        \
// } while (false)

// /**
//  * @brief If ``NDEBUG`` is defined then ``TFL_ASSERT(expr)`` is  `` `` otherwise ``assert(expr)``.
//  *
//  * This is for expressions with side-effects.
//  */
// #ifndef NDEBUG
// #define TFL_ASSERT_NO_ASSUME(expr) assert(expr)
// #else
// #define TFL_ASSERT_NO_ASSUME(expr)                                                                          \
//     do {                                                                                                     \
// } while (false)
// #endif

// /**
//  * @brief If ``NDEBUG`` is defined then ``TFL_ASSERT(expr)`` is  ``TFL_ASSUME(expr)`` otherwise
//  * ``assert(expr)``.
//  */
// #ifndef NDEBUG
// #define TFL_ASSERT(expr) assert(expr)
// #else
// #define TFL_ASSERT(expr) TFL_ASSUME(expr)
// #endif


// #define STATIC_ASSERT(_XP)                                                                        \
// do {                                                                                             \
//         static_assert(noexcept(_XP));                                                                  \
//         STDEXEC_ASSERT_FN(_XP);                                                                        \
// } while (false)

// /**
//  * @brief Macro to prevent a function to be inlined.
//  */
// #if !defined(TFL_NOINLINE)
// #if defined(_MSC_VER) && !defined(__clang__)
// #define TFL_NOINLINE __declspec(noinline)
// #elif defined(__GNUC__) && __GNUC__ > 3
// // Clang also defines __GNUC__ (as 4)
// #if defined(__CUDACC__)
// // nvcc doesn't always parse __noinline__, see: https://svn.boost.org/trac/boost/ticket/9392
// #define TFL_NOINLINE __attribute__((noinline))
// #elif defined(__HIP__)
// // See https://github.com/boostorg/config/issues/392
// #define TFL_NOINLINE __attribute__((noinline))
// #else
// #define TFL_NOINLINE __attribute__((__noinline__))
// #endif
// #else
// #define TFL_NOINLINE
// #endif
// #endif

// /**
//  * @brief Force no-inline for clang, works-around https://github.com/llvm/llvm-project/issues/63022.
//  *
//  * TODO: Check __apple_build_version__ when xcode 16 is released.
//  */
// #if defined(__clang__)
// #if defined(__apple_build_version__) || __clang_major__ <= 16
// #define TFL_CLANG_TLS_NOINLINE TFL_NOINLINE
// #else
// #define TFL_CLANG_TLS_NOINLINE
// #endif
// #else
// #define TFL_CLANG_TLS_NOINLINE
// #endif

// /**
//  * @brief Macro to use next to 'inline' to force a function to be inlined.
//  *
//  * \rst
//  *
//  * .. note::
//  *
//  *    This does not imply the c++'s `inline` keyword which also has an effect on linkage.
//  *
//  * \endrst
//  */
// #if !defined(TFL_FORCEINLINE)
// #if defined(_MSC_VER) && !defined(__clang__)
// #define TFL_FORCEINLINE __forceinline
// #elif defined(__GNUC__) && __GNUC__ > 3
// // Clang also defines __GNUC__ (as 4)
// #define TFL_FORCEINLINE __attribute__((__always_inline__))
// #else
// #define TFL_FORCEINLINE
// #endif
// #endif

// #if defined(__clang__) && defined(__has_attribute)
// /**
//    * @brief Compiler specific attribute.
//    */
// #if __has_attribute(coro_return_type)
// #define TFL_CORO_RETURN_TYPE [[clang::coro_return_type]]
// #else
// #define TFL_CORO_RETURN_TYPE
// #endif
// /**
//    * @brief Compiler specific attribute.
//    */
// #if __has_attribute(coro_only_destroy_when_complete)
// #define TFL_CORO_ONLY_DESTROY_WHEN_COMPLETE [[clang::coro_only_destroy_when_complete]]
// #else
// #define TFL_CORO_ONLY_DESTROY_WHEN_COMPLETE
// #endif

// /**
//    * @brief Compiler specific attributes libfork uses for its coroutine types.
//    */
// #define TFL_CORO_ATTRIBUTES TFL_CORO_RETURN_TYPE TFL_CORO_ONLY_DESTROY_WHEN_COMPLETE

// #else
// /**
//    * @brief Compiler specific attributes libfork uses for its coroutine types.
//    */
// #define TFL_CORO_ATTRIBUTES
// #endif

// /**
//  * @brief Compiler specific attributes libfork uses for its coroutine types.
//  */
// #if defined(__clang__) && defined(__has_attribute)
// #if __has_attribute(coro_wrapper)
// #define TFL_CORO_WRAPPER [[clang::coro_wrapper]]
// #else
// #define TFL_CORO_WRAPPER
// #endif
// #else
// #define TFL_CORO_WRAPPER
// #endif

// /**
//  * @brief __[public]__ A customizable logging macro.
//  *
//  * By default this is a no-op. Defining ``TFL_DEFAULT_LOGGING`` will enable a default
//  * logging implementation which prints to ``std::cout``. Overridable by defining your
//  * own ``TFL_LOG`` macro with an API like ``std::format()``.
//  */
// #ifndef TFL_LOG
// #ifdef TFL_DEFAULT_LOGGING
// #include <iostream>
// #include <thread>
// #include <type_traits>

// #ifdef __cpp_lib_format
// #include <format>

// #define TFL_FORMAT(message, ...) std::format((message)__VA_OPT__(, ) __VA_ARGS__)
// #else
// #define TFL_FORMAT(message, ...) (message)
// #endif

// #ifdef __cpp_lib_syncbuf
// #include <syncstream>

// #define TFL_SYNC_COUT std::osyncstream(std::cout) << std::this_thread::get_id()
// #else
// #define TFL_SYNC_COUT std::cout << std::this_thread::get_id()
// #endif

// #define TFL_LOG(message, ...)                                                                             \
//     do {                                                                                                   \
//         if (!std::is_constant_evaluated()) {                                                                 \
//             TFL_SYNC_COUT << ": " << TFL_FORMAT(message __VA_OPT__(, ) __VA_ARGS__) << '\n';                     \
//     }                                                                                                    \
// } while (false)
// #else
// #define TFL_LOG(head, ...)
// #endif
// #endif

// /**
//  * @brief Concatenation macro
//  */
// #define TFL_CONCAT_OUTER(a, b) TFL_CONCAT_INNER(a, b)
// /**
//  * @brief Internal concatenation macro (use TFL_CONCAT_OUTER)
//  */
// #define TFL_CONCAT_INNER(a, b) a##b

// /**
//  * @brief Depreciate operator() in favor of operator[] if multidimensional subscript is available.
//  */
// #if defined(__cpp_multidimensional_subscript) && __cpp_multidimensional_subscript >= 202211L
// #define TFL_DEPRECATE_CALL [[deprecated("Use operator[] instead of operator()")]]
// #else
// #define TFL_DEPRECATE_CALL
// #endif

// /**
//  * @brief Expands to ``_Pragma(#x)``.
//  */
// #define TFL_AS_PRAGMA(x) _Pragma(#x)

// /**
//  * @brief Expands to `#pragma unroll n` or equivalent if the compiler supports it.
//  */
// #ifdef __clang__
// #define TFL_PRAGMA_UNROLL(n) TFL_AS_PRAGMA(unroll n)
// #elif defined(__GNUC__)
// #define TFL_PRAGMA_UNROLL(n) TFL_AS_PRAGMA(GCC unroll n)
// #else
// #define TFL_PRAGMA_UNROLL(n)
// #endif

// #define TFL_DEFAULT_QUEUE_SIZE 1024
// // 必须为 2 的幂，例如 256, 512, 1024, 2048 ...
// static_assert((TFL_DEFAULT_QUEUE_SIZE & (TFL_DEFAULT_QUEUE_SIZE - 1)) == 0,"TFL_DEFAULT_QUEUE_SIZE must be power of 2");

// // Debug 下启用
// #ifndef NDEBUG

// #define CHECK(expr) \
// do { \
//         if (!(expr)) { \
//             auto loc = std::source_location::current(); \
//             std::cerr << "CHECK failed: " << #expr \
//             << "\n  at " << loc.file_name() << ":" << loc.line() \
//             << " in " << loc.function_name() << "\n"; \
//             std::abort(); \
//     } \
// } while (0)

// #define CHECK_EX(expr, msg) \
//     do { \
//         if (!(expr)) { \
//             auto loc = std::source_location::current(); \
//             std::cerr << "CHECK failed: " << #expr \
//             << "\n  message: " << msg \
//             << "\n  at " << loc.file_name() << ":" << loc.line() \
//             << " in " << loc.function_name() << "\n"; \
//             std::abort(); \
//     } \
// } while (0)

// #else

// // Release 下禁用（什么都不做）
// #define CHECK(expr)       ((void)0)
// #define CHECK_EX(expr, msg) ((void)0)

// #endif





