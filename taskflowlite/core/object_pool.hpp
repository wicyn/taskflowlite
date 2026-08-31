/// @file object_pool.hpp
/// @brief 分桶式并发固定类型对象池，使用 tagged 原子空闲栈与内部 Slab 批量存储。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-08-20
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

#include "macros.hpp"
#include "utility.hpp"

namespace tfl {

// ============================================================================
// TaggedHead128
// ============================================================================

/// @brief 128-bit tagged head：64-bit 指针与 64-bit ABA 版本计数器。
struct alignas(16) TaggedHead128 final {
    using pointer_type = void*;
    using tag_type = std::uintptr_t;

private:
    pointer_type m_pointer{nullptr};
    tag_type m_tag{0};

public:
    /// @brief 构造空 head。
    constexpr TaggedHead128() noexcept = default;

    /// @brief 使用指针和 ABA 版本计数器构造 head。
    constexpr TaggedHead128(pointer_type pointer, tag_type version) noexcept
        : m_pointer{pointer}, m_tag{version} {
    }

    /// @brief 返回保存的指针。
    [[nodiscard]] constexpr pointer_type pointer() const noexcept {
        return m_pointer;
    }

    /// @brief 返回 ABA 版本计数器。
    [[nodiscard]] constexpr tag_type tag() const noexcept {
        return m_tag;
    }

    /// @brief 判断指针是否能被当前 head 完整保存。
    [[nodiscard]] static constexpr bool can_encode(pointer_type) noexcept {
        return true;
    }

    /// @brief 返回下一个 ABA 版本计数器。
    [[nodiscard]] static constexpr tag_type next_tag(tag_type current) noexcept {
        return current + tag_type{1};
    }
};

static_assert(sizeof(void*) == 8, "TaggedHead128 requires a 64-bit target");
static_assert(sizeof(std::uintptr_t) == 8, "TaggedHead128 requires 64-bit uintptr_t");
static_assert(sizeof(TaggedHead128) == 16);
static_assert(alignof(TaggedHead128) == 16);
static_assert(std::is_trivially_copyable_v<TaggedHead128>);

// ============================================================================
// TaggedHead64
// ============================================================================

/// @brief 64-bit tagged head：低 PointerBits 保存指针，其余高位保存 ABA 版本计数器。
///
/// 默认 PointerBits == 48：
///
/// - 低 48-bit 保存指针；
/// - 高 16-bit 保存 ABA 版本计数器。
///
/// @tparam PointerBits 用于保存指针的低位数量，必须保留至少 16-bit tag。
/// @warning 只能编码高位全部为零的地址。
/// @warning 默认 16-bit tag 每 65536 次 head 修改后发生一次回绕。
template <int PointerBits = TF_POINTER_BITS>
    requires (PointerBits > 0) && (PointerBits <= 48)
struct TaggedHead64 final {
    using pointer_type = void*;
    using storage_type = std::uintptr_t;

    static constexpr int pointer_bits = PointerBits;
    static constexpr int tag_bits = 64 - PointerBits;

    using tag_type = std::conditional_t<(tag_bits <= 16), std::uint16_t,
                                        std::conditional_t<(tag_bits <= 32), std::uint32_t, std::uint64_t>>;

private:
    static constexpr storage_type pointer_mask = (storage_type{1} << pointer_bits) - storage_type{1};
    static constexpr storage_type tag_mask = ~storage_type{0} >> pointer_bits;

    storage_type m_bits{0};

public:
    /// @brief 构造空 head。
    constexpr TaggedHead64() noexcept = default;

    /// @brief 使用指针和 ABA 版本计数器构造 head。
    TaggedHead64(pointer_type pointer, tag_type version) noexcept
        : m_bits{(reinterpret_cast<storage_type>(pointer) & pointer_mask) |
                 ((static_cast<storage_type>(version) & tag_mask) << pointer_bits)} {
        TFL_ASSERT(can_encode(pointer));
    }

    /// @brief 返回保存的指针。
    [[nodiscard]] pointer_type pointer() const noexcept {
        return reinterpret_cast<pointer_type>(m_bits & pointer_mask);
    }

    /// @brief 返回 ABA 版本计数器。
    [[nodiscard]] constexpr tag_type tag() const noexcept {
        return static_cast<tag_type>((m_bits >> pointer_bits) & tag_mask);
    }

    /// @brief 判断指针是否能被低 PointerBits 完整编码。
    [[nodiscard]] static bool can_encode(pointer_type pointer) noexcept {
        const storage_type address = reinterpret_cast<storage_type>(pointer);
        return (address & ~pointer_mask) == 0;
    }

