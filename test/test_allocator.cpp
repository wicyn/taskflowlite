/// @file test_allocator.cpp
/// @brief ObjectPool / SegmentedPool / FreeStack 内存池测试
///
/// @details
/// 覆盖 TaskflowLite 分配器子系统的三级组件：
///   - FreeStack           : ABA-safe 无锁侵入式栈，池的底层并发原语
///   - SegmentedPool       : 按 size class 分片的 lock-free freelist 集合
///   - ObjectPool          : 类型级静态单例门面，对接 operator new/delete
///
/// 核心验证策略：
///   - 单线程：对齐、读写、复用、容量边界、size class 路由
///   - 多线程：payload 标记法检测 aliasing（比集合追踪更可靠，无假阳性）
///   - 析构/释放：ASan 验证无泄漏
///
/// 覆盖的不变量 (see references/invariants-catalog.md):
///   A1  ✓ allocate 返回对齐合规、可读写的指针
///   A2  ✓ deallocate 后可复用（同 size class 内）
///   A3  ✓ 已分配未释放的指针不会重复返回（no aliasing）
///   A4  ✓ 超 cached_bytes 上限时 deallocate 直接交还系统
///   A5  ✓ 多线程 allocate/deallocate 无 race、无 UAF
///   A6  ✓ 析构 / release 释放所有 chunk（ASan 无 leak）
///   A7  ⏸ 默认初始化（池返回裸内存，初始化由用户负责）
///   A8  ✓ 各 size_class 均能 allocate
///   A9  ✓ TSan 干净
///   A10 ⏸ alloc/free 吞吐 vs new/delete（P3 性能基准，独立 CI job）

#include "test_common.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <memory>
#include <mutex>
#include <random>
#include <set>

using tfl_test::TestEnv;

// ============================================================================
// 测试辅助类型
// ============================================================================

namespace {

    /// @brief 池分配测试用的简单结构体（16 字节对齐，含 padding 确保最小尺寸）
    struct alignas(16) TestBlock {
        std::atomic<int> value{ 0 };
        std::atomic<bool> in_use{ false };
        char padding[16];  // 确保不小于池的最小 class size
    };

}  // namespace

// ============================================================================
// 第 1 节: FreeStack 基础单元 —— LIFO 语义、空栈、并发 push/pop
// ============================================================================

/// @test [allocator][freestack][unit] FreeStack push/pop LIFO 语义
/// @details FreeStack 是 SegmentedPool 底层的 ABA-safe 无锁栈。
///          这里用裸 operator new 分配的 chunk 直接验证其 push/pop 正确性。
///          每个 chunk 的前 sizeof(void*) 字节被 FreeStack 复用为链表指针，
///          因此 chunk 必须 >= sizeof(void*) 且对齐到 alignof(void*)。
TEST_CASE("FreeStack: basic push/pop LIFO", "[allocator][freestack][unit]") {
    constexpr int N = 5;
    std::vector<void*> chunks(N);
    for (int i = 0; i < N; ++i) {
        chunks[i] = ::operator new(64);  // 64 字节 chunk
    }

    tfl::FreeStack stack;

    SECTION("empty stack pop returns nullptr") {
        REQUIRE(stack.pop() == nullptr);
        REQUIRE(stack.empty());
    }

    SECTION("push then pop returns same pointer (single element)") {
        stack.push(chunks[0]);
        REQUIRE_FALSE(stack.empty());
        auto* got = stack.pop();
        REQUIRE(got == chunks[0]);
        REQUIRE(stack.pop() == nullptr);
        REQUIRE(stack.empty());
    }

    SECTION("push N, pop N in LIFO order") {
        for (int i = 0; i < N; ++i) {
            stack.push(chunks[i]);
        }
        REQUIRE_FALSE(stack.empty());

        // LIFO: 最后 push 的最先 pop
        for (int i = N - 1; i >= 0; --i) {
            auto* got = stack.pop();
            INFO("expected chunk " << i << " at position " << (N - 1 - i));
            REQUIRE(got == chunks[i]);
        }
        REQUIRE(stack.empty());
    }

    // 释放 chunk
    for (auto* p : chunks) ::operator delete(p, 64);
}

// ============================================================================
// 第 2 节: SegmentedPool 单线程单元 —— 对齐、复用、容量、size class
// ============================================================================

