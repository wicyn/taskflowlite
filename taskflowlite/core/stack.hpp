/// @file stack.hpp
/// @brief Stacklet 与 Stack —— Coro 分配栈段与 Worker 空闲栈段缓存。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @license MIT

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

#include "macros.hpp"
#include "utility.hpp"
namespace tfl {

/// @brief 由 Coro 独占的 Coroutine Frame 栈段。
///
/// 栈段头部和用户存储区域位于同一块 malloc 内存中，整块大小为 2 的幂。
/// 栈段由 Stack 统一创建。
/// m_prev 连接同一 Coro 的活动栈段；m_next 只在栈段退空且归还 Worker 后使用。
/// 一个栈段不能同时属于 Coro 和 Worker 的空闲缓存。
///
/// @warning try_allocate / deallocate 必须严格遵循 LIFO；不负责析构 Frame。
/// @warning 只保证不超过 alignof(std::max_align_t) 的对齐。
class alignas(alignof(std::max_align_t)) Stacklet final : public Immovable<Stacklet> {
    friend class Stack;

public:
    /// @brief 默认栈段总大小（含 Stacklet 头部）。
    static constexpr std::size_t default_size = 4096;

    /// @brief 栈内存储的保证对齐。
    static constexpr std::size_t alignment = alignof(std::max_align_t);

    static_assert(std::has_single_bit(default_size));
    static_assert(std::has_single_bit(alignment));
    static_assert(default_size % alignment == 0);

    /// @brief 尝试分配 size 字节，空间不足返回 nullptr，不创建新段。
    /// @pre size > 0。
    [[nodiscard]] void* try_allocate(std::size_t size) noexcept {
        TFL_ASSERT(size > 0);
        if (size > available()) [[unlikely]] {
            return nullptr;
        }
        // 剩余空间已按 alignment 对齐，通过容量检查后向上取整不会溢出或越界。
        constexpr std::size_t mask = alignment - 1;
        const std::size_t extent = (size + mask) & ~mask;
        return std::exchange(m_sp, m_sp + extent);
    }

    /// @brief 退回最近一次分配；不保存每次分配的长度。
    /// @pre pointer 为当前栈段中最后一次尚未释放的分配地址。
    void deallocate(void* pointer) noexcept {
        TFL_ASSERT(pointer);
        auto* address = static_cast<std::byte*>(pointer);
        TFL_ASSERT(reinterpret_cast<std::uintptr_t>(address) >= reinterpret_cast<std::uintptr_t>(_begin()));
        TFL_ASSERT(reinterpret_cast<std::uintptr_t>(address) < reinterpret_cast<std::uintptr_t>(m_sp));
        TFL_ASSERT(reinterpret_cast<std::uintptr_t>(address) % alignment == 0);
        m_sp = address;
    }

    /// @brief 将独立空栈段压到当前活动栈段之后，返回新的活动栈顶。
    /// @pre 当前段属于 Coro；next 为空且不属于其他链。
    [[nodiscard]] Stacklet* push(Stacklet* next) noexcept {
        TFL_ASSERT(next && next != this && next->empty() && !next->m_prev && !next->m_next);
        next->m_prev = this;
        return next;
    }

    /// @brief 从活动栈段链摘下当前空栈顶，返回前一个活动栈段。
    /// @note 调用后当前栈段成为独立空段，可归还 Worker。
    [[nodiscard]] Stacklet* pop() noexcept {
        TFL_ASSERT(empty() && !m_next);
        return std::exchange(m_prev, nullptr);
    }

    /// @brief 当前栈段之前的活动栈段；根栈段返回 nullptr。
    [[nodiscard]] Stacklet* previous() const noexcept { return m_prev; }

    /// @brief 当前整个栈段的大小（包含头部）。
    [[nodiscard]] std::size_t size() const noexcept {
        return static_cast<std::size_t>(m_hi - reinterpret_cast<const std::byte*>(this));
    }

    /// @brief 栈段的用户存储容量。
    [[nodiscard]] std::size_t capacity() const noexcept {
        return static_cast<std::size_t>(m_hi - _begin());
    }

    /// @brief 栈段的剩余可分配空间。
    [[nodiscard]] std::size_t available() const noexcept {
        return static_cast<std::size_t>(m_hi - m_sp);
    }

    /// @brief 当前栈段是否不包含活动分配。
    [[nodiscard]] bool empty() const noexcept { return m_sp == _begin(); }

private:
    Stacklet() = default;

    /// @brief 创建默认大小的栈段，总大小为 4096 字节；仅供 Stack 调用。
    [[nodiscard]] static Stacklet* create() {
        return create(default_size - sizeof(Stacklet));
    }

