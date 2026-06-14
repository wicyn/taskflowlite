/// @file test_queue.cpp
/// @brief BoundedQueue / UnboundedQueue 无锁队列测试 —— 最核心的并发数据结构。
///
/// 注意：UnboundedQueue 现已移除 pop()，为 steal-only（仅窃取，FIFO 语义）。
///       所有原先依赖 UnboundedQueue::pop() 的用例改为用 steal() 验证 FIFO。
///       BoundedQueue 仍保留 owner pop()（LIFO）。
///       steal(std::size_t&) 空计数重载已从两个队列移除，相关用例删除。
///
/// 覆盖的不变量 (see references/invariants-catalog.md):
///   Q1  ✓ "push 满/未满的返回值" —— try_push / push on_full
///   Q2  ✓ "pop LIFO 语义" —— owner LIFO 验证 (BoundedQueue)
///   Q3  ✓ "steal FIFO 语义" —— stealer FIFO 验证
///   Q4  ✓ "push N 后 size == N" —— 容量不溢出
///   Q5  ✓ "被消费 ≤ 被 push" —— 性质式 fuzz
///   Q6  ✓ "每个元素至多被消费一次" —— stress 不重复
///   Q7  ✓ "每个元素至少被消费一次" —— stress 不丢失
///   Q8  ✓ "UnboundedQueue resize 正确" —— 溢出后自动扩容
///   Q9  ✓ "resize 不丢元素 / 不重复" —— 扩容并发安全性
///   Q10 ✓ "TSan 干净" —— 并发 stress
///   Q11 ✓ "ASan/UBSan 干净" —— leak 检查
///   Q12 ✓ "析构释放所有 buffer" —— ASan 无 leak
///   Q13 ✓ "模板参数校验" —— static_assert
///   Q14 ⏸ "benchmark 吞吐" (P3)
///   Q15 ⏸ "benchmark 缩放" (P3)

#include "test_common.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <random>
#include <set>
#include <vector>

using tfl_test::TestEnv;

namespace {

/// @brief Test payload: encodes index + checksum to detect corruption
struct Payload {
    std::uintptr_t index;
    std::uintptr_t checksum;  // ~index  for simple corruption detection
};

}  // namespace

// ============================================================================
// SECTION 1: BoundedQueue 单线程单元测试 (Q1, Q2, Q4)
// ============================================================================

/// @test [queue][bounded][unit][st] Q1+Q2+Q4: 单线程 LIFO + 容量边界
/// @details 使用 TEMPLATE_TEST_CASE_SIG 跨多种容量验证 LIFO 行为
TEMPLATE_TEST_CASE_SIG(
    "BoundedQueue: single-thread owner LIFO and capacity bounds",
    "[queue][bounded][unit][st]",
    ((std::size_t Cap), Cap),
    2, 4, 8, 64, 1024)
{
    tfl::BoundedQueue<int*, Cap> q;

    SECTION("empty queue try_pop returns nullptr") {
        REQUIRE(q.pop() == nullptr);
        REQUIRE(q.empty());
        REQUIRE(q.size() == 0);
    }

    SECTION("empty queue steal returns nullptr") {
        REQUIRE(q.steal() == nullptr);
    }

    SECTION("push N (N < Cap) then size() == N") {
        auto n = GENERATE(values<std::size_t>({ 1, Cap / 2 + 1, Cap - 1 }));
        if (n >= Cap) return;
        for (std::size_t i = 1; i <= n; ++i) {
            REQUIRE(q.try_push(reinterpret_cast<int*>(i)));
        }
        REQUIRE(q.size() == n);
    }

    SECTION("push then pop observes LIFO order") {
        for (std::size_t i = 1; i <= Cap; ++i) {
            REQUIRE(q.try_push(reinterpret_cast<int*>(i)));
        }
        for (std::uintptr_t expected = Cap; expected >= 1; --expected) {
            INFO("LIFO expected " << expected);
            auto got = q.pop();
            REQUIRE(got != nullptr);
            REQUIRE(reinterpret_cast<std::uintptr_t>(got) == expected);
        }
        REQUIRE(q.pop() == nullptr);
        REQUIRE(q.empty());
    }

    SECTION("full queue try_push returns false") {
        for (std::size_t i = 1; i <= Cap; ++i) {
            REQUIRE(q.try_push(reinterpret_cast<int*>(i)));
        }
        REQUIRE(q.size() == Cap);
        REQUIRE_FALSE(q.try_push(reinterpret_cast<int*>(999)));
        REQUIRE(q.size() == Cap);  // size unchanged after failed push
    }

    SECTION("capacity() == Cap") {
        REQUIRE(q.capacity() == Cap);
    }
}

