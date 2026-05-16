// /// @file work_pool.hpp
// /// @brief Work* 节点统一回收池 —— unordered_dense::map 索引、mutex 串行化
// /// @author wicyn
// /// @contact https://github.com/wicyn
// /// @date 2026-05-10
// /// @license MIT
// /// @copyright Copyright (c) 2026 wicyn
// ///
// /// @details
// /// `WorkPool<DefaultAlign>` 是按对齐粒度回收 chunk 的池，内部用稠密哈希
// /// 表映射 aligned_size → freelist —— **只为见过的 size 创建 slot**，不预
// /// 承诺最大 size 上界、不为未出现的 size 占用空间。
// ///
// /// ============================================================================
// ///  设计取舍：选 unordered_dense::map 而非 vector / std::unordered_map
// /// ============================================================================
// ///   - 比 vector 索引版省内存：vector 索引版即便只见过 80B 也会 resize 出
// ///     idx=0..4 共 5 个 slot；hash 表只为 80B 一个 size 建一个 entry
// ///   - 比 std::unordered_map 快：稠密存储 + 紧凑节点布局，cache 友好；
// ///     entry 数 5-15 时查表 ~10 ns（std::unordered_map 约 30 ns）
// ///   - 同样无 size 上界、按需创建 slot
// ///
// /// 代价：每次访问要 hash + bucket lookup（~10 ns），相对 vector 索引的
// /// O(1) 数组访问慢一些；但相对 ::operator new (~50 ns) 仍然快得多。
// ///
// /// ============================================================================
// ///  为什么 V 是 unique_ptr<FreeStack> 而非 FreeStack 本身
// /// ============================================================================
// /// `unordered_dense::map` 的稠密存储要求 V 可移动（rehash 时整段数组重建，
// /// 元素必须能搬迁）。FreeStack 继承 `Immovable<FreeStack>` —— 既不可拷贝
// /// 也不可移动。直接放编译失败：
// ///
// ///   error: 尝试引用已删除的函数
// ///   note: "std::pair<Key,T>::pair(const std::pair<Key,T> &)"
// ///
// /// 套一层 unique_ptr：rehash 时移动的是指针（cheap），FreeStack 实例在
// /// 堆上原地不动，Immovable 契约保留。每个 slot 多一次堆分配、多一次
// /// 指针解引用，但相比 hash 查表本身的开销可忽略。
// ///
// /// 这是 C++ 标准库稠密容器存放 Immovable 类型的标准 trick，同样适用于
// /// boost::container::flat_map / absl::flat_hash_map / tsl::robin_map。
// ///
// /// ============================================================================
// ///  并发模型：单 mutex 串行化全部
// /// ============================================================================
// /// FreeStack 非线程安全；hash 表自身非线程安全。一把 mutex 覆盖：
// ///   - hash 表 lookup / insert / rehash
// ///   - unique_ptr 的 make_unique / 解引用
// ///   - FreeStack 的 push / pop
// /// `::operator new` / `::operator delete` 自身线程安全，放锁外减少持锁时间。
// ///
// /// ============================================================================
// ///  Align 与 over-aligned
// /// ============================================================================
// /// 模板参数 `DefaultAlign` 是池接管的对齐粒度，必须是 2 的幂、>= sizeof(void*)。
// /// 推荐 16（与 __STDCPP_DEFAULT_NEW_ALIGNMENT__ 一致）。
// ///
// /// over-aligned 对象（alignas(32) 及以上）由 Work::operator new 的 align_val_t
// /// 重载直接 fallback 到 ::operator new(bytes, align_val_t)，**不进池**。
// /// 池内部不感知运行时 align —— 简化设计、避开 (size, align) 二元 key。
// ///
// /// ============================================================================
// ///  默认关闭
// /// ============================================================================
// /// 默认 `Work::operator new` 直走 `::operator new`。启用方式：编译时
// /// 定义 `TFL_ENABLE_WORK_POOL`。
// ///
// /// @see free_stack.hpp   底层（非线程安全）单链栈
// /// @see work.hpp         Work::operator new/delete 接入位置

