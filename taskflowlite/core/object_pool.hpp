/// @file  object_pool.hpp
/// @brief 类型级静态对象池门面 —— 与 operator new/delete 对接的统一入口。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once
#include <cstddef>
#include <new>
#include "macros.hpp"
#include "segmented_pool.hpp"
#include "size_class.hpp"

namespace tfl {

/// @brief 类型级静态对象池（Meyers 单例），与 operator new/delete 对接的统一入口。
///
/// @tparam T              池服务的对象类型
/// @tparam Scheme         size-class 方案，默认 SubdividedScheme 覆盖 [sizeof(T), 8192]
/// @tparam MaxCachedBytes 全池缓存字节上限，默认 64 MB。超出的 deallocate 直接交还系统
///
/// @note 定义 TFL_ENABLE_OBJECT_POOL 走池化路径，否则直接 ::operator new/delete（零成本）。
template <typename T,
         SizeClassScheme Scheme = SubdividedScheme<sizeof(T), 8192, 4>,
         std::size_t MaxCachedBytes = (1u << 26)>   // 64 MB
class ObjectPool {
public:
    using value_type  = T;
    using scheme_type = Scheme;
    using pool_type   = SegmentedPool<scheme_type, MaxCachedBytes>;

    /// @brief 全池缓存字节上限 (编译期常量)
    static constexpr std::size_t max_cached_bytes = MaxCachedBytes;

    static_assert(SizeClassScheme<scheme_type>, "invalid size-class scheme");

    /// @brief 分配 bytes 字节内存。
    /// @param bytes 请求的字节数 (通常是 sizeof(T))。
    /// @return 指向新分配内存的指针。
    /// @note 启用 TFL_ENABLE_OBJECT_POOL 时走 pool; 否则走全局 new (零开销)。
    [[nodiscard]] static TFL_FORCE_INLINE void* allocate(std::size_t bytes) {
        return pool().allocate(bytes);
    }

    /// @brief 归还 p 到池 (bytes 必须与 allocate 时一致, 一般为 sizeof(T))。
    /// @param p     归还的指针 (允许 nullptr, 此时为无操作)。
    /// @param bytes 原始分配时的字节数。
    /// @note 未启用 TFL_ENABLE_OBJECT_POOL 时走 sized delete, bytes 仍传给系统
    ///       分配器以加速反查 (jemalloc/tcmalloc 提速 30-50%)。
    static TFL_FORCE_INLINE void deallocate(void* p, std::size_t bytes) noexcept {
        pool().deallocate(p, bytes);
    }

    /// @brief 获取当前静态池单例 (Meyers 单例)。
    ///
    /// @details 首次访问时构造, atexit 阶段自动析构。析构时调用 release(),
    /// 归还所有 freelist 缓存给 ::operator delete。
    ///
    /// @warning 静态析构顺序:
    /// 池析构在 atexit 阶段进行。若有其他静态对象在析构时调用
    /// ObjectPool<T>::allocate/deallocate (析构里写日志、清理全局缓存等),
    /// 可能踩到"池已析构"问题。
    ///
    /// 缓解:
    ///   1. 调用方保证 alloc/dealloc 在 main 返回前完成
    ///   2. main 返回前主动调 release() 清空缓存 (release 后池仍可用,
    ///      只是 miss 率短暂上升)
    [[nodiscard]] static pool_type& pool() noexcept {
        static pool_type inst;
        return inst;
    }

    /// @brief 查询当前缓存总字节数 (近似, relaxed 读取)。
    /// @return 当前池中缓存的字节总数。
    [[nodiscard]] static std::size_t cached_bytes() noexcept {
        return pool().cached_bytes();
    }

    /// @brief 手动释放池中所有缓存 chunk, 归还 ::operator delete。
    ///
    /// @details
    /// 池本身不销毁, 后续仍可 allocate/deallocate (miss 率短暂上升, 直到
    /// 缓存重新填满)。
    ///
    /// 使用场景:
    ///   - 应用进入低活动期, 主动释放缓存
    ///   - 单元测试隔离: 每个用例前清空池
    ///   - 长跑服务定期回收: 防止缓存累积
    ///
    /// 一般无需调用 —— atexit 阶段池析构会自动 release。
    ///
    /// @warning 调用方必须保证此时无并发 allocate/deallocate, 且无对象仍引用池块。
    static void release() noexcept {
        pool().release();
    }
};

}  // namespace tfl
