/// @file free_stack.hpp
/// @brief 无锁侵入式栈 FreeStack —— Work 内存池的并发原语
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-10
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn
///
/// @details
/// `FreeStack` 是 Work 池的并发底座:无锁、ABA-safe Treiber 栈,
/// 节点是已析构的 chunk,链接复用 chunk 前 sizeof(void*) 字节。
///
/// ============================================================================
///  为什么链接放在 chunk 的前 8 字节
/// ============================================================================
/// chunk 在栈中处于 [basic.life]/5 "storage available, no object" 状态 ——
/// 旧对象已 dtor、新对象未 ctor,前 sizeof(void*) 字节是无主裸内存,
/// 标准明确允许复用。`ChunkLink` 用 std::memcpy 实现 store/load,避开
/// strict aliasing;-O2 下编译器折叠成单条 mov,零运行时开销。
///
/// ============================================================================
///  ABA 防护:双策略 + 平台自动选择
/// ============================================================================
/// 经典 Treiber 栈在 pop 期间被切出、对方完成 "pop A → pop B → push A"
/// 序列后回来 CAS 会误判成功 —— ABA 问题。本文件提供两套互补策略:
///
///   [优先] FreeStack128 —— 16 字节 tagged pointer
///       Tagged{ptr, tag} 整体作为 128-bit 原子,DWCAS 比较;
///       tag 为 64-bit,工程上等同 "永不 ABA"。
///       支持 5-level paging (LA57) / ARMv8.2 LVA / HWASAN / MTE / PAC 等
///       指针高位被占用的平台。
///       需 x86-64 cmpxchg16b 或 ARM64 LDXP/STXP / LSE CASP。
///
///   [回退] FreeStack48 —— 48-bit ptr + 16-bit tag 打包成 64-bit
///       单条 cmpxchg 完成,任何 x86-64/ARM64 都 always_lock_free,
///       无 ABI 依赖,延迟更低。
///       tag 仅 16-bit,2^16 次回卷一次 —— Work 池规模下不会触发 ABA。
///       仅适用于指针 ≤ 48-bit 的平台。
///
/// ============================================================================
///  平台自动选择
/// ============================================================================
/// 编译期判定两个独立维度:
///
///   kRequires128 —— 平台是否必须用 128-bit
///     ├─ TFL_PLATFORM_LA57            x86-64 5-level paging (用户 VA ≤ 57-bit)
///     ├─ TFL_PLATFORM_ARM64_LVA       ARMv8.2 LVA (用户 VA ≤ 52-bit)
///     ├─ __SANITIZE_HWADDRESS__       GCC HWASAN (高 8-bit 是标签)
///     ├─ __has_feature(hwaddress_sanitizer)  Clang HWASAN
///     ├─ __ARM_FEATURE_MEMORY_TAGGING ARMv8.5 MTE (高位是标签)
///     └─ __ARM_FEATURE_PAC_DEFAULT    ARMv8.3 PAC (高位是签名)
///
///   kHasLockFree128 —— DWCAS 是否硬件可用
///     ├─ x86-64:__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16 (GCC/Clang)
///     ├─ x86-64:_MSC_VER (MSVC 总是支持,运行时 OS 保证 CPU 兼容)
///     └─ aarch64:__ARM_FEATURE_ATOMICS (LSE)
///
/// 决策表:
///   kRequires128  kHasLockFree128   →  选择
///   ─────────────────────────────────────────────────
///        true          true         →  FreeStack128
///        true          false        →  #error (平台不可用)
///        false         true         →  FreeStack128
///        false         false        →  FreeStack48
///
/// 用户编译期开关:
///   TFL_FREESTACK_FORCE_128BIT   强制 128-bit;硬件不支持则报错。
///   TFL_FREESTACK_FORCE_48BIT    强制 48-bit;高地址平台不安全则报错。

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

// ---- HWASAN 归一化 (GCC 用 __SANITIZE_HWADDRESS__,Clang 用 __has_feature) --
#if defined(__has_feature)
#   if __has_feature(hwaddress_sanitizer)
#       define TFL_HAS_HWASAN 1
#   endif
#endif
#if defined(__SANITIZE_HWADDRESS__)
#   define TFL_HAS_HWASAN 1
#endif