// #pragma once

// #include <bit>
// #include <cstddef>
// #include <memory>
// #include <mutex>
// #include <new>

// #include "free_stack.hpp"
// #include "macros.hpp"
// #include "unordered_dense.hpp"
// #include "utility.hpp"

// namespace tfl {

// // ============================================================================
// //  WorkPool<DefaultAlign>
// // ============================================================================

// /// @brief 按对齐粒度分桶的 chunk 回收池，slot 按需创建。
// ///
// /// @tparam DefaultAlign  池接管的对齐粒度（字节），必须是 2 的幂、>= sizeof(void*)。
// ///                       推荐 16（与 __STDCPP_DEFAULT_NEW_ALIGNMENT__ 一致）。
// template <std::size_t DefaultAlign = 16>
// class WorkPool : public Immovable<WorkPool<DefaultAlign>> {
//     static_assert(DefaultAlign >= sizeof(void*),
//                   "DefaultAlign must hold a freelist link pointer");
//     static_assert(std::has_single_bit(DefaultAlign),
//                   "DefaultAlign must be a power of two");

// public:
//     WorkPool() = default;

//     /// @brief 析构 —— 排空所有 freelist，归还内存给 ::operator delete。
//     ///
//     /// 假设此时无并发访问 —— 全局单例的析构在 atexit 触发，要求所有
//     /// 线程已 join；局部对象的析构由用户保证作用域。
//     ~WorkPool() noexcept {
//         std::lock_guard lk{m_mu};
//         for (auto& [aligned_size, slot] : m_pools) {
//             if (!slot) continue;
//             while (void* p = slot->pop()) {
//                 ::operator delete(p);
//             }
//         }
//     }

//     /// @brief 分配一块至少 bytes 字节的 chunk。
//     ///
//     /// 流程：
//     ///   1. bytes == 0 → fallback ::operator new(0)
//     ///   2. bytes 向上对齐到 DefaultAlign → aligned
//     ///   3. 锁内：hash 表 lookup，找到 / 创建对应 freelist；尝试 pop
//     ///   4. 命中 → 返回；miss → ::operator new(aligned)
//     ///
//     /// 关键：miss 路径必须按 aligned 而非 bytes 分配，否则下次同 key 但
//     /// 更大请求 pop 出来会越界写。这是按桶分配的强制契约。
//     [[nodiscard]] void* alloc(std::size_t bytes) {
//         if (bytes == 0) [[unlikely]] {
//             return ::operator new(bytes);
//         }

//         const auto aligned = align_up(bytes);

//         // 临界区：hash 表 lookup + freelist pop
//         {
//             std::lock_guard lk{m_mu};
//             auto& slot = m_pools[aligned];
//             if (!slot) slot = std::make_unique<detail::FreeStack>();
//             if (void* p = slot->pop()) return p;
//         }
//         // miss：按对齐后桶大小分配，确保下次同桶 pop 不越界
//         return ::operator new(aligned);
//     }

//     /// @brief 释放 chunk —— 对象必须已析构。
//     ///
//     /// 多态 sized deleting destructor 自动把 sizeof(Derived) 喂给 bytes。
//     void dealloc(void* p, std::size_t bytes) noexcept {
//         if (!p) return;
//         if (bytes == 0) [[unlikely]] {
//             ::operator delete(p);
//             return;
//         }

//         const auto aligned = align_up(bytes);

//         std::lock_guard lk{m_mu};
//         auto& slot = m_pools[aligned];
//         if (!slot) slot = std::make_unique<detail::FreeStack>();
//         slot->push(p);
//     }

//     // ------------------------------------------------------------------------
//     //  内省接口
//     // ------------------------------------------------------------------------

//     /// @brief 当前 hash 表中已创建的 slot 数 —— 调试 / dump 用。
//     /// 仅在锁外快照读，可能立即过时。
//     [[nodiscard]] std::size_t num_active_slots() const noexcept {
//         std::lock_guard lk{m_mu};
//         return m_pools.size();
//     }