    /// @brief 返回下一个 ABA 版本计数器。
    [[nodiscard]] static constexpr tag_type next_tag(tag_type current) noexcept {
        return static_cast<tag_type>(
            (static_cast<storage_type>(current) + storage_type{1}) & tag_mask
            );
    }
};

static_assert(sizeof(void*) == 8, "TaggedHead64 requires a 64-bit target");
static_assert(sizeof(std::uintptr_t) == 8, "TaggedHead64 requires 64-bit uintptr_t");
static_assert(sizeof(TaggedHead64<>) == 8);
static_assert(alignof(TaggedHead64<>) == alignof(std::uintptr_t));
static_assert(std::is_trivially_copyable_v<TaggedHead64<>>);
static_assert(std::is_same_v<TaggedHead64<48>::tag_type, std::uint16_t>);

/// @brief ObjectPool 可使用的 tagged head 类型约束。
template <typename T>
concept tagged_head =
    std::is_trivially_copyable_v<T> &&
    (sizeof(T) == 8 || sizeof(T) == 16) &&
    requires(T head, void* pointer, typename T::tag_type tag) {
        typename T::pointer_type;
        requires std::same_as<typename T::pointer_type, void*>;
        { T{pointer, tag} };
        { head.pointer() } -> std::same_as<void*>;
        { head.tag() } -> std::same_as<typename T::tag_type>;
        { T::can_encode(pointer) } -> std::convertible_to<bool>;
        { T::next_tag(tag) } -> std::same_as<typename T::tag_type>;
    };


/// @brief ObjectPool 默认使用的 tagged head 类型。
///
/// 平台原生支持无锁 128-bit atomic 时优先使用 TaggedHead128，
/// 否则回退到 TaggedHead64<>，避免默认配置依赖锁实现的 128-bit atomic。
using DefaultTaggedHead = std::conditional_t<
    std::atomic<TaggedHead128>::is_always_lock_free,
    TaggedHead128,
    TaggedHead64<>
    >;

// ============================================================================
// ObjectPool
// ============================================================================

/// @brief 分桶式两层并发固定类型对象池。
///
/// ObjectPool 由 BucketCount 个独立 Bucket 组成。每个 Bucket 包含：
///
/// - tagged 原子空闲栈，负责 destroy 后 ObjectBlock 的高频复用；
/// - 内部 Slab 链，按 BlocksPerSlab 个 ObjectBlock 一次批量增长；
/// - refill_mutex，只用于空闲栈完全耗尽后的冷路径补充。
///
/// 构造 ObjectPool 时，每个 Bucket 立即创建首个 Slab，并将全部 ObjectBlock
/// 连接后直接发布到空闲栈。因此第一次 create() 即可进入原子 pop 热路径。
///
/// 空闲栈耗尽后，每次 refill 只进行一次 Slab 分配。当前线程直接取得首个
/// ObjectBlock，其余节点已经在新 Slab 内连接完成，再通过一次 CAS 整链发布。
///
/// destroy() 不重新选择 Bucket。每个 ObjectBlock 永久保存所属 FreeStack
/// 地址，因此对象可以由任意线程直接返回原 Bucket。
///
/// @tparam T 对象池管理的对象类型。
/// @tparam BucketCount Bucket 数量，必须为 2 的幂。
/// @tparam BlocksPerSlab 每个 Slab 包含的 ObjectBlock 数量。
/// @tparam TaggedHead tagged head 类型，通常使用 TaggedHead128 或 TaggedHead64<>。
template <typename T, std::size_t BucketCount = 32, std::size_t BlocksPerSlab = 64, tagged_head TaggedHead = DefaultTaggedHead>
    requires std::is_object_v<T> && (!std::is_const_v<T>) && (!std::is_volatile_v<T>) && (BucketCount > 0) && (std::has_single_bit(BucketCount)) && (BlocksPerSlab > 0)
class ObjectPool final : public Immovable<ObjectPool<T, BucketCount, BlocksPerSlab, TaggedHead>> {
    using head_type = TaggedHead;
    using tag_type = typename head_type::tag_type;

    static constexpr std::size_t bucket_mask = BucketCount - 1;

    // ============================================================================
    // FreeStack
    // ============================================================================

    /// @brief 单个 Bucket 的 tagged 原子空闲栈状态。
    ///
    /// FreeStack 独占一个 cache line，避免高频 head CAS 与 refill 冷状态
    /// 产生伪共享。
    struct alignas(cache_line_size) FreeStack final {
        std::atomic<head_type> head{head_type{nullptr, tag_type{0}}};
    };