/// @brief 平台是否必须用 128-bit (指针可能 >48-bit 或高位被污染)。
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
/// 不依赖 std::atomic<16B>::is_always_lock_free —— 它在某些 ABI 下即使
/// 硬件支持也返回 false (libstdc++ 走 libatomic 路径)。直接看编译器宏更准。
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

/// @brief ABA-safe lock-free LIFO 栈 (DWCAS 实现)。
///
/// chunk 必须满足 ChunkLink 的安全前提:
///   1. chunk 处于 "storage available, no object" 状态
///   2. chunk 起始按 alignof(void*) 对齐 (operator new 默认满足)
///   3. chunk 容量 >= sizeof(void*)
///
/// 整体 alignas(2*cache_line_size) 避免与相邻字段伪共享 —— pool 里多个
/// FreeStack 紧密排列,cache 行隔离很重要。
///
/// Note: 不在内部 static_assert 硬件能力,由顶层选择逻辑保证只在可用平台
/// 实例化本类。
class alignas(cache_line_size * 2) FreeStack128 : public Immovable<FreeStack128> {
public:
    /// @brief 构造空栈 —— 头指针置为 `{nullptr, 0}`。
    FreeStack128() noexcept {
        m_head.store(Tagged{nullptr, 0}, std::memory_order_relaxed);
    }

    /// @brief 检查栈是否为空 (快照,并发下可能立即过时)。
    [[nodiscard]] bool empty() const noexcept {
        return m_head.load(std::memory_order_relaxed).ptr == nullptr;
    }