// private:
//     /// @brief 把 bytes 向上对齐到 DefaultAlign 的整数倍。
//     /// DefaultAlign 是 2 幂时编译器折叠为 `(bytes + Align - 1) & ~(Align - 1)`，
//     /// 即一次 add + and。
//     [[nodiscard]] TFL_FORCE_INLINE static constexpr
//         std::size_t align_up(std::size_t bytes) noexcept {
//         return (bytes + DefaultAlign - 1) & ~(DefaultAlign - 1);
//     }

//     /// @brief aligned_size → freelist 的稠密 hash 表。
//     ///
//     /// 单链 separate chaining 实现，节点指针 rehash 时不失效；FreeStack 在
//     /// 节点内通过 unique_ptr 间接持有，rehash 移动 unique_ptr（cheap）而
//     /// FreeStack 堆上原地不动，兼顾稠密容器性能与 Immovable 契约。
//     ///
//     /// 不预 reserve 容量 —— 实际见到的 size 通常 5-15 个，让 hash 表自然
//     /// 增长即可，不必为预估付内存。
//     unordered_dense::map
//         <std::size_t,
//         std::unique_ptr<detail::FreeStack>
//             > m_pools;

//     /// @brief 串行化所有 alloc/dealloc。
//     /// `mutable` 让 num_active_slots 这类 const 成员函数也能加锁。
//     mutable std::mutex m_mu;
// };

// // ============================================================================
// //  默认池类型与单例
// // ============================================================================

// /// @brief 默认池 —— 16 字节对齐，与 __STDCPP_DEFAULT_NEW_ALIGNMENT__ 一致。
// using DefaultWorkPool = WorkPool<16>;
// #define TFL_ENABLE_WORK_POOL
// #if defined(TFL_ENABLE_WORK_POOL)

// /// @brief 进程级默认池单例。Meyers 单例，首次访问构造。
// inline DefaultWorkPool& work_pool() noexcept {
//     static DefaultWorkPool inst;
//     return inst;
// }

// #endif

// // ============================================================================
// //  Work 接入辅助
// // ============================================================================

// namespace detail {

// /// @brief Work::operator new 的统一入口（默认对齐走池）。
// [[nodiscard]] TFL_FORCE_INLINE inline void* pool_alloc(std::size_t bytes) {
// #if defined(TFL_ENABLE_WORK_POOL)
//     return work_pool().alloc(bytes);
// #else
//     return ::operator new(bytes);
// #endif
// }

// /// @brief Work::operator delete 的统一入口（默认对齐走池）。
// TFL_FORCE_INLINE inline void pool_dealloc(void* p, std::size_t bytes) noexcept {
// #if defined(TFL_ENABLE_WORK_POOL)
//     work_pool().dealloc(p, bytes);
// #else
//     (void)bytes;
//     ::operator delete(p);
// #endif
// }

// }  // namespace detail

// }  // namespace tfl






