    // ============================================================================
    // ObjectBlock
    // ============================================================================

    /// @brief 保存对象存储以及空闲栈元数据的内部节点。
    ///
    /// free_stack 在 Slab 创建时永久绑定；next_free 始终保持独立 atomic 生命周期。
    /// 因此其它线程成功弹出 Block 并在 storage 中构造 T 后，CAS 失败线程继续读取
    /// 旧 Block 的 next_free 仍不会与 T 的生命周期发生重叠或形成非原子数据竞争。
    struct ObjectBlock final {
        /// @brief 当前 ObjectBlock 永久绑定的空闲栈。
        FreeStack* free_stack{nullptr};

        /// @brief 原子侵入式空闲链后继。
        std::atomic<ObjectBlock*> next_free{nullptr};

        /// @brief 一个 T 的原始存储。
        alignas(T) std::byte storage[sizeof(T)];

        /// @brief 返回用于构造 T 的原始存储地址。
        [[nodiscard]] TFL_FORCE_INLINE T* storage_ptr() noexcept {
            return reinterpret_cast<T*>(storage);
        }

        /// @brief 从对象地址恢复所属 ObjectBlock。
        [[nodiscard]] static TFL_FORCE_INLINE ObjectBlock* from_object(T* obj) noexcept {
            TFL_ASSERT(obj != nullptr);
            return reinterpret_cast<ObjectBlock*>(reinterpret_cast<std::byte*>(obj) - offsetof(ObjectBlock, storage));
        }
    };

    static_assert(std::is_standard_layout_v<ObjectBlock>, "ObjectBlock must be standard-layout");
    static_assert(std::is_trivially_destructible_v<ObjectBlock>, "ObjectBlock must be trivially destructible");

    // ============================================================================
    // Slab
    // ============================================================================

    /// @brief 固定包含 BlocksPerSlab 个长期存活 ObjectBlock 的内部 Slab。
    ///
    /// Slab 创建时一次完成所属 FreeStack 绑定和 next_free 本地链连接；整个 Slab
    /// 在 ObjectPool 析构前保持存在，因此任何并发 pop 持有的旧 ObjectBlock
    /// 地址都不会因为内存回收而悬空。
    struct Slab final {
        /// @brief 创建 Slab、链接前一个 Slab，并初始化完整 ObjectBlock 空闲链。
        explicit Slab(Slab* previous, FreeStack* free_stack) noexcept
            : m_previous{previous} {
            TFL_ASSERT(free_stack != nullptr);

            m_blocks[0].free_stack = free_stack;

            ObjectBlock* previous_block = std::addressof(m_blocks[0]);

            for (std::size_t i = 1; i < BlocksPerSlab; ++i) {
                ObjectBlock* current = std::addressof(m_blocks[i]);

                current->free_stack = free_stack;
                previous_block->next_free.store(current, std::memory_order_relaxed);
                previous_block = current;
            }
        }

        /// @brief 返回 Slab 中第一个 ObjectBlock。
        [[nodiscard]] TFL_FORCE_INLINE ObjectBlock* first() noexcept {
            return std::addressof(m_blocks[0]);
        }

        /// @brief 返回 Slab 中最后一个 ObjectBlock。
        [[nodiscard]] TFL_FORCE_INLINE ObjectBlock* last() noexcept {
            return std::addressof(m_blocks[BlocksPerSlab - 1]);
        }

        /// @brief 返回前一个 Slab；根 Slab 返回 nullptr。
        [[nodiscard]] TFL_FORCE_INLINE Slab* previous() noexcept {
            return m_previous;
        }

    private:
        /// @brief 前一个 Slab；根 Slab 为 nullptr。
        Slab* m_previous;

        /// @brief 当前 Slab 固定拥有的全部 ObjectBlock。
        ObjectBlock m_blocks[BlocksPerSlab];
    };

    static_assert(alignof(Slab) >= alignof(ObjectBlock));

    // ============================================================================
    // Bucket
    // ============================================================================

    /// @brief 一个独立对象池 Bucket。
    ///
    /// free_stack 为高频并发热状态并独占首个 cache line；refill_mutex 与
    /// slab_head 仅在补货和析构冷路径访问。
    struct alignas(cache_line_size) Bucket final {
        FreeStack free_stack{};

        std::mutex refill_mutex{};
        Slab* slab_head{nullptr};

