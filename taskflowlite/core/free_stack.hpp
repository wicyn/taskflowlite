/// @file  free_stack.hpp
/// @brief 无锁侵入式栈 FreeStack —— Work 内存池的并发原语 (ABA-safe Treiber 栈)。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "macros.hpp"
#include "utility.hpp"


namespace tfl {

namespace detail {

// ============================================================================
//  平台能力探测
// ============================================================================

// ---- HWASAN 归一化 (GCC 用 __SANITIZE_HWADDRESS__, Clang 用 __has_feature) --
#if defined(__has_feature)
#   if __has_feature(hwaddress_sanitizer)
#       define TFL_HAS_HWASAN 1
#   endif
#endif
#if defined(__SANITIZE_HWADDRESS__)
#   define TFL_HAS_HWASAN 1
#endif

/// @brief 平台是否必须用 128-bit (指针可能 > 48-bit 或高位被污染)。
/// @details LA57 / LVA / HWASAN / MTE / PAC 等会占用指针高位, 48-bit 打包不安全。
inline constexpr bool kRequires128 =
#if defined(TFL_PLATFORM_LA57)            || \
    defined(TFL_PLATFORM_ARM64_LVA)       || \
    defined(TFL_HAS_HWASAN)               || \
    defined(__ARM_FEATURE_MEMORY_TAGGING) || \
    defined(__ARM_FEATURE_PAC_DEFAULT)
    true
#else
    false
#endif
    ;

/// @brief DWCAS 是否硬件可用 (编译期判定)。
///
/// @details 不依赖 std::atomic<16B>::is_always_lock_free —— 它在某些 ABI 下
///          即使硬件支持也返回 false (libstdc++ 走 libatomic 路径)。
///          直接看编译器宏更准。
inline constexpr bool kHasLockFree128 =
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)
    true
#elif defined(_MSC_VER)
    true
#else
    false
#endif
#elif defined(__aarch64__)
#if defined(__ARM_FEATURE_ATOMICS)
    true
#else
    false
#endif
#else
        false
#endif
    ;


// ============================================================================
//  Tagged128 —— 16 字节 tagged pointer 布局
// ============================================================================

/// @brief 16 字节对齐的 tagged pointer (ptr 8B + tag 8B), 供 DWCAS 单指令比较。
struct alignas(16) Tagged128 {
    void*       ptr;
    std::size_t tag;
};
static_assert(sizeof(Tagged128) == 16);
static_assert(std::is_trivially_copyable_v<Tagged128>);

}  // namespace detail


// ============================================================================
//  FreeStack128 —— 16 字节 tagged pointer 版本 (优先)
// ============================================================================

/// @brief ABA-safe 无锁 LIFO 栈（128-bit tagged pointer DWCAS 实现）。
///
/// {ptr, tag} 整体作为 16 字节原子 CAS 比较，tag 为 64-bit 工程上等同"永不 ABA"。
/// 需 x86-64 cmpxchg16b 或 ARM64 LSE CASP 硬件支持。整体 alignas(2*cache_line_size) 避免伪共享。
///
/// @note chunk 处于 "storage available, no object" 状态，前 sizeof(void*) 字节可安全复用为链接指针。
class alignas(cache_line_size * 2) FreeStack128 : public Immovable<FreeStack128> {
public:
    /// @brief 构造空栈 —— 头指针置为 {nullptr, 0}。
    FreeStack128() noexcept {
        m_head.store(Tagged{nullptr, 0}, std::memory_order_relaxed);
    }

    /// @brief 检查栈是否为空 (快照, 并发下可能立即过时)。
    /// @return 头指针为 null 时返回 true。
    [[nodiscard]] bool empty() const noexcept {
        return m_head.load(std::memory_order_relaxed).ptr == nullptr;
    }

    /// @brief 推入一个 chunk 到栈顶 (release-CAS)。
    /// @param p 待推入 chunk 起始地址; 必须满足 ChunkLink 的安全前提。
    TFL_FORCE_INLINE void push(void* p) noexcept {
        Tagged curr = m_head.load(std::memory_order_relaxed);
        while (true) {
            ChunkLink::store(p, curr.ptr);
            const Tagged next{p, curr.tag + 1};
            if (m_head.compare_exchange_weak(curr, next, std::memory_order_release, std::memory_order_relaxed)) {
                return;
            }
            // CAS 失败: curr 已自动重载到最新值, 下轮重新链接重试
        }
    }