// // /// @file work_pool.hpp
// // /// @brief Work* 节点统一回收池 —— vector 索引、按需扩容、mutex 串行化
// // /// @author wicyn
// // /// @contact https://github.com/wicyn
// // /// @date 2026-05-10
// // /// @license MIT
// // /// @copyright Copyright (c) 2026 wicyn
// // ///
// // /// @details
// // /// `WorkPool<DefaultAlign>` 是按对齐粒度回收 chunk 的池，内部用 vector
// // /// 索引 freelist —— **下标 = aligned_size / DefaultAlign - 1**，slot 按需
// // /// 创建，不预承诺最大 size 上界。
// // ///
// // /// ============================================================================
// // ///  设计取舍
// // /// ============================================================================
// // ///   - 下标访问 O(1)：比 hash 表快一个数量级
// // ///   - 按需扩容：启动内存接近 0，只为见过的 size 范围付内存
// // ///   - 无 size 上界：sizeof = 5000 的 Invoker 也能正常进池
// // ///   - 单一对齐维度：池统一按 DefaultAlign 对齐，over-aligned 由 Work 接入
// // ///     点直接 fallback 到 ::operator new，池内部不感知运行时 align
// // ///
// // /// ============================================================================
// // ///  为什么 over-aligned 不进池
// // /// ============================================================================
// // /// Work 家族绝大部分对齐 ≤ 16，over-aligned (alignas(32)/64/...) 极少。
// // /// 若把 align 也作为池的一个维度，结构复杂度大幅上升（freelist 要按
// // /// (size, align) 二元组分桶），换来的收益对小众情况几乎不可见。
// // ///
// // /// 取舍：池只服务默认对齐对象。over-aligned 类型由 Work::operator new
// // /// 的 align_val_t 重载直接 fallback 到 ::operator new(bytes, align_val_t)，
// // /// 不享受池的复用，但功能正确。
// // ///
// // /// 用户若真有大量 alignas(64) 的 Invoker，可单独给该类型绑 ObjectFreelist，
// // /// 不影响主池设计简洁。
// // ///
// // /// ============================================================================
// // ///  为什么用 unique_ptr<FreeStack> 而非直接放 FreeStack
// // /// ============================================================================
// // /// vector 扩容触发 reallocation，所有元素**移动**到新地址。FreeStack 继承
// // /// `Immovable<FreeStack>` —— 既不可拷贝也不可移动。直接放 vector 编译失败。
// // ///
// // /// 套一层 unique_ptr：vector 移动的是指针（cheap memcpy），FreeStack 实例
// // /// 在堆上原地不动，Immovable 契约保留。每个空槽位代价仅 8 字节 nullptr。
// // ///
// // /// ============================================================================
// // ///  并发模型：单 mutex 串行化全部
// // /// ============================================================================
// // /// FreeStack 非线程安全；vector 扩容也非线程安全。一把 mutex 覆盖：
// // ///   - vector 的 resize / 元素访问
// // ///   - unique_ptr 的 make_unique / 解引用
// // ///   - FreeStack 的 push / pop
// // /// `::operator new` / `::operator delete` 自身线程安全，放锁外减少持锁时间。
// // ///
// // /// ============================================================================
// // ///  默认关闭
// // /// ============================================================================
// // /// 默认 `Work::operator new` 直走 `::operator new`。启用方式：编译时
// // /// 定义 `TFL_ENABLE_WORK_POOL`。
// // ///
// // /// @see free_stack.hpp   底层（非线程安全）单链栈
// // /// @see work.hpp         Work::operator new/delete 接入位置

// // #pragma once

// // #include <bit>
// // #include <cstddef>
// // #include <memory>
// // #include <mutex>
// // #include <new>
// // #include <vector>

// // #include "free_stack.hpp"
// // #include "macros.hpp"
// // #include "utility.hpp"

// // namespace tfl {

// // // ============================================================================
// // //  WorkPool<DefaultAlign>
// // // ============================================================================

// // /// @brief 按对齐粒度分桶的 chunk 回收池，slot 按需扩容。
// // ///
// // /// @tparam DefaultAlign  池接管的对齐粒度（字节），必须是 2 的幂、>= sizeof(void*)。
// // ///                       推荐 16（与 __STDCPP_DEFAULT_NEW_ALIGNMENT__ 一致）。
// // template <std::size_t DefaultAlign = 16>
// // class WorkPool : public Immovable<WorkPool<DefaultAlign>> {
// //     static_assert(DefaultAlign >= sizeof(void*),
// //                   "DefaultAlign must hold a freelist link pointer");
// //     static_assert(std::has_single_bit(DefaultAlign),
// //                   "DefaultAlign must be a power of two");

// // public:
// //     WorkPool() = default;

// //     /// @brief 析构 —— 排空所有 freelist，归还内存给 ::operator delete。
// //     ///
// //     /// 假设此时无并发访问 —— 全局单例的析构在 atexit 触发，要求所有
// //     /// 线程已 join；局部对象的析构由用户保证作用域。
// //     ~WorkPool() noexcept {
// //         std::lock_guard lk{m_mu};
// //         for (auto& slot : m_slots) {
// //             if (!slot) continue;
// //             while (void* p = slot->pop()) {
// //                 ::operator delete(p);
// //             }
// //         }
// //     }

