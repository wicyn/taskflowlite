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

// ============================================================================
//  FreeStack
// ============================================================================

/// @brief ABA-safe 无锁 LIFO 栈（128-bit tagged pointer DWCAS 实现）。
///
/// {ptr, tag} 整体作为 16 字节原子 CAS 比较，tag 为 64-bit 工程上等同"永不 ABA"。
/// 需 x86-64 cmpxchg16b 或 ARM64 LSE CASP 硬件支持。整体 alignas(2*cache_line_size) 避免伪共享。
///
/// @note chunk 处于 "storage available, no object" 状态，前 sizeof(void*) 字节可安全复用为链接指针。
class alignas(cache_line_size * 2) FreeStack : public Immovable<FreeStack> {
public:
    /// @brief 构造空栈 —— 头指针置为 {nullptr, 0}。
    FreeStack() noexcept {
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
        for (;;) {
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

    // ============================================================================
    //  Tagged —— 16 字节 tagged pointer 布局
    // ============================================================================

    /// @brief 16 字节对齐的 tagged pointer (ptr 8B + tag 8B), 供 DWCAS 单指令比较。
    struct alignas(16) Tagged {
        void*       ptr;
        std::size_t tag;
    };
    static_assert(sizeof(Tagged) == 16);
    static_assert(std::is_trivially_copyable_v<Tagged>);


    /// @brief head 单独占一个 cache line, 避免与外部字段伪共享。
    alignas(cache_line_size) std::atomic<Tagged> m_head;
};


}  // namespace tfl
