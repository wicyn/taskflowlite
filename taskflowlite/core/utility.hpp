/// @file  utility.hpp
/// @brief 框架基础工具集 —— CRTP 策略基类、安全类型转换、源码位置包装、向量映射等。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <atomic>
#include <source_location>
#include <limits>
#include <type_traits>
#include <string>
#include <string_view>
#include <typeinfo>
#include <vector>
#include <utility>

// 替换原来的 #include <stacktrace>
#if __has_include(<stacktrace>)
#  include <stacktrace>
#endif

// 放到 macros.hpp 或 utility.hpp 顶部皆可
#if defined(__cpp_lib_stacktrace) && __cpp_lib_stacktrace >= 202011L
#  define TFL_HAS_STACKTRACE 1
#else
#  define TFL_HAS_STACKTRACE 0
#endif

#include "macros.hpp"

namespace tfl {

// 跨平台且规避 Clang 陷阱的缓存行大小推导
#if defined(__cpp_lib_hardware_interference_size) && !defined(__GNUC__)
    // 1. 如果编译器和标准库完整支持 C++17 特性，优先使用标准库
inline constexpr std::size_t cache_line_size = std::hardware_destructive_interference_size;
#else
    // 2. 否则，根据编译器宏和目标架构进行精细化推导
#if defined(__APPLE__) && defined(__aarch64__)
    // 重要补充：Apple Silicon (M1/M2/M3) 的 L1 缓存行大小是 128 字节！
inline constexpr std::size_t cache_line_size = 128;
#elif defined(__powerpc64__)
    // PowerPC64 (如 Power7) 的 L1 D-cache 缓存行大小
inline constexpr std::size_t cache_line_size = 128;
#elif defined(__s390x__)
    // IBM z/Architecture 通常是 256 字节
inline constexpr std::size_t cache_line_size = 256;
#elif defined(__arm__)
    // 32位 ARM 处理器的兼容处理
#if defined(__ARM_ARCH_5T__)
inline constexpr std::size_t cache_line_size = 32;
#else
inline constexpr std::size_t cache_line_size = 64;
#endif
#elif defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    // x86 和 x86_64 架构（包含 MSVC 的宏 _M_IX86 / _M_X64）绝大多数是 64 字节
inline constexpr std::size_t cache_line_size = 64;
#else
    // 3. 合理的默认猜测：高估会浪费一点内存，低估则会导致伪共享浪费大量时间
inline constexpr std::size_t cache_line_size = 64;
#endif
#endif


/// @brief 每字节的二进制位数，等价于 C 的 CHAR_BIT。
inline constexpr unsigned char_bits = std::numeric_limits<unsigned char>::digits;

// ============================================================================
//  ChunkLink —— 在已析构内存上原子地 store/load 链接指针
// ============================================================================
/// @brief chunk-as-link 工具：在 chunk 的前 sizeof(void*) 字节做指针存取。
///
/// 安全前提：
///   1. chunk 处于 "storage available, no object" 状态（dtor 已返回 / 未构造）
///   2. chunk 起始按 alignof(void*) 对齐 —— operator new 默认满足
///   3. chunk 容量 >= sizeof(void*) —— 由 size class policy 的最小档保证
///
/// 为什么用 std::atomic_ref 而非 memcpy：
///   FreeStack(Treiber 栈) 的 pop() 会在 CAS 成功前"投机"读取节点的 link 字段，
///   此时该节点可能已被另一线程 pop 走并交给用户写入——非原子 memcpy 读 + 用户写
///   构成数据竞争(UB), TSan 报 race。改用 atomic_ref relaxed 后, 该读成为
///   well-defined 的原子读: 投机读到的值仍会被随后失败的 CAS 丢弃, 语义不变,
///   但不再是数据竞争。relaxed 足够: 节点间的可见性由 m_head 的 acq/rel 建立。
struct ChunkLink {
    TFL_FORCE_INLINE static void store(void* chunk, void* next) noexcept {
        std::atomic_ref<void*> link{*static_cast<void**>(chunk)};
        link.store(next, std::memory_order_relaxed);
    }

    [[nodiscard]] TFL_FORCE_INLINE static void* load(const void* chunk) noexcept {
        // chunk 内容此刻是裸 storage，对其首 void* 做原子读
        std::atomic_ref<void*> link{
                                     *static_cast<void**>(const_cast<void*>(chunk))};
        return link.load(std::memory_order_relaxed);
    }
};

/// @brief CRTP 空基类：禁止拷贝与移动，保证构造后地址不变。
///
/// 禁掉全部四种特殊成员。模板化 CRTP 避免多重继承冲突，EBO 不增加派生类大小。
/// @tparam CRTP 派生类类型
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

/// @brief CRTP 空基类: 允许移动语义但禁止拷贝。
///
/// 适用于需要转移所有权的对象（如 Flow 或 AsyncTask）。
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

    /// @brief 获取被包装的基础值，即构造时传入的格式串或消息 payload。
    /// @return 对内部存储 T 的常量引用。
    constexpr const T& format() const noexcept { return m_inner; }

    /// @brief 获取该包装器构造时自动捕获的源码位置（文件、行号、函数名）。
    /// @return std::source_location 结构体的常量引用。
    constexpr const std::source_location& location() const noexcept { return m_loc; }
};

/// @brief 带有堆栈跟踪与源码位置的值包装器。
///
/// 继承自 `Located` 并额外附加 `std::stacktrace`。适用于需要完整调用链诊断信息的场景。
///
/// @tparam T 被包装的基础类型。
/// @note 捕获堆栈涉及运行时栈回溯开销，仅应在异常或诊断错误路径使用。
#if TFL_HAS_STACKTRACE
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

    /// @brief 获取该包装器构造时自动捕获的完整调用栈快照。
    /// @return std::stacktrace 对象的常量引用，包含从 throw 点到 main 的调用链。
    constexpr const std::stacktrace& stacktrace() const noexcept { return m_trace; }
};
#endif
} // namespace tfl