        /// @brief 释放当前 Bucket 持有的全部 Slab。
        ~Bucket() noexcept {
            while (slab_head) {
                Slab* previous = slab_head->previous();
                delete slab_head;
                slab_head = previous;
            }
        }
    };

public:
    /// @brief 对象池 Bucket 数量。
    static constexpr std::size_t bucket_count = BucketCount;

    /// @brief 每个 Slab 包含的 ObjectBlock 数量。
    static constexpr std::size_t blocks_per_slab = BlocksPerSlab;

    /// @brief 原子空闲栈是否在目标平台始终使用 lock-free atomic。
    ///
    /// 这里只描述 free-stack 原子操作，不表示 ObjectPool 整体 lock-free；
    /// refill 冷路径仍然使用 mutex。
    static constexpr bool free_stack_is_always_lock_free =
        std::atomic<head_type>::is_always_lock_free &&
        std::atomic<ObjectBlock*>::is_always_lock_free;

    /// @brief 创建对象池并预填充每个 Bucket 的首个 Slab。
    /// @throws std::bad_alloc 任一初始 Slab 分配失败。
    ///
    /// 如果构造中途抛出异常，已经成功创建的 Bucket Slab 会由 Bucket 析构自动释放。
    ObjectPool() {
        for (Bucket& bucket : m_buckets) {
            Slab* slab = _allocate_slab(bucket);

            bucket.free_stack.head.store(
                head_type{static_cast<void*>(slab->first()), tag_type{0}},
                std::memory_order_relaxed
                );
        }
    }

    /// @brief 释放全部 Bucket 及其 Slab。
    ///
    /// @pre 所有通过 create() 创建的活动对象必须已经 destroy()。
    /// @pre 不得存在并发 create() 或 destroy()。
    ~ObjectPool() noexcept = default;

    /// @brief 从对象池构造并返回一个 T。
    ///
    /// 当前线程首先选择下一个 Bucket，并尝试从其原子空闲栈弹出 ObjectBlock。
    /// 只有空闲栈完全耗尽时才进入 refill 冷路径。
    ///
    /// 如果 T 构造抛出异常，已经取得的 ObjectBlock 会重新返回所属空闲栈。
    ///
    /// @tparam Args T 构造参数类型。
    /// @param args 转发给 T 构造函数的参数。
    /// @return 新构造对象地址。
    template <typename... Args>
        requires std::constructible_from<T, Args...>
    [[nodiscard]] TFL_FORCE_INLINE T* create(Args&&... args) {
        Bucket& bucket = m_buckets[_next_bucket()];

        ObjectBlock* block = _pop_block(bucket.free_stack);

        if (!block) [[unlikely]] {
            block = _refill_and_take(bucket);
        }

        if constexpr (std::is_nothrow_constructible_v<T, Args...>) {
            return std::construct_at(block->storage_ptr(), std::forward<Args>(args)...);
        } else {
            try {
                return std::construct_at(block->storage_ptr(), std::forward<Args>(args)...);
            } catch (...) {
                _push_block(*block->free_stack, block);
                throw;
            }
        }
    }

    /// @brief 析构对象并返回其永久绑定的空闲栈。
    ///
    /// destroy() 不重新选择 Bucket，也不访问 m_buckets。ObjectBlock 内保存
    /// 的 FreeStack 地址可以直接将 Block 返回原 Bucket，因此支持跨线程回收。
    ///
    /// @param obj create() 返回的对象；nullptr 为 no-op。
    /// @warning obj 必须来自当前 ObjectPool，并且不得重复 destroy。
    TFL_FORCE_INLINE void destroy(T* obj) noexcept(std::is_nothrow_destructible_v<T>) {
        if (!obj) {
            return;
        }

        ObjectBlock* block = ObjectBlock::from_object(obj);
        FreeStack* free_stack = block->free_stack;

        TFL_ASSERT(free_stack != nullptr);

        std::destroy_at(obj);

        _push_block(*free_stack, block);
    }

private:
    // ============================================================================
    // Slab ownership / refill
    // ============================================================================

    /// @brief 为 Bucket 创建一个新 Slab，并挂到其私有 Slab 所有权链头部。
    /// @return 新创建的 Slab。
    /// @throws std::bad_alloc Slab 分配失败。
    [[nodiscard]] static Slab* _allocate_slab(Bucket& bucket) {
        Slab* slab = new Slab{
            bucket.slab_head,
            std::addressof(bucket.free_stack)
        };

        TFL_ASSERT(head_type::can_encode(slab->first()));
        TFL_ASSERT(head_type::can_encode(slab->last()));

        bucket.slab_head = slab;

        return slab;
    }