    /// @brief 从栈顶弹出一个 chunk (acquire-CAS); 空则返回 nullptr。
    /// @return chunk 起始地址, 或 nullptr (栈空)。
    [[nodiscard]] TFL_FORCE_INLINE void* pop() noexcept {
        Tagged curr = m_head.load(std::memory_order_acquire);
        while (curr.ptr != nullptr) {
            void* link = ChunkLink::load(curr.ptr);
            const Tagged next{link, curr.tag + 1};
            if (m_head.compare_exchange_weak(curr, next, std::memory_order_release, std::memory_order_acquire)) {
                return curr.ptr;
            }
        }
        return nullptr;
    }

private:
    using Tagged = detail::Tagged128;

    /// @brief head 单独占一个 cache line, 避免与外部字段伪共享。
    alignas(cache_line_size) std::atomic<Tagged> m_head;
};


// ============================================================================
//  FreeStack48 —— 48-bit ptr + 16-bit tag 打包版本 (回退)
// ============================================================================

/// @brief ABA-safe 无锁 LIFO 栈（48-bit ptr + 16-bit tag 打包为 64-bit CAS）。
///
/// 单条 cmpxchg 完成，任何 x86-64/ARM64 均 always_lock_free，延迟更低。
/// 仅适用于指针 <= 48-bit 的平台（非 LA57/LVA/HWASAN/MTE/PAC）。
///
/// @note 2^16 tag 在 Work 池规模下不会触发 ABA 回卷。
class alignas(cache_line_size * 2) FreeStack48 : public Immovable<FreeStack48> {
public:
    /// @brief 构造空栈 —— 头指针置为 0 (48-bit packed 表示)。
    FreeStack48() noexcept {
        m_head.store(0, std::memory_order_relaxed);
    }

    /// @brief 检查栈是否为空 (快照, 并发下可能立即过时)。
    /// @return 头指针为 null 时返回 true。
    [[nodiscard]] bool empty() const noexcept {
        return Tagged::ptr_of(m_head.load(std::memory_order_relaxed)) == nullptr;
    }

    /// @brief 推入一个 chunk 到栈顶 (release-CAS)。
    /// @param p 待推入 chunk 起始地址; 必须满足前提且地址 < 2^48。
    TFL_FORCE_INLINE void push(void* p) noexcept {
        std::uint64_t curr = m_head.load(std::memory_order_relaxed);
        while (true) {
            ChunkLink::store(p, Tagged::ptr_of(curr));
            const std::uint64_t next = Tagged::pack(p, Tagged::tag_of(curr) + 1);
            if (m_head.compare_exchange_weak(curr, next, std::memory_order_release, std::memory_order_relaxed)) {
                return;
            }
            // CAS 失败: curr 已自动重载到最新值, 下轮重新链接重试
        }
    }

    /// @brief 从栈顶弹出一个 chunk (acquire-CAS); 空则返回 nullptr。
    /// @return chunk 起始地址, 或 nullptr (栈空)。
    [[nodiscard]] TFL_FORCE_INLINE void* pop() noexcept {
        std::uint64_t curr = m_head.load(std::memory_order_acquire);
        while (true) {
            void* top = Tagged::ptr_of(curr);
            if (top == nullptr) return nullptr;
            // top 可能已被别的线程 pop 后再 push 回来, ChunkLink::load
            // 读到的 link 也许 "旧", 但 tag 不匹配会让 CAS 失败,
            // 旧 link 不会污染 head —— ABA 由 tag 兜底
            void* link = ChunkLink::load(top);
            const std::uint64_t next = Tagged::pack(link, Tagged::tag_of(curr) + 1);
            if (m_head.compare_exchange_weak(curr, next, std::memory_order_release, std::memory_order_acquire)) {
                return top;
            }
        }
    }

private:
    // ========================================================================
    //  Tagged —— 48-bit ptr (低) | 16-bit tag (高) 打包/解包
    // ========================================================================