// //     /// @brief 分配一块至少 bytes 字节的 chunk。
// //     ///
// //     /// 流程：
// //     ///   1. bytes == 0  → fallback ::operator new(0)
// //     ///   2. 范围内 → 按 idx 进 vector，命中即返回（热路径）
// //     ///   3. miss → ::operator new(aligned)
// //     ///
// //     /// 关键：miss 路径必须按 aligned 而非 bytes 分配，否则下次同 idx 但
// //     /// 更大请求 pop 出来会越界写。这是按桶分配的强制契约。
// //     [[nodiscard]] void* alloc(std::size_t bytes) {
// //         if (bytes == 0) [[unlikely]] {
// //             return ::operator new(bytes);
// //         }

// //         const auto aligned = align_up(bytes);
// //         const auto idx     = aligned_to_index(aligned);

// //         // 临界区：vector 访问 + freelist pop
// //         {
// //             std::lock_guard lk{m_mu};
// //             ensure_slot(idx);
// //             if (void* p = m_slots[idx]->pop()) return p;
// //         }
// //         // miss：按桶上限分配
// //         return ::operator new(aligned);
// //     }

// //     /// @brief 释放 chunk —— 对象必须已析构。
// //     ///
// //     /// 多态 sized deleting destructor 自动把 sizeof(Derived) 喂给 bytes。
// //     void dealloc(void* p, std::size_t bytes) noexcept {
// //         if (!p) return;
// //         if (bytes == 0) [[unlikely]] {
// //             ::operator delete(p);
// //             return;
// //         }

// //         const auto aligned = align_up(bytes);
// //         const auto idx     = aligned_to_index(aligned);

// //         std::lock_guard lk{m_mu};
// //         ensure_slot(idx);
// //         m_slots[idx]->push(p);
// //     }

// //     // ------------------------------------------------------------------------
// //     //  内省接口
// //     // ------------------------------------------------------------------------

// //     /// @brief 当前 vector 的 slot 数（含空 slot）—— 调试 / dump 用。
// //     [[nodiscard]] std::size_t num_slots() const noexcept {
// //         std::lock_guard lk{m_mu};
// //         return m_slots.size();
// //     }

// //     /// @brief 当前已实际创建（非 nullptr）的 slot 数 —— 反映真实 size 多样性。
// //     [[nodiscard]] std::size_t num_active_slots() const noexcept {
// //         std::lock_guard lk{m_mu};
// //         std::size_t n = 0;
// //         for (const auto& s : m_slots) if (s) ++n;
// //         return n;
// //     }

// // private:
// //     /// @brief 把 bytes 向上对齐到 DefaultAlign 的整数倍。
// //     /// DefaultAlign 是 2 幂时编译为 `(bytes + Align - 1) & ~(Align - 1)`。
// //     [[nodiscard]] TFL_FORCE_INLINE static constexpr
// //         std::size_t align_up(std::size_t bytes) noexcept {
// //         return (bytes + DefaultAlign - 1) & ~(DefaultAlign - 1);
// //     }

// //     /// @brief 对齐后字节数 → vector 下标。
// //     /// idx=0 对应 DefaultAlign 字节桶（最小），idx 随 size 单调递增。
// //     [[nodiscard]] TFL_FORCE_INLINE static constexpr
// //         std::size_t aligned_to_index(std::size_t aligned) noexcept {
// //         return (aligned / DefaultAlign) - 1;
// //     }

// //     /// @brief 确保 m_slots[idx] 可访问且 unique_ptr 非空。
// //     ///
// //     /// 调用方必须持有 m_mu。如果 vector 不够大就扩容；如果对应 slot 是
// //     /// nullptr 就 make_unique 创建 FreeStack。
// //     ///
// //     /// 扩容策略依赖 std::vector 的 amortized O(1) push/resize 行为 ——
// //     /// 通常 1.5× 或 2× 几何增长，摊到大量 alloc 上每次扩容代价可忽略。
// //     void ensure_slot(std::size_t idx) {
// //         if (idx >= m_slots.size()) {
// //             m_slots.resize(idx + 1);
// //         }
// //         if (!m_slots[idx]) {
// //             m_slots[idx] = std::make_unique<detail::FreeStack>();
// //         }
// //     }

