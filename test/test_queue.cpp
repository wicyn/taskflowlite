/// @file test_queue.cpp
/// @brief 当前 BoundedQueue 的容量、LIFO/FIFO、回调、并发与性质测试。
/// SharedWorkStack 的非拥有 Work 链测试见 test_shared_work_stack.cpp。
#include "test_common.hpp"
#include <array>
#include <cstdint>
#include <random>
#include <type_traits>

namespace {
// Test helper only: the production API reports overflow through push's callback.
template <typename Q, typename P>
bool push_accepted(Q& queue, P pointer) {
    bool accepted = true;
    queue.push(pointer, [&](auto) noexcept { accepted = false; });
    return accepted;
}
} // namespace

TEST_CASE("BoundedQueue: zero batch and throwing overflow preserve published prefix",
          "[queue][bounded][exception]") {
    tfl::BoundedQueue<int*, 2> queue;
    int values[] = {1, 2, 3};
    int* pointers[] = {&values[0], &values[1], &values[2]};
    int** empty = nullptr;
    queue.push(empty, 0, [](auto, std::size_t) { FAIL("empty batch must not invoke callback"); });
    REQUIRE(queue.empty());
    int** rejected = nullptr;
    std::size_t remaining = 0;
    REQUIRE_THROWS_AS(queue.push(pointers, 3, [&](auto first, std::size_t n) {
        rejected = first;
        remaining = n;
        throw std::runtime_error("overflow");
    }), std::runtime_error);
    REQUIRE(rejected == pointers + 2);
    REQUIRE(remaining == 1);
    REQUIRE(queue.size() == 2);
    REQUIRE(queue.steal() == pointers[0]);
    REQUIRE(queue.pop() == pointers[1]);
    REQUIRE(queue.empty());
    // Retry only the unpublished suffix, never the full original batch.
    queue.push(rejected, remaining, [](auto, std::size_t) { FAIL("unexpected overflow"); });
    REQUIRE(queue.pop() == pointers[2]);
}

TEST_CASE("BoundedQueue: repeated ring reuse with concurrent thieves", "[queue][bounded][stress]") {
    constexpr std::size_t count = 20000;
    tfl::BoundedQueue<int*, 8> queue;
    std::vector<int> values(count);
    std::vector<std::atomic<unsigned>> seen(count);
    std::atomic<std::size_t> consumed{0};
    std::atomic<bool> error{false};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    const auto consume = [&](int* value) {
        const auto index = static_cast<std::size_t>(*value - 1);
        if (index >= count || seen[index].fetch_add(1) != 0) error = true;
        ++consumed;
    };
    std::vector<std::jthread> thieves;
    for (int i = 0; i < 3; ++i) thieves.emplace_back([&] {
        while (consumed.load() < count && !error.load()) {
            if (auto* value = queue.steal()) consume(value);
            else {
                if (std::chrono::steady_clock::now() >= deadline) { error = true; break; }
                std::this_thread::yield();
            }
        }
    });
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = static_cast<int>(i + 1);
        queue.push(&values[i], consume); // Caller handles a rejected element exactly once.
        if (i % 3 == 0) if (auto* value = queue.pop()) consume(value);
    }
    while (consumed.load() < count && !error.load()) {
        if (auto* value = queue.pop()) consume(value);
        if (std::chrono::steady_clock::now() >= deadline) error = true;
    }
    thieves.clear();
    REQUIRE_FALSE(error.load());
    REQUIRE(consumed.load() == count);
    for (const auto& hits : seen) REQUIRE(hits.load() == 1);
    REQUIRE(queue.empty());
}