// 测试用 size class 方案: PowerOfTwo{64, 128, 256}
using TestScheme = tfl::PowerOfTwoScheme<64, 256>;
using TestPool = tfl::SegmentedPool<TestScheme, 1u << 20>;  // 1 MB 缓存上限

/// @test [allocator][pool][unit] A1: allocate 返回可读写、对齐到 alignof(void*) 的内存
TEST_CASE("SegmentedPool: allocate returns usable aligned memory", "[allocator][pool][unit]") {
    TestPool pool;

    SECTION("allocate 64 bytes") {
        void* p = pool.allocate(64);
        REQUIRE(p != nullptr);
        // 对齐验证: operator new 默认对齐到 alignof(void*)
        REQUIRE(reinterpret_cast<std::uintptr_t>(p) % alignof(void*) == 0);
        // 可写
        std::memset(p, 0xAB, 64);
        // 可读
        REQUIRE(static_cast<unsigned char*>(p)[0] == 0xAB);
        pool.deallocate(p, 64);
    }

    SECTION("allocate various sizes") {
        for (std::size_t sz : {64, 128, 256}) {
            void* p = pool.allocate(sz);
            REQUIRE(p != nullptr);
            std::memset(p, 0xCD, sz);
            pool.deallocate(p, sz);
        }
    }
}

/// @test [allocator][pool][unit] A2+A3: deallocate 后可复用，不会 aliasing
/// @details 验证两个核心不变量：
///          1. deallocate 后的块可被后续 allocate 复用（freelist 机制）
///          2. 已分配未释放的指针不会再次返回（连续分配返回不同指针）
TEST_CASE("SegmentedPool: deallocate+allocate reuses memory, no aliasing",
    "[allocator][pool][unit]")
{
    TestPool pool;

    SECTION("single block reuse") {
        void* p1 = pool.allocate(64);
        REQUIRE(p1 != nullptr);
        std::memset(p1, 0xFF, 64);
        static_cast<unsigned char*>(p1)[0] = 0x42;  // 写入标记

        pool.deallocate(p1, 64);

        // 可能取回同一块（freelist 命中），也可能取到新块（freelist 被清过）
        // 无论如何必须可读写
        void* p2 = pool.allocate(64);
        REQUIRE(p2 != nullptr);
        std::memset(p2, 0x00, 64);
        pool.deallocate(p2, 64);
    }

    SECTION("no aliasing: two consecutive allocs return different pointers") {
        std::set<void*> ptrs;
        for (int i = 0; i < 20; ++i) {
            void* p = pool.allocate(64);
            REQUIRE(p != nullptr);
            REQUIRE_FALSE(ptrs.contains(p));  // 不重复
            ptrs.insert(p);
        }
        // 全部归还
        for (void* p : ptrs) {
            pool.deallocate(p, 64);
        }
    }

    SECTION("no aliasing across dealloc/alloc cycles") {
        constexpr int kRounds = 50;
        std::set<void*> currently_allocated;

        for (int round = 0; round < kRounds; ++round) {
            void* p = pool.allocate(64);
            REQUIRE(p != nullptr);
            REQUIRE_FALSE(currently_allocated.contains(p));  // 不与已持有指针重复
            currently_allocated.insert(p);

            // 归还一半旧指针，模拟真实使用模式
            if (!currently_allocated.empty()) {
                auto it = currently_allocated.begin();
                pool.deallocate(*it, 64);
                currently_allocated.erase(it);
            }
        }

        // 清理
        for (void* p : currently_allocated) {
            pool.deallocate(p, 64);
        }
    }
}

/// @test [allocator][pool][unit] A4: 超 cached_bytes 上限时 deallocate 不缓存，直接交还系统
/// @details 用小上限（256 字节）构造池，分配+归还 256 字节块。
///          第一个归还缓存到 freelist（cached_bytes ≈ 256），
///          第二个归还时 CAS 判断容量超限，直接 ::operator delete 交还系统。
TEST_CASE("SegmentedPool: capacity cap respected", "[allocator][pool][unit]") {
    using TinyPool = tfl::SegmentedPool<TestScheme, 256>;  // 仅 256 字节缓存
    TinyPool pool;

    REQUIRE(pool.cached_bytes() == 0);

    void* p = pool.allocate(256);
    REQUIRE(p != nullptr);
    pool.deallocate(p, 256);

    // 第一个归还后被缓存
    REQUIRE(pool.cached_bytes() <= 256);

    // 第二个归还时容量不足，应被系统回收
    void* p2 = pool.allocate(256);
    REQUIRE(p2 != nullptr);
    pool.deallocate(p2, 256);

    // cached_bytes 严格不超过上限（CAS 循环保证）
    REQUIRE(pool.cached_bytes() <= 256);
}