    /// @brief 空闲栈耗尽时获取一个 ObjectBlock，并按完整 Slab 批量补货。
    ///
    /// 获得 refill_mutex 后先重新检查 free stack，因为等待锁期间其它线程可能
    /// 已经完成 refill 或 destroy。如果仍为空，则创建一个新 Slab：首个
    /// ObjectBlock 直接返回当前线程，其余 ObjectBlock 通过一次 CAS 整链发布。
    [[nodiscard]] static ObjectBlock* _refill_and_take(Bucket& bucket) {
        std::lock_guard guard{bucket.refill_mutex};

        if (ObjectBlock* block = _pop_block(bucket.free_stack)) {
            return block;
        }

        Slab* slab = _allocate_slab(bucket);
        ObjectBlock* block = slab->first();

        if constexpr (BlocksPerSlab > 1) {
            ObjectBlock* remaining = block->next_free.load(std::memory_order_relaxed);
            TFL_ASSERT(remaining != nullptr);
            _push_chain(bucket.free_stack, remaining, slab->last());
        }

        return block;
    }

    // ============================================================================
    // Free stack
    // ============================================================================

    /// @brief 将单个 ObjectBlock 压入指定原子空闲栈。
    static TFL_FORCE_INLINE void _push_block(FreeStack& stack, ObjectBlock* block) noexcept {
        TFL_ASSERT(block != nullptr);

        _push_chain(stack, block, block);
    }

    /// @brief 将完整 ObjectBlock 链通过一次成功 CAS 发布到空闲栈。
    ///
    /// first..last 必须已经连接完成。CAS 重试时仅修改 last->next_free，
    /// 不需要重新遍历链。
    static TFL_FORCE_INLINE void _push_chain(FreeStack& stack, ObjectBlock* first, ObjectBlock* last) noexcept {
        TFL_ASSERT(first != nullptr);
        TFL_ASSERT(last != nullptr);
        TFL_ASSERT(head_type::can_encode(first));

        head_type current = stack.head.load(std::memory_order_relaxed);
        head_type next;

        do {
            last->next_free.store(static_cast<ObjectBlock*>(current.pointer()), std::memory_order_relaxed);
            next = head_type{static_cast<void*>(first), head_type::next_tag(current.tag())};
        } while (!stack.head.compare_exchange_weak(
            current,
            next,
            std::memory_order_release,
            std::memory_order_relaxed
            ));
    }

    /// @brief 从指定原子空闲栈弹出一个 ObjectBlock。
    ///
    /// next_free 自身为 atomic，因此 CAS 失败线程即使继续持有旧 Block 并读取
    /// next_free，也不会和其它线程对该字段的更新形成非原子数据竞争。
    ///
    /// TaggedHead tag 用于阻止未发生版本回绕时的 ABA。
    ///
    /// @return 原栈顶 ObjectBlock；空栈返回 nullptr。
    [[nodiscard]] static TFL_FORCE_INLINE ObjectBlock* _pop_block(FreeStack& stack) noexcept {
        head_type current = stack.head.load(std::memory_order_acquire);

        while (ObjectBlock* block = static_cast<ObjectBlock*>(current.pointer())) {
            ObjectBlock* next_block = block->next_free.load(std::memory_order_relaxed);

            TFL_ASSERT(head_type::can_encode(next_block));

            const head_type next{static_cast<void*>(next_block), head_type::next_tag(current.tag())};

            if (stack.head.compare_exchange_weak(
                    current,
                    next,
                    std::memory_order_acquire,
                    std::memory_order_acquire
                    )) {
                return block;
            }
        }

        return nullptr;
    }

    // ============================================================================
    // Bucket selection
    // ============================================================================

    /// @brief 为当前线程选择下一个 Bucket。
    ///
    /// counter 为 thread_local，不产生共享原子访问。第一次初始化时使用
    /// thread id hash 获得不同起点，之后仅执行递增和位掩码。
    [[nodiscard]] static TFL_FORCE_INLINE std::size_t _next_bucket() noexcept {
        thread_local std::size_t counter = std::hash<std::thread::id>{}(std::this_thread::get_id());
        return counter++ & bucket_mask;
    }

    // ============================================================================
    // Data members
    // ============================================================================

    /// @brief 编译期固定数量的独立 Bucket。
    std::array<Bucket, BucketCount> m_buckets{};
};

}  // namespace tfl
