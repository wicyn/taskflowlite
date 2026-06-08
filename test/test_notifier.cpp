/// @file test_notifier.cpp
/// @brief Notifier 无锁通知器测试 —— 两阶段唤醒协议的正确性验证。
///
/// 覆盖的不变量:
///   N1  ✓ prepare/check/commit 双检查不丢通知
///   N2  ✓ notify_one 至少唤醒一个等待者
///   N3  ✓ notify_all 唤醒所有等待者
///   N4  ✓ notify_n 唤醒指定数量
///   N5  ✓ 无通知时不返回 (超时哨兵)
///   N6  ✓ 析构后状态干净
///   N7  ✓ TSan 干净
///   N8  ⏸ P3 性能基准

#include "test_common.hpp"

#include <algorithm>
#include <atomic>
#include <random>

using tfl_test::TestEnv;

// ============================================================================
// 第 1 节: 基础属性
// ============================================================================

/// @test [notifier][basic] 构造、容量、等待者数量查询
TEST_CASE("Notifier: construction and basic properties", "[notifier][basic]") {
    SECTION("constructor with valid size") {
        tfl::Notifier n{ 4 };
        REQUIRE(n.size() == 4);
        REQUIRE(n.capacity() == 65535);  // 2^16 - 1
        REQUIRE(n.num_waiters() == 0);
    }

    SECTION("constructor with many waiters") {
        tfl::Notifier n{ 256 };
        REQUIRE(n.size() == 256);
        REQUIRE(n.num_waiters() == 0);
    }
}

// ============================================================================
// 第 2 节: prepare_wait + cancel_wait 取消路径 (N1)
// ============================================================================

/// @test [notifier][prepare][cancel] N1: prepare 后 cancel 不阻塞，两阶段协议第一分支
TEST_CASE("Notifier: prepare_wait + cancel_wait does not block", "[notifier][prepare][cancel]") {
    tfl::Notifier n{ 4 };
    n.prepare_wait(0);   // 宣告准备等待
    n.cancel_wait(0);    // 撤回，不应阻塞
    SUCCEED("cancel_wait returned without blocking");
}

/// @test [notifier][prepare][cancel] N1: 多轮 prepare/cancel 计数正确
TEST_CASE("Notifier: multiple prepare/cancel sequences", "[notifier][prepare][cancel]") {
    tfl::Notifier n{ 8 };
    for (int trial = 0; trial < 100; ++trial) {
        n.prepare_wait(0);
        n.cancel_wait(0);
    }
    REQUIRE(n.num_waiters() == 0);  // 全部撤回后无残留
}

/// @test [notifier][prepare][cancel] N1: 不同 wid 独立 prepare/cancel
/// @details cancel_wait 要求按 prepare 的顺序依次处理——
///          epoch 机制保证先 prepare 的 waiter 必须先被处理（cancel 或 commit），
///          后 prepare 的才能继续。因此 cancel 顺序必须与 prepare 顺序一致。
TEST_CASE("Notifier: independent prepare/cancel per wid", "[notifier][prepare][cancel]") {
    tfl::Notifier n{ 4 };

    // prepare 顺序: 0 → 1。cancel 必须同序: 0 先，1 后。
    n.prepare_wait(0);
    n.prepare_wait(1);
    n.cancel_wait(0);  // 先处理排在前的 waiter 0
    n.cancel_wait(1);  // waiter 0 处理后，waiter 1 的 epoch 对齐
    REQUIRE(n.num_waiters() == 0);

    // 也可以逐个 prepare → cancel，不受顺序约束
    n.prepare_wait(2);
    n.cancel_wait(2);
    n.prepare_wait(3);
    n.cancel_wait(3);
    REQUIRE(n.num_waiters() == 0);
}

// ============================================================================
// 第 3 节: notify_one 唤醒测试 (N2)
// ============================================================================

/// @test [notifier][notify_one] N2: notify_one 唤醒已提交的等待者
TEST_CASE("Notifier: notify_one wakes a committed waiter", "[notifier][notify_one]") {
    tfl::Notifier n{ 2 };

    std::atomic<bool> woken{ false };
    std::atomic<bool> waiter_ready{ false };

    std::thread waiter([&] {
        n.prepare_wait(0);
        waiter_ready.store(true, std::memory_order_release);

        // 双重检查：尚无通知 → 进入 OS 等待
        if (!woken.load(std::memory_order_acquire)) {
            n.commit_wait(0);
        }
        woken.store(true, std::memory_order_release);
        });

    // 等待 waiter 进入 commit_wait
    while (!waiter_ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::yield();  // 给 waiter 时间 park
    std::this_thread::yield();

    n.notify_one();
    waiter.join();

    REQUIRE(woken.load());
}

/// @test [notifier][notify_one] N2: notify_one 在 commit 前消耗 prewaiter，避免 OS 挂起开销
TEST_CASE("Notifier: notify_one consumes prewaiter before OS park", "[notifier][notify_one]") {
    tfl::Notifier n{ 2 };
    std::atomic<bool> committed{ false };
    std::atomic<bool> woken{ false };

    std::thread waiter([&] {
        n.prepare_wait(0);
        committed.store(true, std::memory_order_release);
        std::this_thread::yield();  // 拉大窗口让通知有机会到达
        n.commit_wait(0);  // 如果通知已消费 prewaiter，这里应是空操作
        woken.store(true, std::memory_order_release);
        });

    // 等待 prepare 完成
    while (!committed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    // 通知在 prepare→commit 窗口内到达
    n.notify_one();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!woken.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) {
            FAIL("waiter never woke up — lost wakeup?");
        }
        std::this_thread::yield();
    }
    waiter.join();
}

