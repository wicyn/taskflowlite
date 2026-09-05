/// @file test_allocator.cpp
/// @brief ObjectPool / TaggedHead 内存池测试 —— 构造、析构、对齐、复用与并发。
///
/// 当前 ObjectPool<T> 是拥有 slab 的实例，不再提供按字节大小分配接口。
/// 池必须比全部存活对象更长寿；每个 create 必须配对一次 destroy。

#include "test_common.hpp"
#include "../taskflowlite/core/object_pool.hpp"

#include <array>
#include <cstdint>
#include <unordered_set>

namespace {

struct alignas(128) PoolValue {
    std::atomic<int>* live;
    int value;
    explicit PoolValue(std::atomic<int>& count, int n) : live(&count), value(n) {
        live->fetch_add(1);
    }
    ~PoolValue() { live->fetch_sub(1); }
};

struct ThrowingValue {
    explicit ThrowingValue(bool fail) {
        if (fail) throw std::runtime_error("constructor");
    }
};

}  // namespace

// ============================================================================
// SECTION 1: TaggedHead 指针与版本编码
// ============================================================================

/// @test [allocator][head] 两种头布局均保留指针与版本，并允许空指针。
TEST_CASE("TaggedHead: pointer tag and wraparound", "[allocator][head]") {
    int value = 42;
    SECTION("128-bit head") {
        tfl::TaggedHead128 head{&value, 17};
        REQUIRE(head.pointer() == &value);
        REQUIRE(head.tag() == 17);
        REQUIRE(tfl::TaggedHead128::can_encode(&value));
        REQUIRE(tfl::TaggedHead128{}.pointer() == nullptr);
        REQUIRE(tfl::TaggedHead128::next_tag(std::numeric_limits<tfl::TaggedHead128::tag_type>::max()) == 0);
    }
    SECTION("64-bit head") {
        using Head = tfl::TaggedHead64<>;
        REQUIRE(Head::can_encode(&value));
        Head head{&value, 23};
        REQUIRE(head.pointer() == &value);
        REQUIRE(head.tag() == 23);
        REQUIRE(Head{}.pointer() == nullptr);
        REQUIRE(Head::next_tag(std::numeric_limits<Head::tag_type>::max()) == 0);
    }
    STATIC_REQUIRE(tfl::tagged_head<tfl::TaggedHead128>);
    STATIC_REQUIRE(tfl::tagged_head<tfl::TaggedHead64<>>);
    STATIC_REQUIRE_FALSE(tfl::tagged_head<int>);
}

// ============================================================================
// SECTION 2: 构造、生命周期与 slab 扩容
// ============================================================================

/// @test [allocator][pool] 跨多个 slab 分配，活动地址唯一且符合过对齐要求。
TEST_CASE("ObjectPool: constructs unique aligned objects across slabs", "[allocator][pool]") {
    std::atomic<int> live{0};
    tfl::ObjectPool<PoolValue, 1, 4> pool;
    std::vector<PoolValue*> objects;
    std::unordered_set<PoolValue*> addresses;
    for (int i = 0; i < 33; ++i) {
        auto* object = pool.create(live, i);
        objects.push_back(object);
        REQUIRE(addresses.insert(object).second);
        REQUIRE(reinterpret_cast<std::uintptr_t>(object) % alignof(PoolValue) == 0);
        REQUIRE(object->value == i);
    }
    REQUIRE(live.load() == 33);
    for (auto* object : objects) pool.destroy(object);
    REQUIRE(live.load() == 0);
}

/// @test [allocator][reuse] 单桶销毁后的块可以复用，同时重新调用构造函数。
TEST_CASE("ObjectPool: destroy makes storage reusable", "[allocator][reuse]") {
    std::atomic<int> live{0};
    tfl::ObjectPool<PoolValue, 1, 1> pool;
    auto* first = pool.create(live, 7);
    pool.destroy(first);
    auto* second = pool.create(live, 42);
    REQUIRE(second == first);
    REQUIRE(second->value == 42);
    REQUIRE(live.load() == 1);
    pool.destroy(second);
    REQUIRE(live.load() == 0);
}

/// @test [allocator][exception] 构造抛异常归还块；后续分配和析构仍正常。
TEST_CASE("ObjectPool: constructor failure returns its block", "[allocator][exception]") {
    tfl::ObjectPool<ThrowingValue, 1, 1> pool;
    for (int i = 0; i < 20; ++i) {
        REQUIRE_THROWS_AS(pool.create(true), std::runtime_error);
        auto* value = pool.create(false);
        REQUIRE(value != nullptr);
        pool.destroy(value);
    }
}

/// @test [allocator][head] 显式选择 64-bit / 128-bit 头的池均可分配对象。
TEST_CASE("ObjectPool: selectable tagged head layouts", "[allocator][head][pool]") {
    tfl::ObjectPool<int, 2, 4, tfl::TaggedHead64<>> narrow;
    auto* a = narrow.create(17);
    REQUIRE(*a == 17);
    narrow.destroy(a);
    tfl::ObjectPool<int, 2, 4, tfl::TaggedHead128> wide;
    auto* b = wide.create(23);
    REQUIRE(*b == 23);
    wide.destroy(b);
}

// ============================================================================
// SECTION 3: 多线程争用
// ============================================================================

/// @test [allocator][stress] 活动指针集合检查重复分配；worker 只记录错误。
TEST_CASE("ObjectPool: concurrent create destroy has no active aliasing", "[allocator][stress][mt]") {
    std::atomic<int> live{0};
    std::atomic<bool> valid{true};
    tfl::ObjectPool<PoolValue, 2, 4> pool;
    std::mutex mutex;
    std::unordered_set<PoolValue*> active;
    std::vector<std::thread> threads;
    for (int id = 0; id < 8; ++id) {
        threads.emplace_back([&, id] {
            for (int i = 0; i < 2000; ++i) {
                auto* value = pool.create(live, id);
                {
                    std::lock_guard lock(mutex);
                    if (!active.insert(value).second) valid.store(false);
                }
                if (value->value != id) valid.store(false);
                {
                    std::lock_guard lock(mutex);
                    active.erase(value);
                }
                pool.destroy(value);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    REQUIRE(valid.load());
    REQUIRE(active.empty());
    REQUIRE(live.load() == 0);
}
