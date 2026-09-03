/// @file macros.hpp
/// @brief 提供 taskflowlite 框架底层的编译器指令封装、宏元编程工具与断言诊断体系。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cassert>

#include <climits>

// ============================================================================
// Assert
// ============================================================================

/// @brief 控制是否启用 taskflowlite 断言检查或编译器假设。
///
/// Debug 构建默认启用，并使用标准 `assert` 执行运行期检查；
/// Release 构建默认禁用。用户可在 Release 构建中手动启用，
/// 此时 `TFL_ASSERT` 退化为 `TFL_ASSUME`。
///
/// 配置值：
/// - 1：启用；
/// - 0：禁用。
///
/// 默认配置：
/// - Debug：1；
/// - Release：0。
///
/// @warning Release 构建手动启用后，`TFL_ASSERT` 不再提供运行期断言诊断；
/// 若表达式为 false，则进入不可达路径并导致未定义行为。
#ifndef TFL_ENABLE_ASSERT
#ifdef NDEBUG
#define TFL_ENABLE_ASSERT 0
#else
#define TFL_ENABLE_ASSERT 1
#endif
#endif

#if TFL_ENABLE_ASSERT != 0 && TFL_ENABLE_ASSERT != 1
#error "TFL_ENABLE_ASSERT must be defined as 0 or 1"
#endif

// ============================================================================
// Work Execution Check
// ============================================================================

/// @brief 控制是否启用 Work 执行生命周期一致性检查。
///
/// 启用后，每个 Work 从本轮首次进入执行开始到最终完成 tear-down 为止，
/// 都会保持执行标志；若在上一轮执行尚未结束时再次开始执行同一 Work，
/// 则视为调度或依赖状态发生内部一致性错误并立即终止程序。
///
/// 配置值：
/// - 1：启用 Work 执行生命周期检查；
/// - 0：禁用检查，不产生额外原子操作。
///
/// Debug 构建（未定义 `NDEBUG`）默认启用；Release 构建默认禁用。
/// 用户可在包含 taskflowlite 头文件之前自行定义该宏，或通过构建系统
/// 显式覆盖默认配置。
///
/// @code{.cpp}
/// #define TFL_ENABLE_WORK_EXECUTION_CHECK 1
/// #include <taskflowlite/taskflowlite.hpp>
/// @endcode
///
/// CMake 示例：
/// @code{.cmake}
/// target_compile_definitions(target PRIVATE TFL_ENABLE_WORK_EXECUTION_CHECK=1)
/// @endcode
///
/// @warning 同一程序中所有使用 taskflowlite 的翻译单元必须保持一致配置。
#ifndef TFL_ENABLE_WORK_EXECUTION_CHECK
#ifdef NDEBUG
#define TFL_ENABLE_WORK_EXECUTION_CHECK 0
#else
#define TFL_ENABLE_WORK_EXECUTION_CHECK 1
#endif
#endif

#if TFL_ENABLE_WORK_EXECUTION_CHECK != 0 && TFL_ENABLE_WORK_EXECUTION_CHECK != 1
#error "TFL_ENABLE_WORK_EXECUTION_CHECK must be defined as 0 or 1"
#endif

// ============================================================================
// Task Pool
// ============================================================================

/// @brief 控制是否启用 Work 对象池。
///
/// 启用后，Work 的创建与销毁通过共享 ObjectPool<Work> 完成，
/// 以复用已分配的节点内存并减少频繁 new/delete 带来的分配开销。
///
/// 配置值：
/// - 1：启用 Work 对象池；
/// - 0：禁用 Work 对象池，退化为直接使用 new/delete。
///
/// 默认启用。用户可在包含 taskflowlite 头文件之前自行定义该宏，
/// 或通过构建系统传入对应的预处理器定义。
///
/// @code{.cpp}
/// #define TFL_ENABLE_TASK_POOL 0
/// #include <taskflowlite/taskflowlite.hpp>
/// @endcode
///
/// CMake 示例：
/// @code{.cmake}
/// target_compile_definitions(target PRIVATE TFL_ENABLE_TASK_POOL=0)
/// @endcode
///
/// @warning 同一程序中所有使用 taskflowlite 的翻译单元必须保持一致配置。
#ifndef TFL_ENABLE_TASK_POOL
#define TFL_ENABLE_TASK_POOL 1
#endif

#if TFL_ENABLE_TASK_POOL != 0 && TFL_ENABLE_TASK_POOL != 1
#error "TFL_ENABLE_TASK_POOL must be defined as 0 or 1"
#endif


// ============================================================================
// Work Layout
// ============================================================================