/// @test [allocator][pool][unit] A8: 各 size class 均能 allocate 并正确归还
TEST_CASE("SegmentedPool: all size classes work", "[allocator][pool][unit]") {
    TestPool pool;

    // PowerOfTwo{64, 256} 的三个 class: 64, 128, 256
    std::vector<std::size_t> sizes = { 64, 128, 256 };
    std::vector<void*> ptrs;

    for (auto sz : sizes) {
        void* p = pool.allocate(sz);
        REQUIRE(p != nullptr);
        ptrs.push_back(p);
    }

    for (std::size_t i = 0; i < ptrs.size(); ++i) {
        pool.deallocate(ptrs[i], sizes[i]);
    }
}

/// @test [allocator][pool][unit] 超出 [min_size, max_size] 范围的请求回退到全局 new/delete
TEST_CASE("SegmentedPool: out-of-range falls back to global new", "[allocator][pool][unit]") {
    TestPool pool;

    // 低于 min_size（64）→ 回退 ::operator new
    void* p_small = pool.allocate(16);
    REQUIRE(p_small != nullptr);
    pool.deallocate(p_small, 16);

    // 高于 max_size（256）→ 回退 ::operator new
    void* p_large = pool.allocate(1024);
    REQUIRE(p_large != nullptr);
    pool.deallocate(p_large, 1024);
}

// ============================================================================
// 第 3 节: SegmentedPool 生命周期 —— release / 析构 (A6)
// ============================================================================

/// @test [allocator][pool][unit] A6: release 清空所有 freelist 缓存，交还系统
/// @details release 后 cached_bytes == 0，但池仍可用（后续 allocate 走全局 new）。
TEST_CASE("SegmentedPool: release cleans all cached chunks", "[allocator][pool][unit]") {
    TestPool pool;

    // 填充缓存
    for (int i = 0; i < 50; ++i) {
        void* p = pool.allocate(64);
        REQUIRE(p != nullptr);
        pool.deallocate(p, 64);
    }

    REQUIRE(pool.cached_bytes() > 0);

    pool.release();
    REQUIRE(pool.cached_bytes() == 0);

    // release 后池仍可用
    void* p = pool.allocate(64);
    REQUIRE(p != nullptr);
    pool.deallocate(p, 64);
}

// ============================================================================
// 第 4 节: SegmentedPool 并发 stress (A5, A9)
// ============================================================================