TEMPLATE_TEST_CASE_SIG(
    "BoundedQueue: single-thread owner LIFO and capacity bounds",
    "[queue][bounded][unit][st]",
    ((std::size_t Cap), Cap),
    2, 4, 8, 64, 1024)
{
    tfl::BoundedQueue<int*, Cap> q;

    SECTION("empty queue pop returns nullptr") {
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
            REQUIRE(push_accepted(q, reinterpret_cast<int*>(i)));
        }
        REQUIRE(q.size() == n);
    }

    SECTION("push then pop observes LIFO order") {
        for (std::size_t i = 1; i <= Cap; ++i) {
            REQUIRE(push_accepted(q, reinterpret_cast<int*>(i)));
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

    SECTION("full queue invokes overflow callback") {
        for (std::size_t i = 1; i <= Cap; ++i) {
            REQUIRE(push_accepted(q, reinterpret_cast<int*>(i)));
        }
        REQUIRE(q.size() == Cap);
        REQUIRE_FALSE(push_accepted(q, reinterpret_cast<int*>(999)));
        REQUIRE(q.size() == Cap);  // size unchanged after failed push
    }

    SECTION("capacity() == Cap") {
        REQUIRE(q.capacity() == Cap);
    }
}

TEST_CASE("BoundedQueue: push with on_full callback", "[queue][bounded][unit][st]") {
    tfl::BoundedQueue<int*, 8> q;

    SECTION("on_full called when queue is full") {
        // Fill queue
        for (int i = 0; i < 8; ++i) {
            q.push(reinterpret_cast<int*>(static_cast<std::uintptr_t>(i + 1)),
                   [](int*) { FAIL("should not overflow yet"); });
        }
        REQUIRE(q.size() == 8);

        bool overflow_called = false;
        q.push(reinterpret_cast<int*>(static_cast<std::uintptr_t>(999)),
               [&](int* rejected) { overflow_called = rejected == reinterpret_cast<int*>(999); });
        REQUIRE(overflow_called);
        REQUIRE(q.size() == 8);  // element not pushed
    }

    SECTION("on_full NOT called when space available") {
        for (int i = 0; i < 4; ++i) {
            q.push(reinterpret_cast<int*>(static_cast<std::uintptr_t>(i + 1)),
                   [](auto) { FAIL("unexpected overflow"); });
        }
        REQUIRE(q.size() == 4);
    }
}

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
        REQUIRE(overflow_count == 8);
        REQUIRE(overflow_first == items.begin() + 16);
        REQUIRE(static_cast<std::size_t>(q.size()) + overflow_count == 24);
    }
}

TEST_CASE("BoundedQueue: single stealer FIFO order", "[queue][bounded][unit][st]") {
    tfl::BoundedQueue<int*, 64> q;

    SECTION("steal observes FIFO order") {
        constexpr std::size_t N = 50;
        for (std::size_t i = 1; i <= N; ++i) {
            REQUIRE(push_accepted(q, reinterpret_cast<int*>(i)));
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
            error.store(true, std::memory_order_relaxed);
            return;
        }
        consumed.fetch_add(1, std::memory_order_relaxed);
    };

            // ── Phase 1：单线程填满 + 验证"满"（尚无 stealer，断言合法且安全）──
    for (std::uintptr_t i = 1; i <= kN; ++i) {
        REQUIRE(push_accepted(q, reinterpret_cast<int*>(i)));
    }
    REQUIRE_FALSE(push_accepted(q, reinterpret_cast<int*>(kN + 1)));  // 此刻确实满

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

TEST_CASE("BoundedQueue: destruction does not own pointees", "[queue][bounded][unit]") {
    int value = 42;
    {
        tfl::BoundedQueue<int*, 2> q;
        q.push(&value, [](int*) { FAIL("unexpected overflow"); });
    }
    REQUIRE(value == 42);
}

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
            q.push(reinterpret_cast<int*>(next), [](auto) { FAIL("unexpected overflow"); });
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
            REQUIRE(push_accepted(q, reinterpret_cast<int*>(i)));
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

TEST_CASE("BoundedQueue: minimum capacity (2) edge cases", "[queue][bounded][unit][st]") {
    tfl::BoundedQueue<int*, 2> q;

    SECTION("push 2, pop 2") {
        REQUIRE(push_accepted(q, reinterpret_cast<int*>(1)));
        REQUIRE(push_accepted(q, reinterpret_cast<int*>(2)));
        REQUIRE_FALSE(push_accepted(q, reinterpret_cast<int*>(3)));  // full
        REQUIRE(q.pop() == reinterpret_cast<int*>(2));  // LIFO
        REQUIRE(q.pop() == reinterpret_cast<int*>(1));
        REQUIRE(q.pop() == nullptr);  // empty
    }

    SECTION("push 1, steal 1") {
        REQUIRE(push_accepted(q, reinterpret_cast<int*>(42)));
        REQUIRE(q.steal() == reinterpret_cast<int*>(42));  // FIFO
        REQUIRE(q.steal() == nullptr);
    }

    SECTION("single element race: pop vs steal") {
        REQUIRE(push_accepted(q, reinterpret_cast<int*>(99)));
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