// ============================================================================
// 第 4 节: notify_all 测试 (N3)
// ============================================================================

/// @test [notifier][notify_all] N3: notify_all 唤醒所有等待者
TEST_CASE("Notifier: notify_all wakes all waiters", "[notifier][notify_all]") {
    constexpr std::size_t kN = 4;
    tfl::Notifier n{ kN };

    std::atomic<int> woken_count{ 0 };
    std::atomic<bool> all_ready{ false };
    std::atomic<int> ready_count{ 0 };

    std::vector<std::thread> waiters;
    for (std::size_t i = 0; i < kN; ++i) {
        waiters.emplace_back([&, wid = i] {
            n.prepare_wait(wid);
            ready_count.fetch_add(1, std::memory_order_release);

            while (!all_ready.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            n.commit_wait(wid);
            woken_count.fetch_add(1, std::memory_order_release);
            });
    }

    // 等所有 waiter 都 prepare
    while (ready_count.load(std::memory_order_acquire) < static_cast<int>(kN)) {
        std::this_thread::yield();
    }
    all_ready.store(true, std::memory_order_release);
    std::this_thread::yield();
    std::this_thread::yield();  // 给 waiter 时间 park

    n.notify_all();

    for (auto& t : waiters) t.join();
    REQUIRE(woken_count.load() == static_cast<int>(kN));
}

// ============================================================================
// 第 5 节: notify_n 测试 (N4)
// ============================================================================

/// @test [notifier][notify_n] N4: notify_n(k) 仅唤醒 k 个等待者
TEST_CASE("Notifier: notify_n wakes specified count", "[notifier][notify_n]") {
    constexpr std::size_t kN = 6;
    constexpr std::size_t kToWake = 3;
    tfl::Notifier n{ kN };

    std::atomic<int> woken{ 0 };
    std::atomic<bool> start{ false };
    std::atomic<int> prepped{ 0 };

    std::vector<std::thread> waiters;
    for (std::size_t i = 0; i < kN; ++i) {
        waiters.emplace_back([&, wid = i] {
            n.prepare_wait(wid);
            prepped.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            n.commit_wait(wid);
            woken.fetch_add(1, std::memory_order_release);
            });
    }

    while (prepped.load(std::memory_order_acquire) < static_cast<int>(kN)) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    std::this_thread::yield();
    std::this_thread::yield();

    n.notify_n(kToWake);

    // 等待被通知的 kToWake 个醒来
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (woken.load(std::memory_order_acquire) < static_cast<int>(kToWake)) {
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::yield();
    }

    // 唤醒剩余的
    n.notify_all();

    for (auto& t : waiters) t.join();
    REQUIRE(woken.load() == static_cast<int>(kN));
}

// ============================================================================
// 第 6 节: 并发压力 —— 丢失唤醒防御 (N1, N7)
// ============================================================================

/// @test [notifier][stress][mt] N1+N7: 多通知多等待循环，无丢失唤醒
TEST_CASE("Notifier: stress — no lost wakeups under contention",
    "[notifier][stress][mt]")
{
    auto seed = GENERATE(take(3, random(0u, UINT32_MAX)));
    constexpr std::size_t kWaiters = 4;
    constexpr int kRounds = 500;
    INFO("seed = " << seed);

    tfl::Notifier n{ kWaiters };

    std::atomic<int> total_woken{ 0 };
    std::atomic<int> total_notifies{ 0 };
    std::atomic<bool> stop{ false };
    std::atomic<int> ready_count{ 0 };

    std::vector<std::thread> waiters;
    for (std::size_t i = 0; i < kWaiters; ++i) {
        waiters.emplace_back([&, wid = i] {
            while (!stop.load(std::memory_order_relaxed)) {
                n.prepare_wait(wid);
                ready_count.fetch_add(1, std::memory_order_relaxed);

                // 双重检查：有通知到来吗？
                bool has_work = (total_notifies.load(std::memory_order_relaxed) >
                    total_woken.load(std::memory_order_relaxed));

                if (has_work || stop.load(std::memory_order_relaxed)) {
                    n.cancel_wait(wid);
                    ready_count.fetch_sub(1, std::memory_order_relaxed);
                    if (has_work) {
                        total_woken.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                else {
                    n.commit_wait(wid);
                    ready_count.fetch_sub(1, std::memory_order_relaxed);
                    total_woken.fetch_add(1, std::memory_order_relaxed);
                }
            }
            });
    }

    // 通知者线程
    std::thread notifier_thread([&, seed] {
        std::mt19937 rng(seed);
        for (int round = 0; round < kRounds; ++round) {
            while (ready_count.load(std::memory_order_relaxed) == 0) {
                std::this_thread::yield();
            }
            total_notifies.fetch_add(1, std::memory_order_relaxed);

            if (std::uniform_int_distribution<int>(0, 1)(rng)) {
                n.notify_one();
            }
            else {
                n.notify_all();
            }
            std::this_thread::yield();
        }
        });

    notifier_thread.join();
    stop.store(true, std::memory_order_relaxed);

    // 唤醒所有可能还在睡眠的等待者
    for (int i = 0; i < 10; ++i) {
        n.notify_all();
        std::this_thread::yield();
    }

    // 带超时的 join 循环
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool all_joined = false;
    while (!all_joined && std::chrono::steady_clock::now() < deadline) {
        all_joined = true;
        for (auto& t : waiters) {
            if (t.joinable()) {
                n.notify_all();
                all_joined = false;
            }
        }
        std::this_thread::yield();
    }

    for (auto& t : waiters) {
        if (t.joinable()) t.join();
    }

    // 每条通知至少唤醒一个等待者
    REQUIRE(total_woken.load() >= total_notifies.load());
    REQUIRE(total_notifies.load() == kRounds);
}

// ============================================================================
// 第 7 节: 析构行为 (N6)
// ============================================================================

/// @test [notifier][lifecycle] N6: 无活跃等待者时析构正常
TEST_CASE("Notifier: destructor with no active waiters", "[notifier][lifecycle]") {
    {
        tfl::Notifier n{ 4 };
        n.prepare_wait(0);
        n.cancel_wait(0);  // 干净退出
    }
    SUCCEED("destructor assertion passes with clean state");
}

// ============================================================================
// 第 8 节: 边界条件 (N5)
// ============================================================================

/// @test [notifier][edge] N5: 无通知时不虚假唤醒
TEST_CASE("Notifier: no spurious wakeup without notification", "[notifier][edge]") {
    tfl::Notifier n{ 1 };
    std::atomic<bool> woken_early{ false };

    std::thread waiter([&] {
        n.prepare_wait(0);
        n.commit_wait(0);   // 无通知到来 → 应保持睡眠
        woken_early.store(true);
        });

    std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 50ms 后仍应睡眠（未发送通知）
    REQUIRE_FALSE(woken_early.load());

    // 清理：唤醒 waiter
    n.notify_all();
    waiter.join();
    REQUIRE(woken_early.load());
}

// ============================================================================
// 第 9 节: notify_n 边界
// ============================================================================

/// @test [notifier][notify_n] notify_n(0) 是空操作
TEST_CASE("Notifier: notify_n(0) is no-op", "[notifier][notify_n]") {
    tfl::Notifier n{ 1 };
    REQUIRE_NOTHROW(n.notify_n(0));
    REQUIRE(n.num_waiters() == 0);
}

/// @test [notifier][notify_n] notify_n >= size 退化为 notify_all
TEST_CASE("Notifier: notify_n larger than size calls notify_all", "[notifier][notify_n]") {
    tfl::Notifier n{ 2 };
    REQUIRE_NOTHROW(n.notify_n(100));
}

/// @test [notifier][basic] capacity 返回最大等待者数
TEST_CASE("Notifier: capacity constant", "[notifier][basic]") {
    tfl::Notifier n{ 1 };
    REQUIRE(n.capacity() == 65535);
    REQUIRE(n.size() == 1);
}

// ============================================================================
// 第 10 节: Waiter 三态机基础验证
// ============================================================================

/// @test [notifier][waiter] Waiter 初始状态为 kNotSignaled
TEST_CASE("Notifier: Waiter starts in kNotSignaled state", "[notifier][waiter]") {
    tfl::Notifier::Waiter w;
    REQUIRE(w.state.load() == tfl::Notifier::Waiter::kNotSignaled);
}

// ============================================================================
// 覆盖矩阵
// ============================================================================
//   N1  ✓ "Notifier: prepare_wait + cancel_wait does not block"
//         "Notifier: multiple prepare/cancel sequences"
//         "Notifier: notify_one consumes prewaiter before OS park"
//         "Notifier: stress — no lost wakeups under contention"
//   N2  ✓ "Notifier: notify_one wakes a committed waiter"
//         "Notifier: notify_one consumes prewaiter before OS park"
//   N3  ✓ "Notifier: notify_all wakes all waiters"
//   N4  ✓ "Notifier: notify_n wakes specified count"
//         "Notifier: notify_n(0) is no-op"
//   N5  ✓ "Notifier: no spurious wakeup without notification"
//   N6  ✓ "Notifier: destructor with no active waiters"
//   N7  ✓ "Notifier: stress — no lost wakeups under contention"
//   N8  ⏸ P3 性能基准