/// @test [queue][bounded][unit][st] Q1: push with on_full callback
TEST_CASE("BoundedQueue: push with on_full callback", "[queue][bounded][unit][st]") {
    tfl::BoundedQueue<int*, 8> q;

    SECTION("on_full called when queue is full") {
        // Fill queue
        for (int i = 0; i < 8; ++i) {
            q.push(reinterpret_cast<int*>(static_cast<std::uintptr_t>(i + 1)),
                   [] { FAIL("should not overflow yet"); });
        }
        REQUIRE(q.size() == 8);

        bool overflow_called = false;
        q.push(reinterpret_cast<int*>(static_cast<std::uintptr_t>(999)),
               [&] { overflow_called = true; });
        REQUIRE(overflow_called);
        REQUIRE(q.size() == 8);  // element not pushed
    }

    SECTION("on_full NOT called when space available") {
        for (int i = 0; i < 4; ++i) {
            q.push(reinterpret_cast<int*>(static_cast<std::uintptr_t>(i + 1)),
                   [] { FAIL("unexpected overflow"); });
        }
        REQUIRE(q.size() == 4);
    }
}

/// @test [queue][bounded][unit][st] Q1: batch push with overflow
TEST_CASE("BoundedQueue: batch push with overflow callback", "[queue][bounded][unit][st]") {
    tfl::BoundedQueue<int*, 16> q;

    SECTION("batch push fits entirely") {
        std::vector<int*> items(8);
        for (std::size_t i = 0; i < 8; ++i) {
            items[i] = reinterpret_cast<int*>(i + 1);
        }
        q.push(items.begin(), 8, [](auto, std::size_t) { FAIL("unexpected overflow"); });
        REQUIRE(q.size() == 8);

                // Verify LIFO
        for (std::uintptr_t expected = 8; expected >= 1; --expected) {
            auto got = q.pop();
            REQUIRE(got != nullptr);
            REQUIRE(reinterpret_cast<std::uintptr_t>(got) == expected);
        }
    }

    SECTION("batch push partially overflows") {
        std::vector<int*> items(24);
        for (std::size_t i = 0; i < 24; ++i) {
            items[i] = reinterpret_cast<int*>(100 + i);
        }

        std::size_t overflow_count = 0;
        auto overflow_first = items.begin();
        q.push(items.begin(), 24,
               [&](auto it, std::size_t n) {
                   overflow_count = n;
                   overflow_first = it;
               });
        REQUIRE(q.size() == 16);
        REQUIRE(overflow_count > 0);
        REQUIRE(static_cast<std::size_t>(q.size()) + overflow_count == 24);
    }
}

// ============================================================================
// SECTION 2: BoundedQueue FIFO steal 语义 (Q3)
// ============================================================================

/// @test [queue][bounded][unit][st] Q3: single stealer observes FIFO order
TEST_CASE("BoundedQueue: single stealer FIFO order", "[queue][bounded][unit][st]") {
    tfl::BoundedQueue<int*, 64> q;

    SECTION("steal observes FIFO order") {
        constexpr std::size_t N = 50;
        for (std::size_t i = 1; i <= N; ++i) {
            REQUIRE(q.try_push(reinterpret_cast<int*>(i)));
        }

        for (std::size_t expected = 1; expected <= N; ++expected) {
            INFO("FIFO expected " << expected);
            auto got = q.steal();
            REQUIRE(got != nullptr);
            REQUIRE(reinterpret_cast<std::uintptr_t>(got) == expected);
        }
        REQUIRE(q.steal() == nullptr);  // empty
    }

    SECTION("steal from empty returns nullptr") {
        REQUIRE(q.steal() == nullptr);
    }
}

// ============================================================================
// SECTION 3: BoundedQueue 并发 stress (Q6, Q7, Q10, Q11)
// ============================================================================