/// @brief Work 内联 Payload 的总存储大小，单位为字节。
///
/// Payload 在固定大小存储区域中保存 Invoker。能够放入内联缓冲区的 Invoker
/// 直接原位构造；超过可用内联容量或具有更高对齐要求的类型则使用堆存储。
///
/// 该值直接影响：
/// - Work 对象大小；
/// - Payload 的 SBO（Small Buffer Optimization）容量；
/// - Work 对象池单个槽位的内存占用；
/// - Work 的二进制布局与 ABI。
///
/// 默认值为 128 字节。
///
/// @warning 修改该值会改变 Work 的对象布局和 ABI，同一程序中所有翻译单元
/// 必须使用完全一致的配置。
#ifndef TFL_WORK_PAYLOAD_SIZE
#define TFL_WORK_PAYLOAD_SIZE 128
#endif

static_assert(TFL_WORK_PAYLOAD_SIZE > 0, "TFL_WORK_PAYLOAD_SIZE must be greater than 0");


// ============================================================================
// Queue
// ============================================================================

/// @brief Executor 默认任务队列容量。
///
/// 该值用于需要固定初始容量或固定环形容量的内部任务队列。
/// 队列实现通过位掩码执行索引环绕，因此容量必须为非零的 2 的幂。
///
/// 默认容量为 1024。
///
/// 用户可在包含 taskflowlite 头文件之前定义该宏，或通过构建系统覆盖默认值。
///
/// @warning 同一程序中建议保持一致配置，避免不同翻译单元使用不同的默认容量。
#ifndef TFL_DEFAULT_QUEUE_SIZE
#define TFL_DEFAULT_QUEUE_SIZE 1024
#endif

static_assert(TFL_DEFAULT_QUEUE_SIZE > 0, "TFL_DEFAULT_QUEUE_SIZE must be greater than 0");
static_assert((TFL_DEFAULT_QUEUE_SIZE & (TFL_DEFAULT_QUEUE_SIZE - 1)) == 0, "TFL_DEFAULT_QUEUE_SIZE must be power of 2");


// ------------------------------------------------------------------------------------------------
// 确定无锁指针（如 TaggedHead64）的虚拟地址有效位数
// ------------------------------------------------------------------------------------------------

#if defined(TF_POINTER_BITS)
// 用户自定义覆盖

#elif defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
// 默认 4 级页表（48 位 VA）。
// 若系统开启了 LA57（5 级页表，57 位 VA），请在编译选项中加入 -DTF_POINTER_BITS=57
#define TF_POINTER_BITS 48

#elif defined(__aarch64__) || defined(_M_ARM64)
// 默认 48 位 VA (TTBR0 范围)。
// 若开启 FEAT_LPA/LPA2 (52 位 VA)，请加入 -DTF_POINTER_BITS=52
#define TF_POINTER_BITS 48

#elif defined(__riscv) && __riscv_xlen == 64
#define TF_POINTER_BITS 48   // 兼容 Sv39 与 Sv48

#elif defined(__SIZEOF_POINTER__)
// GCC / Clang 提供的预处理器安全宏（替代 sizeof）
#define TF_POINTER_BITS (__SIZEOF_POINTER__ * 8)

#elif defined(_WIN64)
#define TF_POINTER_BITS 64

#elif defined(_WIN32)
#define TF_POINTER_BITS 32

#else
#define TF_POINTER_BITS 32
#endif

// 编译期断言校验
static_assert(TF_POINTER_BITS > 0 && TF_POINTER_BITS <= 64, "TF_POINTER_BITS must be between 1 and 64");


// ============================================================================
// 对象布局优化
// ============================================================================

/// @brief 跨编译器封装 `[[no_unique_address]]` / `[[msvc::no_unique_address]]`。
/// @note 空类型成员可与其他子对象共享地址；具体对象布局由编译器决定。
/// @note 该宏只影响对象布局优化机会，不改变语义正确性。
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
// 内联控制指令
// ============================================================================

/// @brief 向支持的编译器强烈请求内联；编译器仍可因合法性或配置拒绝。
/// @note 形式带 `inline` 关键字，可直接用于在头文件中定义的函数。
#if defined(_MSC_VER)
#   define TFL_FORCE_INLINE __forceinline
#elif defined(__GNUC__) && __GNUC__ > 3
#   define TFL_FORCE_INLINE __attribute__((__always_inline__)) inline
#else
#   define TFL_FORCE_INLINE inline
#endif

/// @brief 向支持的编译器请求不要内联目标函数。
/// @note 适用于冷路径 (异常抛出、回退逻辑等)，减小热路径 i-cache 压力。
#if defined(_MSC_VER)
#   define TFL_NO_INLINE __declspec(noinline)
#elif defined(__GNUC__) && __GNUC__ > 3
#   define TFL_NO_INLINE __attribute__((__noinline__))
#else
#   define TFL_NO_INLINE
#endif

