/// @file segmented_pool.hpp
/// @brief Lock-free segmented pool.
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-15
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn
///
/// @details
/// SegmentedPool<Scheme> 是一个多 class lock-free freelist 池。
///
/// 每个 freelist class 对应一个固定 size-class:
///
///   classes[0] -> 16B
///   classes[1] -> 32B
///   classes[2] -> 64B
///   ...
///
/// allocate(bytes):
///
///   bytes -> class_index -> freelist
///
/// 超出 size-class 范围的对象:
///
///   直接回退到全局 operator new/delete。
///
/// 特点:
///
///   - 无锁 freelist
///   - constexpr size-class
///   - 支持表驱动 / 算法驱动 scheme
///   - 编译期 traits 自动补全

#pragma once

#include <array>
#include <cstddef>
#include <new>

#include "macros.hpp"
#include "utility.hpp"
#include "free_stack.hpp"
#include "size_class.hpp"

namespace tfl {

template <SizeClassScheme Scheme>
class SegmentedPool : public Immovable<SegmentedPool<Scheme>> {
public:
    using scheme_type = Scheme;
    using traits_type = SizeClassTraits<scheme_type>;

    /// @brief class 总数。
    static constexpr std::size_t class_count = traits_type::class_count();

    /// @brief 池接管的最小字节数。
    static constexpr std::size_t min_size = traits_type::min_size();

    /// @brief 池接管的最大字节数。
    static constexpr std::size_t max_size = traits_type::max_size();

    static_assert(class_count > 0, "pool must contain at least one class");
    static_assert(min_size >= sizeof(void*), "smallest class must hold a freelist link");
    static_assert(min_size <= max_size, "invalid size-class range");

    SegmentedPool() = default;

    ~SegmentedPool() noexcept {
        release();
    }

    /// @brief 分配内存。
    ///
    /// 小对象:
    ///
    ///   bytes -> class -> freelist
    ///
    /// 大对象:
    ///
    ///   fallback 到全局 operator new
    [[nodiscard]] TFL_FORCE_INLINE void* allocate(std::size_t bytes) {
        // 单次无符号范围检查: bytes < min_size 时下溢成大数,必然 > range,合并两个分支
        if (bytes - min_size > max_size - min_size) [[unlikely]] {
            return ::operator new(bytes);
        }
        const auto idx = traits_type::class_index(bytes);
        if (void* p = m_classes[idx].pop()) [[likely]] {
            return p;
        }
        return ::operator new(traits_type::class_size(idx));
    }

    /// @brief 归还内存。
    ///
    /// 小对象:
    ///
    ///   push 到对应 freelist
    ///
    /// 大对象:
    ///
    ///   fallback 到全局 operator delete
    TFL_FORCE_INLINE void deallocate(void* p, std::size_t bytes) noexcept {
        if (!p) [[unlikely]] {
            return;
        }
        if (bytes - min_size > max_size - min_size) [[unlikely]] {
            ::operator delete(p);
            return;
        }
        m_classes[traits_type::class_index(bytes)].push(p);
    }

    /// @brief 释放所有 freelist 缓存块。
    ///
    /// @warning
    ///
    /// 调用者必须保证:
    ///
    ///   - 无并发 allocate/deallocate
    ///   - 无存活对象仍引用池块
    void release() noexcept {
        for (std::size_t i = 0; i < class_count; ++i) {
            const auto size = traits_type::class_size(i);
            while (void* p = m_classes[i].pop()) {
                ::operator delete(p, size);   // sized delete
            }
        }
    }

private:
    /// @brief 每个 class 一个 lock-free freelist。
    std::array<FreeStack, class_count> m_classes;
};

}  // namespace tfl