/// @test [queue][bounded][stress][mt] Q6+Q7: 不重复 / 不丢失
/// @details 1 owner push + N stealers steal。用 UnboundedQueue 排除容量限制干扰。
///          UnboundedQueue 现为 steal-only：owner 仅生产，全部消费由 stealer 完成。
///          所有会抛的 Catch2 宏均在 join() 之后触发，杜绝 joinable 线程析构 terminate。
TEST_CASE("BoundedQueue: stress — no duplicate, no loss",
          "[queue][bounded][stress][mt]")
{
    auto seed = GENERATE(take(3, random(0u, UINT32_MAX)));
    auto n_stealers = GENERATE(1u, 2u, 4u, 8u);
    constexpr std::size_t kN = 50'000;
    INFO("seed = " << seed);
    INFO("n_stealers = " << n_stealers);

    tfl::UnboundedQueue<int*> q;
    std::vector<std::atomic<bool>> seen(kN);
    std::atomic<std::size_t> consumed{ 0 };
    std::atomic<bool> error{ false };  // 多线程错误标志
    std::atomic<bool> hung{ false };   // 超时标志，join 后再断言

    auto check_consume = [&](int* p) {
        auto v = reinterpret_cast<std::uintptr_t>(p);
        // 守卫：损坏的指针立刻放弃，避免 REQUIRE 失败后继续执行导致越界
        if (v < 1 || v > kN) {
            error.store(true, std::memory_order_relaxed);
            return;
        }
        std::size_t idx = static_cast<std::size_t>(v - 1);
        bool first = !seen[idx].exchange(true, std::memory_order_relaxed);
        if (!first) {
            INFO("duplicate payload " << v);
            error.store(true, std::memory_order_relaxed);
            return;
        }
        consumed.fetch_add(1, std::memory_order_relaxed);
    };

            // Stealer 线程
    std::vector<std::thread> stealers;
    for (std::size_t i = 0; i < n_stealers; ++i) {
        stealers.emplace_back([&, idx = i] {
            std::mt19937 rng(seed ^ idx);
            while (consumed.load(std::memory_order_relaxed) < kN
                   && !error.load(std::memory_order_relaxed)) {
                if (auto* p = q.steal(); p != nullptr) {
                    check_consume(p);
                }
                if (std::uniform_int_distribution<int>(0, 15)(rng) == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }

            // Owner: 推入全部（steal-only：owner 仅生产，不消费）
    for (std::uintptr_t i = 1; i <= kN; ++i) {
        q.push(reinterpret_cast<int*>(i));
    }

            // 等待 stealer 排空
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (consumed.load(std::memory_order_relaxed) < kN
           && !error.load(std::memory_order_relaxed)) {
        if (std::chrono::steady_clock::now() > deadline) {
            hung.store(true, std::memory_order_relaxed);
            error.store(true, std::memory_order_relaxed);  // 让 stealer 退出循环
            break;
        }
        std::this_thread::yield();
    }

    for (auto& t : stealers) t.join();  // 任何断言之前必须先 join

    if (hung.load()) {
        FAIL("stress hung at " << consumed.load() << "/" << kN);
    }
    REQUIRE_FALSE(error.load());
    REQUIRE(consumed.load() == kN);
    for (std::size_t i = 0; i < kN; ++i) {
        INFO("missing element " << (i + 1));
        REQUIRE(seen[i].load());
    }
}

/// @test [queue][bounded][stress][mt] Q6+Q7: BoundedQueue 精确填满容量的并发 stress
/// @details 单线程填满 capacity 个元素并验证"满"，随后启动 stealer，
///          进入 pop vs steal 的单元素争用仲裁阶段。
///          所有会抛的 Catch2 宏均在 join() 之后触发，杜绝 joinable 线程析构 terminate。
TEST_CASE("BoundedQueue: stress at exact capacity — no duplicate, no loss",
          "[queue][bounded][stress][mt]")
{
    auto seed = GENERATE(take(3, random(0u, UINT32_MAX)));
    constexpr std::size_t kCap = 64;
    constexpr std::size_t kN = kCap;
    INFO("seed = " << seed);

    tfl::BoundedQueue<int*, kCap> q;
    std::vector<std::atomic<bool>> seen(kN);
    std::atomic<std::size_t> consumed{ 0 };
    std::atomic<bool> error{ false };
    std::atomic<bool> hung{ false };  // 超时标志，join 后再断言

    auto check = [&](int* p) {
        auto v = reinterpret_cast<std::uintptr_t>(p);
        if (v < 1 || v > kN) { error.store(true, std::memory_order_relaxed); return; }
        std::size_t idx = static_cast<std::size_t>(v - 1);
        if (seen[idx].exchange(true, std::memory_order_relaxed)) {
            INFO("duplicate payload " << v);
            error.store(true, std::memory_order_relaxed);
            return;
        }
        consumed.fetch_add(1, std::memory_order_relaxed);
    };

            // ── Phase 1：单线程填满 + 验证"满"（尚无 stealer，断言合法且安全）──
    for (std::uintptr_t i = 1; i <= kN; ++i) {
        REQUIRE(q.try_push(reinterpret_cast<int*>(i)));
    }
    REQUIRE_FALSE(q.try_push(reinterpret_cast<int*>(kN + 1)));  // 此刻确实满

            // ── Phase 2：启动 stealer，进入并发 drain（排空）阶段 ──
    std::thread stealer([&] {
        while (consumed.load(std::memory_order_relaxed) < kN
               && !error.load(std::memory_order_relaxed)) {
            if (auto* p = q.steal(); p != nullptr) check(p);
            std::this_thread::yield();
        }
    });

            // Owner: pop（与 stealer 在最后一个元素上争用仲裁）
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (consumed.load(std::memory_order_relaxed) < kN
           && !error.load(std::memory_order_relaxed)) {
        if (auto* p = q.pop(); p != nullptr) check(p);
        if (std::chrono::steady_clock::now() > deadline) {
            hung.store(true, std::memory_order_relaxed);
            error.store(true, std::memory_order_relaxed);  // 让 stealer 也退出循环
            break;
        }
    }

    stealer.join();  // 任何断言之前必须先 join

            // ── Phase 3：join 之后再做所有会抛的断言 ──
    if (hung.load()) {
        FAIL("stress hung at " << consumed.load() << "/" << kN);
    }
    REQUIRE_FALSE(error.load());
    REQUIRE(consumed.load() == kN);
    for (std::size_t i = 0; i < kN; ++i) {
        INFO("missing element " << (i + 1));
        REQUIRE(seen[i].load());
    }
}

// ============================================================================
// SECTION 4: BoundedQueue 析构释放 (Q12)
// ============================================================================

/// @test [queue][bounded][unit] Q12: 析构释放所有 buffer (ASan 验证)
TEST_CASE("BoundedQueue: destructor releases buffer", "[queue][bounded][unit]") {
    // This test has no explicit assertions — ASan verifies no leaks
    for (int trial = 0; trial < 10; ++trial) {
        tfl::BoundedQueue<int*, 256> q;
        for (int i = 0; i < 200; ++i) {
            q.push(reinterpret_cast<int*>(static_cast<std::uintptr_t>(i + 1)),
                   [] {});
        }
        // Pop half
        for (int i = 0; i < 100; ++i) {
            q.pop();
        }
        // Destructor should release remaining buffer
    }
    SUCCEED("ASan must not report leaks above");
}

// ============================================================================
// SECTION 5: static_assert 模板参数验证 (Q13)
// ============================================================================

/// @test [queue][bounded][compile] Q13: 编译期模板参数约束
TEST_CASE("BoundedQueue: static assertions on template parameters", "[queue][bounded][compile]") {
    // Cap must be power of 2 and > 1
    STATIC_REQUIRE((tfl::BoundedQueue<int*, 2>::capacity() == 2));
    STATIC_REQUIRE((tfl::BoundedQueue<int*, 64>::capacity() == 64));
    STATIC_REQUIRE((tfl::BoundedQueue<int*, 1024>::capacity() == 1024));

            // Type must be a pointer
    STATIC_REQUIRE(std::is_pointer_v<typename tfl::BoundedQueue<int*, 64>::value_type>);

            // BoundedQueue must be immovable
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<tfl::BoundedQueue<int*, 64>>);
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<tfl::BoundedQueue<int*, 64>>);

            // Noexcept destructor
    STATIC_REQUIRE(std::is_nothrow_destructible_v<tfl::BoundedQueue<int*, 64>>);
}

// ============================================================================
// SECTION 6: UnboundedQueue 单元测试 (Q8) —— steal-only
// ============================================================================

/// @test [queue][unbounded][unit][st] Q8: push 超过初始 capacity 自动 resize
TEST_CASE("UnboundedQueue: resize on overflow", "[queue][unbounded][unit][st]") {
    tfl::UnboundedQueue<int*> q(8);  // start small

    SECTION("push beyond initial capacity triggers resize") {
        auto initial_cap = q.capacity();
        REQUIRE(initial_cap >= 8);

                // Push more than initial capacity
        constexpr std::size_t N = 100;
        for (std::uintptr_t i = 1; i <= N; ++i) {
            q.push(reinterpret_cast<int*>(i));
        }
        REQUIRE(q.size() == N);
        REQUIRE(q.capacity() >= static_cast<std::int64_t>(N));

                // All elements should be recoverable via steal (FIFO)
        for (std::uintptr_t expected = 1; expected <= N; ++expected) {
            auto got = q.steal();
            REQUIRE(got != nullptr);
            REQUIRE(reinterpret_cast<std::uintptr_t>(got) == expected);
        }
        REQUIRE(q.empty());
    }

    SECTION("batch push triggers resize once") {
        std::vector<int*> items(200);
        for (std::size_t i = 0; i < 200; ++i) {
            items[i] = reinterpret_cast<int*>(1000 + i);
        }
        q.push(items.begin(), 200);
        REQUIRE(q.size() == 200);
        REQUIRE(q.capacity() >= 200);
    }

    SECTION("empty state after push+steal all") {
        for (std::uintptr_t i = 1; i <= 50; ++i) {
            q.push(reinterpret_cast<int*>(i));
        }
        for (std::uintptr_t i = 0; i < 50; ++i) {
            REQUIRE(q.steal() != nullptr);
        }
        REQUIRE(q.empty());
        REQUIRE(q.size() == 0);
        REQUIRE(q.steal() == nullptr);
    }

    SECTION("steal from empty fixed-size queue returns nullptr") {
        tfl::UnboundedQueue<int*> empty_q(16);
        REQUIRE(empty_q.steal() == nullptr);
    }
}

/// @test [queue][unbounded][unit][st] Q8: steal FIFO after resize
TEST_CASE("UnboundedQueue: steal FIFO with resize", "[queue][unbounded][unit][st]") {
    tfl::UnboundedQueue<int*> q(4);  // tiny initial capacity

    constexpr std::size_t N = 20;
    for (std::uintptr_t i = 1; i <= N; ++i) {
        q.push(reinterpret_cast<int*>(i));
    }

            // Steal in FIFO order
    for (std::uintptr_t expected = 1; expected <= N; ++expected) {
        INFO("FIFO expected " << expected << " after resize");
        auto got = q.steal();
        REQUIRE(got != nullptr);
        REQUIRE(reinterpret_cast<std::uintptr_t>(got) == expected);
    }
    REQUIRE(q.steal() == nullptr);
    REQUIRE(q.empty());
}

// ============================================================================
// SECTION 7: UnboundedQueue 并发 stress (Q9)
// ============================================================================

/// @test [queue][unbounded][stress][mt] Q9: resize during concurrent steal
/// @details Forces resize by starting with tiny capacity, then pushing many items
///          while concurrent stealers consume them. No duplicates, no loss.
///          UnboundedQueue 为 steal-only：owner 仅生产，全部消费由 stealer 完成。
TEST_CASE("UnboundedQueue: concurrent resize stress — no duplicate, no loss",
          "[queue][unbounded][stress][mt]")
{
    auto seed = GENERATE(take(3, random(0u, UINT32_MAX)));
    auto n_stealers = GENERATE(1u, 2u, 4u);
    constexpr std::size_t kN = 30'000;
    INFO("seed = " << seed);
    INFO("n_stealers = " << n_stealers);

    tfl::UnboundedQueue<int*> q(4);  // tiny: will resize many times
    std::vector<std::atomic<bool>> seen(kN);
    std::atomic<std::size_t> consumed{ 0 };

    std::atomic<bool> check_error{ false };
    std::atomic<bool> hung{ false };

    auto check = [&](int* p) {
        auto v = reinterpret_cast<std::uintptr_t>(p);
        if (v < 1 || v > kN) {
            check_error.store(true, std::memory_order_relaxed);
            return;
        }
        std::size_t idx = static_cast<std::size_t>(v - 1);
        if (seen[idx].exchange(true, std::memory_order_relaxed)) {
            check_error.store(true, std::memory_order_relaxed);
            return;
        }
        consumed.fetch_add(1, std::memory_order_relaxed);
    };

            // Start stealers before pushing to test concurrent resize
    std::vector<std::thread> stealers;
    std::atomic<bool> start_push{ false };
    for (std::size_t i = 0; i < n_stealers; ++i) {
        stealers.emplace_back([&, idx = i] {
            // Wait for signal
            while (!start_push.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
            std::mt19937 rng(seed ^ (idx + 1000));
            while (consumed.load(std::memory_order_relaxed) < kN
                   && !check_error.load(std::memory_order_relaxed)) {
                if (auto* p = q.steal(); p != nullptr) check(p);
                if (std::uniform_int_distribution<int>(0, 31)(rng) == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }

            // Start pushing and signal stealers
    start_push.store(true, std::memory_order_relaxed);

    for (std::uintptr_t i = 1; i <= kN; ++i) {
        q.push(reinterpret_cast<int*>(i));
    }

            // 等待 stealer 排空（steal-only：owner 仅生产，不消费）
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (consumed.load(std::memory_order_relaxed) < kN
           && !check_error.load(std::memory_order_relaxed)) {
        if (std::chrono::steady_clock::now() > deadline) {
            hung.store(true, std::memory_order_relaxed);
            check_error.store(true, std::memory_order_relaxed);  // 让 stealer 退出循环
            break;
        }
        std::this_thread::yield();
    }

    for (auto& t : stealers) t.join();  // 任何断言之前必须先 join

    if (hung.load()) {
        FAIL("stress hung at " << consumed.load() << "/" << kN
                               << " cap=" << q.capacity());
    }
    REQUIRE_FALSE(check_error.load());
    REQUIRE(consumed.load() == kN);
    for (std::size_t i = 0; i < kN; ++i) {
        INFO("missing element " << (i + 1));
        REQUIRE(seen[i].load());
    }
    // Capacity should have grown significantly
    REQUIRE(q.capacity() >= 512);
}

// ============================================================================
// SECTION 8: UnboundedQueue 析构释放 (Q12)
// ============================================================================

/// @test [queue][unbounded][unit] Q12: 析构释放所有 buffer 包括 garbage (ASan 验证)
TEST_CASE("UnboundedQueue: destructor releases all buffers including garbage",
          "[queue][unbounded][unit]")
{
    // Force multiple resizes to accumulate garbage buffers
    for (int trial = 0; trial < 10; ++trial) {
        tfl::UnboundedQueue<int*> q(4);
        // Push enough to cause multiple resize steps: 4 -> 8 -> 16 -> 32 -> ...
        for (std::uintptr_t i = 1; i <= 1000; ++i) {
            q.push(reinterpret_cast<int*>(i));
        }
        // Steal some to leave items in old buffers referenced
        for (int i = 0; i < 500; ++i) {
            q.steal();
        }
        // Destructor should release current buffer + all garbage buffers
    }
    SUCCEED("ASan must not report leaks above");
}

// ============================================================================
// SECTION 9: UnboundedQueue static_assert (Q13)
// ============================================================================

/// @test [queue][unbounded][compile] 编译期约束验证
TEST_CASE("UnboundedQueue: static assertions", "[queue][unbounded][compile]") {
    STATIC_REQUIRE(std::is_pointer_v<typename tfl::UnboundedQueue<int*>::value_type>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<tfl::UnboundedQueue<int*>>);
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<tfl::UnboundedQueue<int*>>);
    STATIC_REQUIRE(std::is_nothrow_destructible_v<tfl::UnboundedQueue<int*>>);
}

// ============================================================================
// SECTION 10: 性质式 fuzz 测试 (Q5, Q6)
// ============================================================================

/// @test [queue][bounded][property][st] Q5+Q6: 随机 push/pop 序列与 reference stack 对照
/// @details 生成随机操作序列，对 BoundedQueue 执行并与 std::stack 对照 LIFO 语义
TEST_CASE("BoundedQueue: random push/pop sequence matches std::stack",
          "[queue][bounded][property][st]")
{
    auto seed = GENERATE(take(5, random(0u, UINT32_MAX)));
    INFO("seed = " << seed);
    std::mt19937 rng(seed);

    tfl::BoundedQueue<int*, 256> q;
    std::vector<std::uintptr_t> ref;  // reference LIFO stack
    std::uintptr_t next = 1;

    for (int step = 0; step < 5000; ++step) {
        auto op = std::uniform_int_distribution<int>(0, 99)(rng);
        if (op < 60 && ref.size() < 256) {
            // Push
            q.push(reinterpret_cast<int*>(next), [] { FAIL("unexpected overflow"); });
            ref.push_back(next);
            ++next;
        }
        else {
            // Pop (owner LIFO)
            auto got = q.pop();
            std::uintptr_t expected = ref.empty() ? 0 : ref.back();
            if (!ref.empty()) ref.pop_back();
            INFO("step=" << step << " got=" << reinterpret_cast<std::uintptr_t>(got)
                         << " expected=" << expected);
            REQUIRE(reinterpret_cast<std::uintptr_t>(got) == expected);
        }
    }
}

/// @test [queue][unbounded][property][st] Q5+Q6: 随机 push/steal 序列与 reference 对照 (FIFO, resize path)
/// @details UnboundedQueue 为 steal-only：用 FIFO 队列（front 出队）作为对照。
TEST_CASE("UnboundedQueue: random push/steal sequence matches reference (FIFO)",
          "[queue][unbounded][property][st]")
{
    auto seed = GENERATE(take(5, random(0u, UINT32_MAX)));
    INFO("seed = " << seed);
    std::mt19937 rng(seed);

    tfl::UnboundedQueue<int*> q(8);
    std::deque<std::uintptr_t> ref;  // reference FIFO queue
    std::uintptr_t next = 1;

    for (int step = 0; step < 5000; ++step) {
        auto op = std::uniform_int_distribution<int>(0, 99)(rng);
        if (op < 70) {
            // Push (unbounded)
            q.push(reinterpret_cast<int*>(next));
            ref.push_back(next);
            ++next;
        }
        else {
            // Steal (FIFO: 从队首出)
            auto got = q.steal();
            std::uintptr_t expected = ref.empty() ? 0 : ref.front();
            if (!ref.empty()) ref.pop_front();
            INFO("step=" << step);
            REQUIRE(reinterpret_cast<std::uintptr_t>(got) == expected);
        }
    }
}

/// @test [queue][bounded][property][st] Q6: 单线程连续 push/pop 不重复消费
/// @details Simple property: push then pop all, each value seen exactly once
TEST_CASE("BoundedQueue: push-pop cycle — each value consumed exactly once",
          "[queue][bounded][property][st]")
{
    auto seed = GENERATE(take(5, random(0u, UINT32_MAX)));
    INFO("seed = " << seed);
    std::mt19937 rng(seed);

    constexpr std::size_t kCap = 128;
    tfl::BoundedQueue<int*, kCap> q;

    for (int trial = 0; trial < 20; ++trial) {
        auto n = std::uniform_int_distribution<std::size_t>(1, kCap)(rng);
        std::vector<bool> seen(n + 1, false);

                // Push n items
        for (std::size_t i = 1; i <= n; ++i) {
            REQUIRE(q.try_push(reinterpret_cast<int*>(i)));
        }

                // Pop n items, verify each seen exactly once
        for (std::size_t i = 0; i < n; ++i) {
            auto got = q.pop();
            REQUIRE(got != nullptr);
            auto v = reinterpret_cast<std::uintptr_t>(got);
            REQUIRE(v >= 1);
            REQUIRE(v <= n);
            REQUIRE_FALSE(seen[v]);
            seen[v] = true;
        }
        REQUIRE(q.pop() == nullptr);

                // All must have been seen
        for (std::size_t i = 1; i <= n; ++i) {
            INFO("trial=" << trial << " value=" << i);
            REQUIRE(seen[i]);
        }
    }
}

// ============================================================================
// SECTION 11: 边界条件 —— capacity=2 (最小容量)
// ============================================================================

/// @test [queue][bounded][unit][st] Q1+Q2: capacity=2 最小容量边界
TEST_CASE("BoundedQueue: minimum capacity (2) edge cases", "[queue][bounded][unit][st]") {
    tfl::BoundedQueue<int*, 2> q;

    SECTION("push 2, pop 2") {
        REQUIRE(q.try_push(reinterpret_cast<int*>(1)));
        REQUIRE(q.try_push(reinterpret_cast<int*>(2)));
        REQUIRE_FALSE(q.try_push(reinterpret_cast<int*>(3)));  // full
        REQUIRE(q.pop() == reinterpret_cast<int*>(2));  // LIFO
        REQUIRE(q.pop() == reinterpret_cast<int*>(1));
        REQUIRE(q.pop() == nullptr);  // empty
    }

    SECTION("push 1, steal 1") {
        REQUIRE(q.try_push(reinterpret_cast<int*>(42)));
        REQUIRE(q.steal() == reinterpret_cast<int*>(42));  // FIFO
        REQUIRE(q.steal() == nullptr);
    }

    SECTION("single element race: pop vs steal") {
        REQUIRE(q.try_push(reinterpret_cast<int*>(99)));
        // Either pop or steal should get the element
        std::atomic<bool> pop_got{ false };
        std::atomic<bool> steal_got{ false };
        std::atomic<int> value_from_pop{ 0 };
        std::atomic<int> value_from_steal{ 0 };

        std::thread stealer([&] {
            auto p = q.steal();
            if (p) {
                steal_got.store(true);
                value_from_steal.store(static_cast<int>(reinterpret_cast<std::uintptr_t>(p)));
            }
        });

        auto p = q.pop();
        if (p) {
            pop_got.store(true);
            value_from_pop.store(static_cast<int>(reinterpret_cast<std::uintptr_t>(p)));
        }
        stealer.join();

                // Exactly one should have gotten it
        REQUIRE(pop_got.load() != steal_got.load());
        if (pop_got.load()) REQUIRE(value_from_pop.load() == 99);
        if (steal_got.load()) REQUIRE(value_from_steal.load() == 99);
        REQUIRE(q.empty());
    }
}

// ============================================================================
// 覆盖矩阵
// ============================================================================
//   Q1  ✓ "BoundedQueue: single-thread owner LIFO and capacity bounds"
//         SECTION "full queue try_push returns false"
//         "BoundedQueue: push with on_full callback"
//         "BoundedQueue: minimum capacity (2) edge cases"
//   Q2  ✓ "BoundedQueue: single-thread owner LIFO and capacity bounds"
//         SECTION "push then pop observes LIFO order"
//         "BoundedQueue: minimum capacity (2) edge cases"
//   Q3  ✓ "BoundedQueue: single stealer FIFO order"
//   Q4  ✓ "BoundedQueue: single-thread owner LIFO and capacity bounds"
//         SECTION "push N (N < Cap) then size() == N"
//   Q5  ✓ "BoundedQueue: random push/pop sequence matches std::stack"
//         "UnboundedQueue: random push/steal sequence matches reference (FIFO)"
//   Q6  ✓ "BoundedQueue: stress — no duplicate, no loss"
//         "BoundedQueue: push-pop cycle — each value consumed exactly once"
//   Q7  ✓ "BoundedQueue: stress — no duplicate, no loss"
//   Q8  ✓ "UnboundedQueue: resize on overflow"
//         "UnboundedQueue: steal FIFO with resize"
//   Q9  ✓ "UnboundedQueue: concurrent resize stress — no duplicate, no loss"
//   Q10 ✓ stress tests run under TSan
//   Q11 ✓ stress tests run under ASan/UBSan
//   Q12 ✓ "BoundedQueue: destructor releases buffer"
//         "UnboundedQueue: destructor releases all buffers including garbage"
//   Q13 ✓ "BoundedQueue: static assertions on template parameters"
//         "UnboundedQueue: static assertions"
//   Q14 ⏸ P3 benchmark (separate CI job)
//   Q15 ⏸ P3 benchmark (separate CI job)