    /// @brief 推入一个 chunk 到栈顶。
    /// @param p 待推入 chunk 起始地址;必须满足 ChunkLink 的安全前提。
    TFL_FORCE_INLINE void push(void* p) noexcept {
        Tagged curr = m_head.load(std::memory_order_relaxed);
        for (;;) {
            ChunkLink::store(p, curr.ptr);
            const Tagged next{p, curr.tag + 1};
            if (m_head.compare_exchange_weak(
                    curr, next,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                return;
            }
            // CAS 失败:curr 已自动重载到最新值,下轮重新链接重试
        }
    }

    /// @brief 从栈顶弹出一个 chunk;空则返回 nullptr。
    [[nodiscard]] TFL_FORCE_INLINE void* pop() noexcept {
        Tagged curr = m_head.load(std::memory_order_acquire);
        while (curr.ptr != nullptr) {
            void* link = ChunkLink::load(curr.ptr);
            const Tagged next{link, curr.tag + 1};
            if (m_head.compare_exchange_weak(
                    curr, next,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                return curr.ptr;
            }
        }
        return nullptr;
    }

private:
    using Tagged = detail::Tagged128;

    /// @brief head 单独占一个 cache line,避免与外部字段伪共享。
    alignas(cache_line_size) std::atomic<Tagged> m_head;
};


// ============================================================================
//  FreeStack48 —— 48-bit ptr + 16-bit tag 打包版本 (回退)
// ============================================================================

/// @brief ABA-safe lock-free LIFO 栈 (8 字节打包实现)。
///
/// chunk 必须满足 FreeStack128 的安全前提,外加:
///   4. chunk 地址在低 48 位 canonical 范围内 (operator new 默认满足;
///      LA57 / ARMv8.2 LVA / HWASAN / MTE / PAC 等平台不适用,
///      已由顶层选择逻辑在编译期排除)。
class alignas(cache_line_size * 2) FreeStack48 : public Immovable<FreeStack48> {
public:
    /// @brief 构造空栈 —— 头指针置为 0（48-bit packed 表示）。
    FreeStack48() noexcept {
        m_head.store(0, std::memory_order_relaxed);
    }

    /// @brief 检查栈是否为空 (快照,并发下可能立即过时)。
    [[nodiscard]] bool empty() const noexcept {
        return Tagged::ptr_of(m_head.load(std::memory_order_relaxed)) == nullptr;
    }

    /// @brief 推入一个 chunk 到栈顶。
    /// @param p 待推入 chunk 起始地址;必须满足前提且地址 < 2^48。
    TFL_FORCE_INLINE void push(void* p) noexcept {
        std::uint64_t curr = m_head.load(std::memory_order_relaxed);
        for (;;) {
            ChunkLink::store(p, Tagged::ptr_of(curr));
            const std::uint64_t next = Tagged::pack(p, Tagged::tag_of(curr) + 1);
            if (m_head.compare_exchange_weak(
                    curr, next,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                return;
            }
            // CAS 失败:curr 已自动重载到最新值,下轮重新链接重试
        }
    }

    /// @brief 从栈顶弹出一个 chunk;空则返回 nullptr。
    [[nodiscard]] TFL_FORCE_INLINE void* pop() noexcept {
        std::uint64_t curr = m_head.load(std::memory_order_acquire);
        for (;;) {
            void* top = Tagged::ptr_of(curr);
            if (top == nullptr) return nullptr;
            // Why: top 可能已被别的线程 pop 后再 push 回来,ChunkLink::load
            // 读到的 link 也许 "旧",但 tag 不匹配会让 CAS 失败,
            // 旧 link 不会污染 head —— ABA 由 tag 兜底。
            void* link = ChunkLink::load(top);
            const std::uint64_t next = Tagged::pack(link, Tagged::tag_of(curr) + 1);
            if (m_head.compare_exchange_weak(
                    curr, next,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                return top;
            }
        }
    }

private:
    // ========================================================================
    //  Tagged —— 48-bit ptr (低) | 16-bit tag (高) 打包/解包
    // ========================================================================
    /// @brief 把 (ptr, tag) 压成 64-bit,绕过 16-byte DWCAS 的 ABI 依赖。
    ///
    /// 全部为 static 纯函数,Tagged 本身无状态 —— 仅作命名空间用,无运行时开销。

    struct Tagged {
        // 容器总位宽 —— uint64_t 标准保证 64 位,此处显式表达意图
        static constexpr unsigned TOTAL_BITS = sizeof(std::uint64_t) * char_bits;
        // 平台契约: x86_64 / AArch64 用户态规范地址,有效虚地址 ≤ 48 位
        // 若未来需支持 LA57 / 52-bit AArch64,改此处即可,TAG_BITS 自动收缩
        static constexpr unsigned PTR_BITS = 48;
        static constexpr unsigned TAG_BITS = TOTAL_BITS - PTR_BITS;

        static constexpr unsigned      TAG_SHIFT = PTR_BITS;
        static constexpr std::uint64_t PTR_MASK  = (std::uint64_t{1} << PTR_BITS) - 1;
        static constexpr std::uint64_t TAG_MASK  = ((std::uint64_t{1} << TAG_BITS) - 1) << TAG_SHIFT;

        static_assert(TOTAL_BITS == 64, "Tagged depends on 64-bit uint64_t");
        static_assert(PTR_BITS + TAG_BITS == TOTAL_BITS);
        static_assert(PTR_BITS > 0 && TAG_BITS > 0);

        static void* ptr_of(std::uint64_t v) noexcept {
            return reinterpret_cast<void*>(v & PTR_MASK);
        }
        static std::uint64_t tag_of(std::uint64_t v) noexcept {
            return v >> TAG_SHIFT;
        }
        static std::uint64_t pack(void* p, std::uint64_t tag) noexcept {
            const auto pi = reinterpret_cast<std::uintptr_t>(p);
            assert((pi & ~PTR_MASK) == 0);                          // 指针契约
            assert(tag < (std::uint64_t{1} << TAG_BITS));           // tag 越界对称检查
            return (tag << TAG_SHIFT) | pi;
        }
        static bool is_canonical(void* p) noexcept {
            return (reinterpret_cast<std::uintptr_t>(p) & ~PTR_MASK) == 0;
        }
    };

    static_assert(sizeof(void*) == 8,
                  "FreeStack48 assumes 64-bit pointer (x86-64 / ARM64)");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "FreeStack48 requires lock-free 8-byte atomics");

    /// @brief head 单独占一个 cache line,避免与外部字段伪共享。
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

    static_assert(
        !(detail::kRequires128 && !detail::kHasLockFree128),
        "Platform requires FreeStack128 (high pointer bits used) "
        "but lock-free 128-bit CAS is unavailable on this target");

    /// @brief 平台最优 FreeStack 别名 —— 128-bit CAS 可用时选 `FreeStack128`，否则回退 `FreeStack48`。
    using FreeStack = std::conditional_t<
        detail::kRequires128 || detail::kHasLockFree128,
        FreeStack128,
        FreeStack48>;

#endif

}  // namespace tfl