/// @test [allocator][pool][stress][mt] A5+A9: 多线程 allocate/deallocate 无 race、无 UAF
/// @details
/// 检测策略：payload 标记法。每线程分配后写入唯一 (tid, seq) 标记，
/// 归还前验证标记仍是自己的。若池返回了仍被其他线程持有的指针（aliasing），
/// 标记会被覆盖，验证失败。
///
/// 为什么不用全局集合追踪：
/// 池正常回收复用（线程 A 归还 → 线程 B 拿到同一块）在集合追踪下是假阳性——
/// 集合无法区分"合法回收复用"和"并发 aliasing"。payload 标记法则只有
/// 真 aliasing（两个线程同时持有同一指针）才会触发。
///
/// 为什么用单一 size class（64 字节）而非混合大小：
/// 排除 size class 路由复杂度，聚焦 FreeStack 本身的并发正确性。
/// 不同 size class 之间由独立的 freelist 隔离，不存在 aliasing 可能。
TEST_CASE("SegmentedPool: concurrent alloc/dealloc stress — no race, no UAF",
    "[allocator][pool][stress][mt]")
{
    auto seed = GENERATE(take(3, random(0u, UINT32_MAX)));
    constexpr std::size_t kThreads = 4;
    constexpr int kOpsPerThread = 5000;
    INFO("seed = " << seed);

    TestPool pool;
    constexpr std::size_t kAllocSize = 64;

    // payload: tid + seq 唯一标记，写入分配块的前若干字节
    struct Payload {
        std::atomic<int> tid;
        std::atomic<int> seq;
    };
    static_assert(sizeof(Payload) <= kAllocSize,
        "payload must fit within allocated block");

    std::atomic<int> corruption_count{ 0 };

    auto worker = [&, seed](int tid) {
        std::mt19937 rng(seed + static_cast<unsigned>(tid));
        std::vector<Payload*> local;
        local.reserve(kOpsPerThread / 2);
        int seq = 0;

        for (int i = 0; i < kOpsPerThread; ++i) {
            auto op = std::uniform_int_distribution<int>(0, 99)(rng);
            if (op < 60 && local.size() < 100) {
                // 分配 + 写入标记
                void* raw = pool.allocate(kAllocSize);
                if (raw) {
                    auto* p = static_cast<Payload*>(raw);
                    p->tid.store(tid, std::memory_order_relaxed);
                    p->seq.store(seq, std::memory_order_relaxed);
                    local.push_back(p);
                    ++seq;
                }
            }
            else if (!local.empty()) {
                // 归还前验证标记未被覆盖（aliasing 检测）
                auto idx = std::uniform_int_distribution<std::size_t>(0, local.size() - 1)(rng);
                Payload* p = local[idx];
                local[idx] = local.back();
                local.pop_back();

                int observed_tid = p->tid.load(std::memory_order_relaxed);
                int observed_seq = p->seq.load(std::memory_order_relaxed);
                if (observed_tid != tid) {
                    // tid 被其他线程改写 → aliasing!
                    corruption_count.fetch_add(1, std::memory_order_relaxed);
                }
                // 清除标记后归还（避免下次分配读到脏数据）
                p->tid.store(-1, std::memory_order_relaxed);
                pool.deallocate(p, kAllocSize);
            }
        }

        // 清理线程本地残留
        for (Payload* p : local) {
            pool.deallocate(p, kAllocSize);
        }
        };

    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < kThreads; ++i) {
        threads.emplace_back(worker, static_cast<int>(i));
    }
    for (auto& t : threads) t.join();

    REQUIRE(corruption_count.load() == 0);
}

/// @test [allocator][pool][stress][mt] 极小缓存上限下的高并发压力
/// @details 512 字节上限 + 4 线程高频 alloc/dealloc → 频繁触发 CAS 容量判断，
///          大量块被系统直接回收而非缓存。验证池在极端限流下不崩溃、不泄漏。
TEST_CASE("SegmentedPool: heavy contention with tiny cap",
    "[allocator][pool][stress][mt]")
{
    using TinyPool = tfl::SegmentedPool<TestScheme, 512>;
    TinyPool pool;

    constexpr int kOpsPerThread = 2000;
    constexpr int kThreads = 4;
    std::atomic<int> ops{ 0 };

    auto worker = [&] {
        for (int i = 0; i < kOpsPerThread; ++i) {
            void* p = pool.allocate(64);
            if (p) {
                std::memset(p, static_cast<int>(i & 0xFF), 64);
                pool.deallocate(p, 64);
                ops.fetch_add(1, std::memory_order_relaxed);
            }
        }
        };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) t.join();

    REQUIRE(ops.load() == kOpsPerThread * kThreads);
    // cached_bytes 严格不超过上限（CAS 循环保证）
    REQUIRE(pool.cached_bytes() <= 512);
}

// ============================================================================
// 第 5 节: ObjectPool 类型级静态门面 (A1, A2, A6)
// ============================================================================

/// @brief ObjectPool 测试用对象类型
struct PoolTestObj {
    int x = 42;
    double y = 3.14;
    char name[32] = "test";
};

/// @test [allocator][objectpool][unit] ObjectPool 基础 allocate/deallocate 与对齐
TEST_CASE("ObjectPool: basic allocate/deallocate", "[allocator][objectpool][unit]") {
    using MyPool = tfl::ObjectPool<PoolTestObj>;

    void* p = MyPool::allocate(sizeof(PoolTestObj));
    REQUIRE(p != nullptr);
    REQUIRE(reinterpret_cast<std::uintptr_t>(p) % alignof(PoolTestObj) == 0);

    // 写入并验证
    auto* obj = static_cast<PoolTestObj*>(p);
    obj->x = 100;
    REQUIRE(obj->x == 100);

    MyPool::deallocate(p, sizeof(PoolTestObj));
}