// //     /// @brief slot 数组：m_slots[i] 持有大小 (i+1)*DefaultAlign 字节的 chunk。
// //     ///
// //     /// vector<unique_ptr<...>> 的设计点：
// //     ///   - vector 扩容时移动 unique_ptr（cheap），FreeStack 堆上不动
// //     ///   - 空槽位是 nullptr unique_ptr，每个 8 字节
// //     ///   - FreeStack 保留 Immovable 契约，地址稳定
// //     std::vector<std::unique_ptr<detail::FreeStack>> m_slots;

// //     /// @brief 串行化所有 alloc/dealloc。
// //     /// `mutable` 让 num_slots / num_active_slots 这类 const 方法能加锁。
// //     mutable std::mutex m_mu;
// // };

// // // ============================================================================
// // //  默认池类型与单例
// // // ============================================================================

// // /// @brief 默认池 —— 16 字节对齐，与 __STDCPP_DEFAULT_NEW_ALIGNMENT__ 一致。
// // using DefaultWorkPool = WorkPool<16>;
// // #define TFL_ENABLE_WORK_POOL
// // #if defined(TFL_ENABLE_WORK_POOL)

// // /// @brief 进程级默认池单例。Meyers 单例，首次访问构造。
// // inline DefaultWorkPool& work_pool() noexcept {
// //     static DefaultWorkPool inst;
// //     return inst;
// // }

// // #endif

// // // ============================================================================
// // //  Work 接入辅助
// // // ============================================================================

// // namespace detail {

// // /// @brief Work::operator new 的统一入口（默认对齐走池）。
// // [[nodiscard]] TFL_FORCE_INLINE inline void* pool_alloc(std::size_t bytes) {
// // #if defined(TFL_ENABLE_WORK_POOL)
// //     return work_pool().alloc(bytes);
// // #else
// //     return ::operator new(bytes);
// // #endif
// // }

// // /// @brief Work::operator delete 的统一入口（默认对齐走池）。
// // TFL_FORCE_INLINE inline void pool_dealloc(void* p, std::size_t bytes) noexcept {
// // #if defined(TFL_ENABLE_WORK_POOL)
// //     work_pool().dealloc(p, bytes);
// // #else
// //     (void)bytes;
// //     ::operator delete(p);
// // #endif
// // }

// // }  // namespace detail

// // }  // namespace tfl






















