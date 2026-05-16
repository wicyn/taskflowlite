/// @file object_pool.hpp
/// @brief Typed object pool facade.
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-15
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn
///
/// @details
/// ObjectPool<T, Scheme> 提供：
///
///   - 类型级静态对象池入口
///   - 与 operator new/delete 对接
///   - 默认 scheme 映射
///
/// 默认情况下：
///
///   ObjectPool<T>
///
/// 等价于：
///
///   ObjectPool<T, PoolSchemeForT<T>>
///
/// 用户也可以手动指定 scheme：
///
/// @code
/// using MyPool = ObjectPool<
///     MyObject,
///     PowerOfTwoScheme<64, 4096>
/// >;
/// @endcode

#pragma once

#include <cstddef>
#include <new>

#include "macros.hpp"
#include "segmented_pool.hpp"
#include "size_class.hpp"

namespace tfl {
#define TFL_ENABLE_WORK_POOL
/// @brief 类型级静态对象池。
template <typename T, SizeClassScheme Scheme = SubdividedScheme<sizeof(T), 4096, 4>>
class ObjectPool {
public:
    using value_type  = T;
    using scheme_type = Scheme;
    using pool_type   = SegmentedPool<scheme_type>;

    static_assert(SizeClassScheme<scheme_type>, "invalid size-class scheme");

    [[nodiscard]] static TFL_FORCE_INLINE void* allocate(std::size_t bytes) {
#if defined(TFL_ENABLE_WORK_POOL)
        return pool().allocate(bytes);
#else
        return ::operator new(bytes);
#endif
    }

    static TFL_FORCE_INLINE void deallocate(void* p, std::size_t bytes) noexcept {
#if defined(TFL_ENABLE_WORK_POOL)
        pool().deallocate(p, bytes);
#else
        (void)bytes;
        ::operator delete(p);
#endif
    }

#if defined(TFL_ENABLE_WORK_POOL)
    /// @brief 获取当前静态池单例。
    ///
    /// Meyers 单例,首次访问时构造,atexit 阶段自动析构。析构时会调用
    /// release(),归还所有 freelist 缓存给 ::operator delete。
    ///
    /// @warning 静态析构顺序问题
    ///
    /// 池单例的析构发生在 atexit 阶段。如果有其他静态对象在析构时调用
    /// ObjectPool<T>::allocate/deallocate (例如析构里写日志、销毁全局
    /// 缓存等),可能踩到"池已析构"问题。
    ///
    /// 缓解方式:
    ///   1. 调用方保证所有 alloc/dealloc 在 main 返回前完成
    ///   2. 使用 release() 在 main 返回前主动清空缓存,降低析构期访问
    ///      的风险 (实际上 inst 在 release 后仍能 alloc,因为 fallback
    ///      路径只依赖 ::operator new)
    [[nodiscard]] static pool_type& pool() noexcept {
        static pool_type inst;
        return inst;
    }

    /// @brief 手动释放池中缓存的所有 chunk,归还给 ::operator delete。
    ///
    /// 池本身不销毁,后续仍可 allocate/deallocate (miss 率会暂时上升,
    /// 直到缓存重新填满)。
    ///
    /// 使用场景:
    ///   - 应用进入低活动期,主动释放缓存
    ///   - 单元测试隔离:每个测试用例之前清空池
    ///   - 长跑服务定期回收:防止缓存累积占用内存
    ///
    /// 一般情况下**不需要调用**——atexit 阶段池析构会自动 release。
    ///
    /// @warning 调用方必须保证此时无并发 allocate/deallocate,
    ///          且无对象仍引用池块。
    static void release() noexcept {
        pool().release();
    }
#endif
};

}  // namespace tfl