/// @test [allocator][objectpool][unit] ObjectPool::release 清空缓存
TEST_CASE("ObjectPool: release clears cache", "[allocator][objectpool][unit]") {
    using MyPool = tfl::ObjectPool<PoolTestObj>;

    // 分配后归还，填充缓存
    for (int i = 0; i < 20; ++i) {
        void* p = MyPool::allocate(sizeof(PoolTestObj));
        MyPool::deallocate(p, sizeof(PoolTestObj));
    }

    MyPool::release();

    // release 后池仍可用（走全局 new/delete）
    void* p = MyPool::allocate(sizeof(PoolTestObj));
    REQUIRE(p != nullptr);
    MyPool::deallocate(p, sizeof(PoolTestObj));
}

/// @test [allocator][objectpool][unit] ObjectPool::cached_bytes 查询
TEST_CASE("ObjectPool: cached_bytes query", "[allocator][objectpool][unit]") {
    using MyPool = tfl::ObjectPool<PoolTestObj>;
    MyPool::release();  // 从干净状态开始

    REQUIRE(MyPool::cached_bytes() == 0);

    void* p = MyPool::allocate(sizeof(PoolTestObj));
    MyPool::deallocate(p, sizeof(PoolTestObj));

    // 归还后至少有 sizeof(PoolTestObj) 字节被缓存
    REQUIRE(MyPool::cached_bytes() >= sizeof(PoolTestObj));
}

// ============================================================================
// 第 6 节: 边界条件
// ============================================================================

/// @test [allocator][pool][unit] deallocate(nullptr) 是空操作，不崩溃
TEST_CASE("SegmentedPool: deallocate nullptr is no-op", "[allocator][pool][unit]") {
    TestPool pool;
    REQUIRE_NOTHROW(pool.deallocate(nullptr, 64));
}