// // /// @file work_pool.hpp
// // /// @brief Work* 节点统一回收池 —— 动态 hash 表 + mutex 串行化
// // /// @author wicyn
// // /// @contact https://github.com/wicyn
// // /// @date 2026-05-10
// // /// @license MIT
// // /// @copyright Copyright (c) 2026 wicyn
// // ///
// // /// @details
// // /// `WorkPool<Align>` 是按对齐粒度回收 chunk 的池。内部用 hash 表把
// // /// "对齐后的 size" 映射到对应的 freelist —— **只为见过的 size 创建 slot**，
// // /// 不预分配 slot 数组、没有 size 上界。
// // ///
// // /// ============================================================================
// // ///  设计取舍：从 std::array 切换到 unordered_map
// // /// ============================================================================
// // /// 之前用 `std::array<FreeStack, num_slots>` 预分配固定数量 slot ——
// // /// 简单但浪费：实际 Invoker sizeof 只取 5-15 个不同值，256 个 slot 大部分
// // /// 永远空闲。换成 hash 表后：
// // ///
// // ///   - 按需创建 slot —— 只为真实出现过的 size 维护 freelist
// // ///   - 无 size 上界 —— 任意大的 Work 派生类都能进池
// // ///   - 内存随实际 size 多样性增长 —— 通常 < 1KB
// // ///
// // /// 代价：每次 alloc 多一次 hash lookup（~20 ns），但远低于 ::operator new。
// // ///
// // /// ============================================================================
// // ///  并发模型：单 mutex 串行化全部
// // /// ============================================================================
// // /// FreeStack 是非线程安全的单链表栈（见 free_stack.hpp）。所有访问通过
// // /// 一把 mutex 串行化，包括：
// // ///
// // ///   1. hash 表的 lookup / insert（unordered_map 自身非线程安全）
// // ///   2. FreeStack 的 push / pop
// // ///
// // /// 锁粒度选择：一把锁覆盖所有 size 是有意为之。alloc 不在 task 执行的
// // /// 热路径上，未竞争 mutex ~5 ns 的代价摊到每个 task 上完全可忽略。
// // /// 真测出锁竞争是瓶颈再考虑分段（如 striped lock）。
// // ///
// // /// ============================================================================
// // ///  Align 模板参数
// // /// ============================================================================
// // /// `WorkPool<Align>` 把请求 size 向上对齐到 Align 字节，同 Align 桶的
// // /// 不同 size 共享同一 freelist。Align 必须是 2 的幂、>= sizeof(void*)。
// // ///
// // /// 推荐 Align=16 ——
// // ///   1. 与 __STDCPP_DEFAULT_NEW_ALIGNMENT__ 一致，operator new 天然满足
// // ///   2. 大多数 Invoker 字段对齐 ≤ 16
// // ///   3. 同 size 合流概率高（很多 sizeof 都是 16 倍数）
// // ///   4. 内部碎片 ≤ 6.25%（[16k+1, 16k+16] → 16k+16 桶）
// // ///
// // /// ============================================================================
// // ///  默认关闭
// // /// ============================================================================
// // /// 默认 `Work::operator new` 直走 `::operator new`。启用方式：编译时
// // /// 定义 `TFL_ENABLE_WORK_POOL`。
// // ///
// // /// @see free_stack.hpp   底层（非线程安全）单链栈
// // /// @see work.hpp         Work::operator new/delete 接入位置

// // #pragma once

// // #include <bit>
// // #include <cstddef>
// // #include <mutex>
// // #include <new>
// // #include <unordered_map>

// // #include "free_stack.hpp"
// // #include "macros.hpp"
// // #include "utility.hpp"
// // #include "unordered_dense.hpp"
// // namespace tfl {

// // // ============================================================================
// // //  WorkPool<Align>
// // // ============================================================================

// // /// @brief 按对齐粒度分桶的 chunk 回收池，slot 按需创建。
// // ///
// // /// @tparam Align 对齐粒度（字节），必须是 2 的幂、>= sizeof(void*)。
// // template <std::size_t Align = 32>
// // class WorkPool : public Immovable<WorkPool<Align>> {
// //     static_assert(Align >= sizeof(void*),
// //                   "Align must hold a freelist link pointer");
// //     static_assert(std::has_single_bit(Align),
// //                   "Align must be a power of two");

// // public:
// //     WorkPool() = default;

// //     /// @brief 析构 —— 排空所有 freelist，归还内存给 ::operator delete。
// //     ///
// //     /// 假设此时无并发访问 —— 全局单例的析构在 atexit 触发，要求所有
// //     /// 线程已 join；局部对象的析构由用户保证作用域。
// //     ~WorkPool() noexcept {
// //         std::lock_guard lk{m_mu};
// //         for (auto& [size, stack] : m_pools) {
// //             while (void* p = stack.pop()) {
// //                 ::operator delete(p);
// //             }
// //         }
// //     }

// //     /// @brief 分配一块至少 bytes 字节的 chunk。
// //     ///
// //     /// 流程：
// //     ///   1. bytes == 0 → ::operator new(0) 直接返回（标准行为）
// //     ///   2. bytes 向上对齐到 Align 的整数倍 → aligned
// //     ///   3. 锁内：lookup hash 表，找到 / 创建对应 freelist；尝试 pop
// //     ///   4. 命中 → 返回；miss → ::operator new(aligned)
// //     ///
// //     /// 关键：miss 路径必须按 aligned 而非 bytes 分配，否则下次同桶但
// //     /// 更大的请求 pop 出来会越界写。这是按桶分配的强制契约。
// //     [[nodiscard]] void* alloc(std::size_t bytes) {
// //         const auto aligned = align_up(bytes);