    /// @brief 创建满足 capacity 字节用户存储的栈段；总大小为不小于 4096 的 2 的幂。
    /// @throws std::bad_alloc 请求过大或系统分配失败。
    [[nodiscard]] static Stacklet* create(std::size_t capacity) {
        constexpr std::size_t max_power = std::bit_floor(std::numeric_limits<std::size_t>::max());
        constexpr std::size_t mask = alignment - 1;
        if (capacity > max_power - sizeof(Stacklet) - mask) [[unlikely]] {
            TFL_THROW(std::bad_alloc{});
        }

        const std::size_t extent = (capacity + mask) & ~mask;
        const std::size_t required = std::max(default_size, sizeof(Stacklet) + extent);
        const std::size_t bytes = std::bit_ceil(required);
        void* memory = std::malloc(bytes);
        if (!memory) [[unlikely]] {
            TFL_THROW(std::bad_alloc{});
        }

        TFL_ASSERT(reinterpret_cast<std::uintptr_t>(memory) % alignment == 0);
        auto* stacklet = ::new (memory) Stacklet;
        stacklet->m_sp = static_cast<std::byte*>(memory) + sizeof(Stacklet);
        stacklet->m_hi = static_cast<std::byte*>(memory) + bytes;
        stacklet->m_prev = nullptr;
        stacklet->m_next = nullptr;
        return stacklet;
    }

    /// @brief 销毁独立且已经退空的栈段。
    static void destroy(Stacklet* stacklet) noexcept {
        TFL_ASSERT(!stacklet || (stacklet->empty() && !stacklet->m_prev && !stacklet->m_next));
        if (stacklet) {
            stacklet->~Stacklet();
            std::free(stacklet);
        }
    }

    [[nodiscard]] const std::byte* _begin() const noexcept {
        return reinterpret_cast<const std::byte*>(this) + sizeof(Stacklet);
    }

    std::byte* m_sp;   ///< 当前栈指针，分配向后移动，回收向前回退。
    std::byte* m_hi;   ///< 栈空间终点后一位。
    Stacklet* m_prev; ///< Coro 活动链中的前一栈段。
    Stacklet* m_next; ///< Worker 空闲缓存中的下一栈段。
};

static_assert(sizeof(Stacklet) % Stacklet::alignment == 0);
static_assert(Stacklet::alignment <= alignof(std::max_align_t));
static_assert(Stacklet::default_size > sizeof(Stacklet));
static_assert(std::is_trivially_destructible_v<Stacklet>);


/// @brief Worker 本地的空闲栈段缓存。
///
/// 默认构造时创建一个 4096 字节的空栈段。
/// acquire(size) 优先复用默认大小的空栈段，不够时创建总大小为 2 的幂的新栈段。
/// try_acquire() 仅从缓存取得空栈段，缓存为空时返回 nullptr。
/// release() 归还已退空的栈段；非默认大小栈段直接释放，避免大块长期驻留。
///
/// @warning 不能缓存仍然包含活动 Coroutine Frame 的栈段。
/// @warning 仅供所属 Worker 线程访问，不支持并发调用。
class Stack final : public Immovable<Stack> {
public:
    /// @brief 创建 Worker 缓存，并预先分配一个默认栈段。
    Stack() : m_free{Stacklet::create()} {}

    /// @brief 释放缓存中的所有空闲栈段。
    ~Stack() noexcept {
        while (Stacklet* stacklet = try_acquire()) {
            Stacklet::destroy(stacklet);
        }
    }

    /// @brief 归还独立、已经退空的栈段。
    /// @note 默认大小的栈段加入缓存；大栈段立即销毁。
    void release(Stacklet* stacklet) noexcept {
        TFL_ASSERT(stacklet && stacklet->empty() && !stacklet->m_prev && !stacklet->m_next);
        if (stacklet->size() != Stacklet::default_size) [[unlikely]] {
            Stacklet::destroy(stacklet);
            return;
        }
        stacklet->m_next = std::exchange(m_free, stacklet);
    }

    /// @brief 从缓存中弹出一个独立空栈段，不创建内存。
    /// @return 缓存为空时返回 nullptr。
    [[nodiscard]] Stacklet* try_acquire() noexcept {
        Stacklet* stacklet = m_free;
        if (stacklet) {
            m_free = std::exchange(stacklet->m_next, nullptr);
        }
        return stacklet;
    }

    /// @brief 取得可容纳 size 字节分配请求的独立空栈段。
    /// @note 小请求优先取缓存；缓存为空或请求超过默认容量时新建。
    /// @pre size > 0。
    [[nodiscard]] Stacklet* acquire(std::size_t size) {
        TFL_ASSERT(size > 0);
        if (size <= Stacklet::default_size - sizeof(Stacklet)) [[likely]] {
            if (Stacklet* stacklet = try_acquire()) {
                return stacklet;
            }
        }
        return Stacklet::create(size);
    }

private:
    Stacklet* m_free{nullptr}; ///< Worker 的空闲栈段单链表表头。
};

} // namespace tfl