// 不含 inline 关键字的替代拼写：TFL_FORCEINLINE / TFL_NOINLINE。
// 与 TFL_FORCE_INLINE / TFL_NO_INLINE 的属性选择一致，但不添加 inline 关键字：
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
// 热 / 冷路径属性
// ============================================================================

/// @brief 标记函数为热路径，提示编译器优先优化、更激进地内联。
#if defined(__GNUC__) || defined(__clang__)
#   define TFL_HOT  __attribute__((hot))
#   define TFL_COLD __attribute__((cold))
#else
#   define TFL_HOT
#   define TFL_COLD
#endif


// ============================================================================
// 硬件预取指令
// ============================================================================

/// @brief 向处理器发出 `ptr` 所在缓存行的读预取提示；缓存层级和是否执行不受保证。
/// @note 在执行当前任务期间提前加载下一个 Work 节点，隐藏内存延迟。
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
// 自旋等待 PAUSE 指令
// ============================================================================

/// @brief 在自旋等待循环中发出 CPU PAUSE 提示，降低超线程竞争与功耗。
/// @note x86 使用 `pause`，ARM64 使用 `yield`，其他平台使用 signal fence。
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
// 不可达控制流与编译器假设
// ============================================================================

#if defined(__cpp_lib_unreachable) && __cpp_lib_unreachable >= 202202L
#include <utility>
using std::unreachable;
#else
/// @brief 标记控制流不可达；若实际执行到此处，行为未定义。
/// @note 在 C++23 `std::unreachable()` 不可用时使用编译器扩展实现。
[[noreturn]] inline void unreachable() noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
    __assume(false);
#elif defined(__GNUC__) || defined(__clang__)
    __builtin_unreachable();
#else
    for (;;) {
    }
#endif
}
#endif

/// @brief 向编译器声明表达式必定为 true。
/// @param expr 必须评估为 true 的布尔表达式。
/// @warning 该宏会在运行时求值 `expr`；表达式不得包含副作用。
/// 若结果为 false，则进入不可达路径并导致未定义行为。
#define TFL_ASSUME(expr)        \
do {                        \
        if (!(expr)) {          \
            unreachable();      \
    }                       \
} while (false)

/// @brief 仅执行 Debug 断言检查，Release 模式不求值表达式。
///
/// 该宏不会在 Release 模式退化为编译器假设，适用于仅用于诊断、
/// 不应参与 Release 优化假设的内部一致性检查。
#ifndef NDEBUG
#define TFL_ASSERT_NO_ASSUME(expr) assert(expr)
#else
#define TFL_ASSERT_NO_ASSUME(expr) ((void)0)
#endif

/// @brief taskflowlite 强断言宏。
///
/// Debug 模式自动使用标准 `assert` 执行运行期检查；Release 模式默认
/// 不求值表达式。仅当用户在 Release 模式显式启用
/// `TFL_ENABLE_ASSERT=1` 时，退化为 `TFL_ASSUME`。
///
/// @warning Release 模式启用后仍会求值表达式；若结果为 false，行为未定义。
#if TFL_ENABLE_ASSERT
#ifndef NDEBUG
#define TFL_ASSERT(expr) assert(expr)
#else
#define TFL_ASSERT(expr) TFL_ASSUME(expr)
#endif
#else
#define TFL_ASSERT(expr) ((void)0)
#endif

/// @brief 编译期要求表达式求值过程为 noexcept，并在 Debug 模式检查其结果。
///
/// `static_assert` 仅验证表达式是否可以无异常求值，不验证表达式在编译期
/// 是否为 true；实际真假检查仅在 Debug 模式执行。
#define STATIC_ASSERT(expr)             \
    do {                                \
        static_assert(noexcept(expr));  \
        TFL_ASSERT_NO_ASSUME(expr);     \
} while (false)



// ============================================================================
// 协程特定属性（Clang）
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
// 格式化与日志输出
// ============================================================================

/// @brief 可插拔的轻量级格式化日志宏。
/// @note 仅在定义 TFL_DEFAULT_LOGGING 时生效；支持 std::format 风格参数插值，
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
// 预处理器元编程工具
// ============================================================================

/// @brief 宏名二次求值拼接 (外层强制展开)。
#define TFL_CONCAT_OUTER(a, b) TFL_CONCAT_INNER(a, b)

/// @brief 宏名直接拼接底层实现。
#define TFL_CONCAT_INNER(a, b) a##b

/// @brief C++23 多维下标特性可用时，将 operator() 标记为弃用以鼓励迁移 operator[]。
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
// 附带现场转储的诊断断言
// ============================================================================

/// @brief Debug 模式输出源码位置并终止进程的诊断检查。
/// @note Debug 启用，失败时利用 C++20 std::source_location 输出文件、行号与函数名，
///       随后 abort；Release 模式不求值表达式。
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