// //         // 临界区只覆盖 hash 表 lookup + freelist pop —— 尽量短
// //         {
// //             std::lock_guard lk{m_mu};
// //             // operator[] 在缺失时默认构造 FreeStack；FreeStack 默认构造合法
// //             if (void* p = m_pools[aligned].pop()) {
// //                 return p;
// //             }
// //         }
// //         // miss：按对齐后的桶大小分配，保证下次同桶 pop 出来不越界
// //         return ::operator new(aligned);
// //     }

// //     /// @brief 释放 chunk —— 对象必须已析构。
// //     ///
// //     /// 多态 sized deleting destructor 自动把 sizeof(Derived) 喂给 bytes。
// //     /// 释放时按对齐粒度路由，与 alloc 配对。
// //     void dealloc(void* p, std::size_t bytes) noexcept {
// //         if (!p) return;
// //         const auto aligned = align_up(bytes);

// //         std::lock_guard lk{m_mu};
// //         m_pools[aligned].push(p);
// //     }

// //     /// @brief 当前 hash 表中已创建的 slot 数 —— 调试 / dump 用。
// //     /// 仅在锁外快照读，可能立即过时。
// //     [[nodiscard]] std::size_t num_active_slots() const noexcept {
// //         std::lock_guard lk{m_mu};
// //         return m_pools.size();
// //     }

// // private:
// //     /// @brief 把 bytes 向上对齐到 Align 的整数倍。
// //     /// Align 是 2 的幂时编译器折叠为 `(bytes + Align - 1) & ~(Align - 1)`，
// //     /// 即一次 add + and。
// //     [[nodiscard]] TFL_FORCE_INLINE static constexpr
// //         std::size_t align_up(std::size_t bytes) noexcept {
// //         return (bytes + Align - 1) & ~(Align - 1);
// //     }

// //     /// @brief size → freelist 的 hash 表。
// //     ///
// //     /// 单链 separate chaining 实现（libstdc++ / libc++ 都是），节点指针
// //     /// rehash 时不失效；FreeStack 在节点内 placement new 构造，不需要
// //     /// 拷贝/移动 —— 兼容 FreeStack 的 Immovable 约束。
// //     ///
// //     /// 不预 reserve 容量 —— 实际见到的 size 通常 5-15 个，让 hash 表
// //     /// 自然增长即可，不必为预估付内存。
// //     unordered_dense::map<std::size_t, detail::FreeStack> m_pools;

// //     /// @brief 串行化所有 alloc/dealloc。
// //     /// `mutable` 让 num_active_slots 这类 const 成员函数也能加锁。
// //     mutable std::mutex m_mu;
// // };

// // // ============================================================================
// // //  默认池类型
// // // ============================================================================

// // /// @brief 默认池 —— 16 字节对齐。
// // using DefaultWorkPool = WorkPool<32>;
// // #define TFL_ENABLE_WORK_POOL
// // #if defined(TFL_ENABLE_WORK_POOL)

// // /// @brief 进程级默认池单例。Meyers 单例，首次访问构造。
// // inline DefaultWorkPool& work_pool() noexcept {
// //     static DefaultWorkPool inst;
// //     return inst;
// // }

// // #endif

// // // ============================================================================
// // //  Work 接入辅助
// // // ============================================================================

// // namespace detail {

// // /// @brief Work::operator new 的统一入口。
// // [[nodiscard]] TFL_FORCE_INLINE inline void* pool_alloc(std::size_t bytes) {
// // #if defined(TFL_ENABLE_WORK_POOL)
// //     return work_pool().alloc(bytes);
// // #else
// //     return ::operator new(bytes);
// // #endif
// // }

// // /// @brief Work::operator delete 的统一入口。
// // TFL_FORCE_INLINE inline void pool_dealloc(void* p, std::size_t bytes) noexcept {
// // #if defined(TFL_ENABLE_WORK_POOL)
// //     work_pool().dealloc(p, bytes);
// // #else
// //     (void)bytes;
// //     ::operator delete(p);
// // #endif
// // }

// // }  // namespace detail

// // }  // namespace tfl
