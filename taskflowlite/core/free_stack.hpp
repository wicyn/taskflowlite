/// @file free_stack.hpp
/// @brief 无锁侵入式栈 FreeStack —— Work 内存池的并发原语
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-10
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn
///
/// @details
/// `FreeStack` 是 Work 池的并发底座：一个无锁、ABA-safe 的 Treiber 栈，
/// 节点是已析构的 chunk，链接通过 chunk 的前 `sizeof(void*)` 字节复用。
///
/// ============================================================================
///  为什么链接放在 chunk 的前 8 字节
/// ============================================================================
/// chunk 在栈中时处于 [basic.life]/5 描述的"storage available, no object"
/// 状态 —— 旧对象已 dtor、新对象未 ctor，前 sizeof(void*) 字节是无主裸内存，
/// 标准明确允许复用。`ChunkLink` 用 std::memcpy 实现 store/load，避开
/// strict aliasing 隐患；-O2 下编译器会折叠成单条 mov，零运行时开销。
///
/// ============================================================================
///  ABA 防护：128-bit tagged pointer
/// ============================================================================
/// 经典 Treiber 栈在 pop 期间被切出、对方完成"pop A → pop B → push A"
/// 序列后回来 CAS 会误判成功 —— ABA 问题。`Tagged{ptr, tag}` 把指针和
/// 单调递增 tag 绑成 16 字节原子，CAS 比较整体；tag 在 2^64 次操作内
/// 不会复现，工程上等同于"永不 ABA"。
///
/// 需要 x86-64 `cmpxchg16b`（GCC: -mcx16；MSVC: /arch:AVX 或更高）
/// 或 ARM64 `LDXP/STXP`。两者在主流目标平台都满足。如需兼容更老平台，
/// 可改用 64-bit 版（高 16 位放 tag、低 48 位放地址，依赖 x86-64 canonical
/// address）—— 见文末注释。
///
/// ============================================================================
///  生命周期约束
/// ============================================================================
/// FreeStack 故意不写析构 —— 与 taskflow NodePool 同款规避静态析构顺序
/// 问题（issue #363）：进程退出时若 thread 持有的 chunk 还未归还，析构
/// 全局栈可能踩到已死指针。leak 让 OS 回收是更鲁棒的工程取舍。
///
/// @see object_pool.hpp           使用本类的池实现
/// @see size_class_policy.hpp   决定每个 stack 容纳的 chunk 大小

#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <type_traits>

#include "macros.hpp"
#include "utility.hpp"

namespace tfl {

// ============================================================================
//  FreeStack —— 无锁侵入式栈
// ============================================================================

/// @brief ABA-safe lock-free LIFO 栈。
///
/// 多线程并发 push/pop 安全。chunk 必须满足 ChunkLink 的安全前提：
///   1. chunk 处于"storage available, no object"状态
///   2. chunk 起始按 alignof(void*) 对齐（operator new 默认满足）
///   3. chunk 容量 >= sizeof(void*)
///
/// 整体 alignas(2*cache_line_size) 避免与相邻字段伪共享 —— pool 里多个
/// FreeStack 紧密排列，cache 行隔离很重要。
class alignas(cache_line_size * 2) FreeStack : public Immovable<FreeStack> {

public:
    FreeStack() noexcept {
        m_head.store(Tagged{nullptr, 0}, std::memory_order_relaxed);
    }

    /// @brief 检查栈是否为空（快照，并发下可能立即过时）。
    [[nodiscard]] bool empty() const noexcept {
        return m_head.load(std::memory_order_relaxed).ptr == nullptr;
    }

    /// @brief 推入一个 chunk 到栈顶。
    /// @param p 待推入 chunk 的起始地址；必须满足 ChunkLink 的安全前提。
    TFL_FORCE_INLINE void push(void* p) noexcept {
        Tagged curr = m_head.load(std::memory_order_relaxed);
        for(;;) {
            ChunkLink::store(p, curr.ptr);
            const Tagged next{p, curr.tag + 1};
            if (m_head.compare_exchange_weak(
                    curr, next,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                return;
            }
            // CAS 失败时 curr 已被自动重载到最新值，重新链接重试
        }
    }

    /// @brief 从栈顶弹出一个 chunk；空则返回 nullptr。
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
    // 64 位平台：用 16 字节 tagged pointer
    struct alignas(16) Tagged {
        void*       ptr;
        std::size_t tag;
    };
    static_assert(sizeof(Tagged) == 16);

    static_assert(std::is_trivially_copyable_v<Tagged>,
                  "Tagged must be trivially copyable");

    /// @brief 与缓存行对齐 —— 防止 stack head 与相邻数据伪共享。
    /// 多个 stack 在同一数组里时，相邻 head 必须各占一行，否则
    /// 不同 size class 的 push/pop 会互相拖慢。
    alignas(cache_line_size) std::atomic<Tagged> m_head;
};

}  // namespace tfl::detail