    /// @brief 把 (ptr, tag) 压成 64-bit, 绕过 16-byte DWCAS 的 ABI 依赖。
    ///
    /// @details 全部为 static 纯函数, Tagged 本身无状态 —— 仅作命名空间用, 无运行时开销。
    struct Tagged {
        /// @brief 容器总位宽 —— uint64_t 标准保证 64 位。
        static constexpr unsigned TOTAL_BITS = sizeof(std::uint64_t) * char_bits;
        /// @brief 平台契约: x86_64 / AArch64 用户态规范地址, 有效虚地址 <= 48 位。
        /// 若未来需支持 LA57 / 52-bit AArch64, 改此处即可, TAG_BITS 自动收缩。
        static constexpr unsigned PTR_BITS = 48;
        static constexpr unsigned TAG_BITS = TOTAL_BITS - PTR_BITS;

        static constexpr unsigned      TAG_SHIFT = PTR_BITS;
        static constexpr std::uint64_t PTR_MASK  = (std::uint64_t{1} << PTR_BITS) - 1;
        static constexpr std::uint64_t TAG_MASK  = ((std::uint64_t{1} << TAG_BITS) - 1) << TAG_SHIFT;

        static_assert(TOTAL_BITS == 64, "Tagged depends on 64-bit uint64_t");
        static_assert(PTR_BITS + TAG_BITS == TOTAL_BITS);
        static_assert(PTR_BITS > 0 && TAG_BITS > 0);

        /// @brief 从 64-bit 值中提取指针 (低 48 位)。
        static void* ptr_of(std::uint64_t v) noexcept {
            return reinterpret_cast<void*>(v & PTR_MASK);
        }
        /// @brief 从 64-bit 值中提取 tag (高 16 位)。
        static std::uint64_t tag_of(std::uint64_t v) noexcept {
            return v >> TAG_SHIFT;
        }
        /// @brief 打包 (ptr, tag) -> uint64_t。
        /// @param p   必须在低 48 位 canonical 范围内。
        /// @param tag 必须 < 2^16。
        static std::uint64_t pack(void* p, std::uint64_t tag) noexcept {
            const auto pi = reinterpret_cast<std::uintptr_t>(p);
            assert((pi & ~PTR_MASK) == 0);                          ///< 指针契约检查
            assert(tag < (std::uint64_t{1} << TAG_BITS));           ///< tag 越界对称检查
            return (tag << TAG_SHIFT) | pi;
        }
        /// @brief 检查指针是否在低 48 位 canonical 范围内。
        static bool is_canonical(void* p) noexcept {
            return (reinterpret_cast<std::uintptr_t>(p) & ~PTR_MASK) == 0;
        }
    };

    static_assert(sizeof(void*) == 8, "FreeStack48 assumes 64-bit pointer (x86-64 / ARM64)");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "FreeStack48 requires lock-free 8-byte atomics");

    /// @brief head 单独占一个 cache line, 避免与外部字段伪共享。
    alignas(cache_line_size) std::atomic<std::uint64_t> m_head;
};


// ============================================================================
//  FreeStack —— 编译期平台选择
// ============================================================================

#if defined(TFL_FREESTACK_FORCE_128BIT) && defined(TFL_FREESTACK_FORCE_48BIT)
#   error "TFL_FREESTACK_FORCE_128BIT and TFL_FREESTACK_FORCE_48BIT are mutually exclusive"
#endif

#if defined(TFL_FREESTACK_FORCE_128BIT)

static_assert(detail::kHasLockFree128,
              "TFL_FREESTACK_FORCE_128BIT requires hardware DWCAS support "
              "(compile with -mcx16 / -march=armv8.1-a+lse / /arch:AVX)");
using FreeStack = FreeStack128;

#elif defined(TFL_FREESTACK_FORCE_48BIT)

static_assert(!detail::kRequires128,
              "TFL_FREESTACK_FORCE_48BIT is unsafe on this platform "
              "(LA57 / LVA / HWASAN / MTE / PAC pollutes pointer high bits)");
using FreeStack = FreeStack48;

#else

    static_assert(!(detail::kRequires128 && !detail::kHasLockFree128),
        "Platform requires FreeStack128 (high pointer bits used) "
        "but lock-free 128-bit CAS is unavailable on this target");

    /// @brief 平台最优 FreeStack 别名: 128-bit CAS 可用时选 FreeStack128，否则回退 FreeStack48。
    using FreeStack = std::conditional_t<detail::kRequires128 || detail::kHasLockFree128, FreeStack128, FreeStack48>;

#endif

}  // namespace tfl