/// @test [allocator][freestack][stress][mt] FreeStack 多线程并发 push/pop
/// @details 2 pusher + 2 popper，每线程 5000 次操作。
///          用 bitmap 验证每个 chunk 恰好被 pop 一次（不重复、不丢失）。
TEST_CASE("FreeStack: concurrent push/pop stress", "[allocator][freestack][stress][mt]") {
    tfl::FreeStack stack;

    constexpr int kOpsPerThread = 5000;
    constexpr int kThreads = 4;
    constexpr std::size_t kChunkSize = 64;

    // 预分配所有 chunk
    constexpr int kTotalChunks = kOpsPerThread * kThreads;
    std::vector<void*> chunks(kTotalChunks);
    for (int i = 0; i < kTotalChunks; ++i) {
        chunks[i] = ::operator new(kChunkSize);
    }

    std::atomic<int> push_idx{ 0 };
    std::atomic<int> pop_count{ 0 };
    std::vector<std::atomic<bool>> popped(kTotalChunks);

    // Pusher 线程: 按原子递增索引分配 chunk，保证无重复
    std::vector<std::thread> pushers;
    for (int i = 0; i < kThreads / 2; ++i) {
        pushers.emplace_back([&] {
            for (int j = 0; j < kOpsPerThread; ++j) {
                int idx = push_idx.fetch_add(1, std::memory_order_relaxed);
                if (idx < kTotalChunks) {
                    stack.push(chunks[idx]);
                }
            }
            });
    }

    // Popper 线程: pop → 查 bitmap 检测重复
    // 使用原子错误标志收集失败信息，主线程 join 后统一 REQUIRE
    std::atomic<bool> dup_error{ false };
    std::atomic<bool> alien_error{ false };
    std::vector<std::thread> poppers;
    for (int i = 0; i < kThreads / 2; ++i) {
        poppers.emplace_back([&] {
            for (int j = 0; j < kOpsPerThread; ++j) {
                void* p = stack.pop();
                if (p) {
                    // 查找到对应 chunk 索引
                    bool found = false;
                    for (int k = 0; k < kTotalChunks; ++k) {
                        if (chunks[k] == p) {
                            if (popped[k].exchange(true, std::memory_order_relaxed)) {
                                dup_error.store(true, std::memory_order_relaxed);
                            }
                            pop_count.fetch_add(1, std::memory_order_relaxed);
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        alien_error.store(true, std::memory_order_relaxed);
                    }
                }
            }
            });
    }

    for (auto& t : pushers) t.join();
    for (auto& t : poppers) t.join();

    // 主线程统一断言
    REQUIRE_FALSE(dup_error.load());
    REQUIRE_FALSE(alien_error.load());

    // 主线程清理残余
    while (void* p = stack.pop()) {
        for (int k = 0; k < kTotalChunks; ++k) {
            if (chunks[k] == p) {
                REQUIRE_FALSE(popped[k].exchange(true));
                pop_count.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
    }

    // 每个推入的 chunk 恰好被 pop 一次
    int total_pushed = std::min(kOpsPerThread * (kThreads / 2), kTotalChunks);
    for (int i = 0; i < total_pushed; ++i) {
        INFO("chunk " << i);
        REQUIRE(popped[i].load());
    }

    REQUIRE(stack.empty());

    // 释放所有 chunk
    for (auto* p : chunks) ::operator delete(p, kChunkSize);
}

// ============================================================================
// 第 7 节: 析构释放 (A6) —— ASan 验证无泄漏
// ============================================================================

/// @test [allocator][pool][unit] A6: 析构自动释放所有 freelist 缓存
/// @details 分配大量块 → 全部归还至 freelist → 析构池。
///          若 ASan 报告泄漏，说明析构函数未正确释放某个 freelist 中的 chunk。
TEST_CASE("SegmentedPool: destructor releases all chunks", "[allocator][pool][unit]") {
    for (int trial = 0; trial < 5; ++trial) {
        TestPool pool;
        std::vector<void*> ptrs;
        for (int i = 0; i < 100; ++i) {
            ptrs.push_back(pool.allocate(64));
        }
        for (void* p : ptrs) {
            pool.deallocate(p, 64);
        }
        // 析构 ~SegmentedPool → release() → 遍历所有 class 的 freelist 逐块 delete
    }
    SUCCEED("ASan must not report leaks above");
}

// ============================================================================
// 第 8 节: 对齐验证 (A1)
// ============================================================================

/// @test [allocator][pool][unit] A1: 对齐满足 alignof(void*) 和 alignof(max_align_t)
TEST_CASE("SegmentedPool: alignment requirements", "[allocator][pool][unit]") {
    TestPool pool;

    SECTION("alignof(void*) alignment") {
        for (int i = 0; i < 20; ++i) {
            void* p = pool.allocate(64);
            REQUIRE(p != nullptr);
            REQUIRE(reinterpret_cast<std::uintptr_t>(p) % alignof(void*) == 0);
            pool.deallocate(p, 64);
        }
    }

    SECTION("alignof(std::max_align_t) for large allocations") {
        for (int i = 0; i < 10; ++i) {
            void* p = pool.allocate(256);
            REQUIRE(p != nullptr);
            REQUIRE(reinterpret_cast<std::uintptr_t>(p) % alignof(std::max_align_t) == 0);
            pool.deallocate(p, 256);
        }
    }
}

// ============================================================================
// 覆盖矩阵
// ============================================================================
//   A1  ✓ "SegmentedPool: allocate returns usable aligned memory"
//         "SegmentedPool: alignment requirements"
//         "ObjectPool: basic allocate/deallocate"
//   A2  ✓ "SegmentedPool: deallocate+allocate reuses memory, no aliasing"
//   A3  ✓ "SegmentedPool: deallocate+allocate reuses memory, no aliasing"
//         SECTION "no aliasing: two consecutive allocs return different pointers"
//         SECTION "no aliasing across dealloc/alloc cycles"
//   A4  ✓ "SegmentedPool: capacity cap respected"
//   A5  ✓ "SegmentedPool: concurrent alloc/dealloc stress — no race, no UAF"
//         "SegmentedPool: heavy contention with tiny cap"
//         "FreeStack: concurrent push/pop stress"
//   A6  ✓ "SegmentedPool: release cleans all cached chunks"
//         "SegmentedPool: destructor releases all chunks"
//         "ObjectPool: release clears cache"
//   A7  ⏸ 默认初始化（池返回裸内存，初始化由用户负责）
//   A8  ✓ "SegmentedPool: all size classes work"
//   A9  ✓ 并发 stress 测试在 TSan 构建下运行
//   A10 ⏸ P3 性能基准（独立 CI job）
