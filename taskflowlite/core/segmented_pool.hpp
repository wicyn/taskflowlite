/// @file segmented_pool.hpp
/// @brief 无锁分段对象池 —— 按 size class 分片，带全局字节预算硬上限。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <new>
#include "macros.hpp"
#include "utility.hpp"
#include "free_stack.hpp"
#include "size_class.hpp"

namespace tfl {

/// @brief 无锁分段对象池 —— 按 size class 分片的 lock-free freelist 集合，带全局字节预算硬上限。
///
/// @tparam Scheme         size-class 方案 (PowerOfTwo / Subdivided / Hybrid / Table)
/// @tparam MaxCachedBytes 全池缓存字节上限，默认 64 MB
///
/// 限流是硬上限: deallocate CAS 循环原子合并"判断容量"与"计数递增"，超 cap 直接交还系统。
/// FreeStack 内部 push/pop 用 release/acquire-CAS 保证数据可见性，m_cached_bytes 仅做限流计数用 relaxed。
template <SizeClassScheme Scheme, std::size_t MaxCachedBytes = (1u << 26)>  // 64 MB
class SegmentedPool : public Immovable<SegmentedPool<Scheme, MaxCachedBytes>> {
public:
    using scheme_type = Scheme;
    using traits_type = SizeClassTraits<scheme_type>;

    static constexpr std::size_t class_count      = traits_type::class_count();
    static constexpr std::size_t min_size         = traits_type::min_size();
    static constexpr std::size_t max_size         = traits_type::max_size();
    static constexpr std::size_t max_cached_bytes = MaxCachedBytes;

    static_assert(class_count > 0, "pool must contain at least one class");
    static_assert(min_size >= sizeof(void*), "smallest class must hold a freelist link");
    static_assert(min_size <= max_size, "invalid size-class range");
    static_assert(MaxCachedBytes >= max_size, "MaxCachedBytes must hold at least one largest block");

    SegmentedPool() = default;
    ~SegmentedPool() noexcept { release(); }

    /// @brief 分配 bytes 字节内存。
    ///
    /// 顺序: pop (acquire-CAS) -> fetch_sub (relaxed)。
    /// acquire 保证 pop 之后的操作不会重排到 pop 之前,
    /// 所以 fetch_sub 必然在 pop 成功后才执行,
    /// 不会出现"sub 先于 add"导致的下溢。
    [[nodiscard]] TFL_FORCE_INLINE void* allocate(std::size_t bytes) {
        // 单次无符号范围检查: bytes < min_size 时下溢成大数,必然 > range,合并两个分支
        if (bytes - min_size > max_size - min_size) [[unlikely]] {
            return ::operator new(bytes);
        }
        const auto idx  = traits_type::class_index(bytes);
        const auto size = traits_type::class_size(idx);
        if (void* p = m_classes[idx].pop()) [[likely]] {        // (1) 出池 (acquire-CAS)
            m_cached_bytes.fetch_sub(size, std::memory_order_relaxed);  // (2) 退位
            return p;
        }
        return ::operator new(size);   // 池空,新申请,无需动计数器
    }

    /// @brief 归还 p 到池 (bytes 字节)。
    ///
    /// 顺序: load -> 容量检查 -> fetch_add (relaxed) -> push (release-CAS)。
    ///   - load 用 relaxed 即可: 读到的值可能滞后,导致瞬时误判超 cap,
    ///     但 CAS 循环会把最终判断权交给原子比较,误判仅损失一次入池机会
    ///   - fetch_add 必须在 push 之前: release 屏障阻止 add 重排到 push 之后,
    ///     保证 cached_bytes 始终是上界 (>= 实际池中字节)
    ///   - CAS 循环: 将"判断容量"与"计数递增"合并为单一原子事务,
    ///     避免竞争窗口内多个线程同时通过检查导致的瞬时超限
    TFL_FORCE_INLINE void deallocate(void* p, std::size_t bytes) noexcept {
        if (!p) [[unlikely]] return;
        if (bytes - min_size > max_size - min_size) [[unlikely]] {
            ::operator delete(p, bytes);
            return;
        }
        const auto idx  = traits_type::class_index(bytes);
        const auto size = traits_type::class_size(idx);

        // CAS 循环: 原子地完成"读 -> 判断 -> 加",保证 cached_bytes 严格 <= cap
        // load + fetch_add 两步分离时,N 个线程可能同时通过检查再各自 add,
        // 导致瞬时超 cap N*size 字节。CAS 把判断和加合并为单一原子事务,
        // 失败的线程直接交还系统,不入池。
        auto curr = m_cached_bytes.load(std::memory_order_relaxed);
        do {
            if (curr + size > MaxCachedBytes) [[unlikely]] {
                ::operator delete(p, size);
                return;
            }
        } while (!m_cached_bytes.compare_exchange_weak(
            curr, curr + size,
            std::memory_order_relaxed,
            std::memory_order_relaxed));

        m_classes[idx].push(p);   // release-CAS,fetch_add 已先于此完成
    }

    /// @brief 排空所有 freelist,把缓存块全部交还系统。
    /// @warning 调用者必须保证: 无并发 allocate/deallocate; 且无存活对象仍引用池块。
    void release() noexcept {
        for (std::size_t i = 0; i < class_count; ++i) {
            const auto size = traits_type::class_size(i);
            while (void* p = m_classes[i].pop()) {
                ::operator delete(p, size);   // sized delete,帮系统分配器省一次反查
            }
        }
        // release 序: 若 release() 后有重启复用线程,确保它们看到 0 而非旧值
        m_cached_bytes.store(0, std::memory_order_release);
    }

    /// @brief 查询当前缓存总字节数 (近似,relaxed)。
    [[nodiscard]] std::size_t cached_bytes() const noexcept {
        return m_cached_bytes.load(std::memory_order_relaxed);
    }

private:
    /// @brief 限流计数 (relaxed),独占一对 cache line 防止与相邻 freelist 头伪共享。
    /// 每次 allocate/deallocate 都 RMW 这个变量,如果与 m_classes[0].m_head
    /// 共享 cache line, 会与小桶热点竞争同一行,造成 false sharing。
    alignas(2 * cache_line_size) std::atomic<std::size_t> m_cached_bytes{0};

    /// @brief 每个 size class 一个 lock-free freelist。
    /// FreeStack 自身 alignas(2 * cache_line_size),数组里相邻 freelist 各占独立 cache 行。
    std::array<FreeStack, class_count> m_classes;
};

}  // namespace tfl
