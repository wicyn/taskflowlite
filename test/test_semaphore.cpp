/// @file test_semaphore.cpp
/// @brief 信号量模块测试 — 限流、序列化、事件通知、多计数、重置。
///
/// 覆盖的接口：
///   - Semaphore(max_value, name?)                 默认构造
///   - Semaphore(max_value, current_value, name?)  指定初始值（可为 0）
///   - Semaphore::value()                          当前剩余可用计数
///   - Semaphore::max_value()                      最大容量
///   - Semaphore::reset(max)                       重置（恢复至满）
///   - Semaphore::reset(max, current)              重置（指定可用数）
///   - Semaphore::name() / Semaphore::name(s)      命名
///   - Task::acquire(sem) / acquire(sem, count)    配置
///   - Task::release(sem) / release(sem, count)    配置
///
/// 核心不变式：
///   1. 任务并发数永远不超过 sem.max_value()；
///   2. 未能获取信号量的任务被"挂起"（不占用工作线程），释放后被唤醒；
///   3. 等待队列非空时，reset() 抛出异常。

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: 基本属性 — value / max_value / name
// ============================================================================

/// @test [semaphore][basic] 默认构造下容量与可用计数之间的关系。
TEST_CASE("Semaphore: Basic properties", "[semaphore][basic]") {
    /// @section initial-equals-capacity
    SECTION("initial value equals capacity") {
        tfl::Semaphore sem{3};
        REQUIRE(sem.max_value() == 3);
        REQUIRE(sem.value() == 3);
        REQUIRE(sem.name().empty());
    }

    /// @section explicit-current-value
    SECTION("explicit current_value") {
        tfl::Semaphore sem{5, 2};
        REQUIRE(sem.max_value() == 5);
        REQUIRE(sem.value() == 2);
    }

    /// @section named-semaphore
    SECTION("named semaphore") {
        tfl::Semaphore sem{1, "io_lock"};
        REQUIRE(sem.name() == "io_lock");
    }

    /// @section current-value-clamped
    SECTION("current_value auto-clamped to max_value") {
        tfl::Semaphore sem{3, 10};  // 写入 10 > 3，自动钳位至 3
        REQUIRE(sem.max_value() == 3);
        REQUIRE(sem.value() == 3);
    }
}

// ============================================================================
// SECTION 2: 限流 — 并发上限
// ============================================================================

/// @test [semaphore][throttle] 当容量为 K 时，最大并发数永远不超过 K。
/// @details 启动 N 个任务（N >> K），每个任务获取并释放信号量。
///          工作线程内部不调用 REQUIRE — 使用原子变量追踪历史峰值，由主线程断言。
TEST_CASE("Semaphore: Concurrency cap throttling", "[semaphore][throttle]") {
    TestEnv env(8);

    constexpr int K = 2;
    constexpr int N = 16;
    tfl::Semaphore sem{K};

    std::atomic<int> active{0};
    std::atomic<int> peak{0};

    tfl::Flow flow;
    for (int i = 0; i < N; ++i) {
        auto t = flow.emplace([&] {
            int now = active.fetch_add(1) + 1;
            // 更新历史峰值（CAS 循环）
            int prev = peak.load();
            while (now > prev && !peak.compare_exchange_weak(prev, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            active.fetch_sub(1);
        });
        t.acquire(sem).release(sem);
    }

    env.executor.deferred_async(flow).start().wait();

    REQUIRE(peak.load() <= K);
    REQUIRE(peak.load() > 0);
    REQUIRE(sem.value() == K);  // 全部释放后，恢复至满
}

/// @test [semaphore][serialize] 容量为 1 = 强制串行化。
TEST_CASE("Semaphore: Capacity 1 enforces serialization", "[semaphore][serialize]") {
    TestEnv env(4);
    tfl::Semaphore mutex_like{1};

    constexpr int N = 8;
    std::atomic<int> active{0};
    std::atomic<int> peak{0};

    tfl::Flow flow;
    for (int i = 0; i < N; ++i) {
        auto t = flow.emplace([&] {
            int now = active.fetch_add(1) + 1;
            int prev = peak.load();
            while (now > prev && !peak.compare_exchange_weak(prev, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            active.fetch_sub(1);
        });
        t.acquire(mutex_like).release(mutex_like);
    }

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(peak.load() == 1);  // 严格串行
}

// ============================================================================
// SECTION 3: 事件通知 — 初始值为 0 的信号量
// ============================================================================

/// @test [semaphore][event] 初始值为 0 的信号量扮演"事件"的角色；消费者阻塞直至 release 唤醒它们。
TEST_CASE("Semaphore: Event signal pattern (initial value 0)", "[semaphore][event]") {
    TestEnv env(4);
    tfl::Semaphore event_sem{3, 0};   // 容量 3，初始可用 0

    std::atomic<int> consumed{0};

    tfl::Flow flow;

    // 3 个消费者：各获取一个单位（初始无可用，全部挂起）
    for (int i = 0; i < 3; ++i) {
        flow.emplace([&] { consumed.fetch_add(1); }).acquire(event_sem);
    }

    // 1 个生产者：短暂休眠后释放 3 个单位
    auto producer = flow.emplace([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });
    producer.release(event_sem, 3);

    // 注意：消费者与生产者之间无 precede 边 — 同步完全依赖信号量
    env.executor.deferred_async(flow).start().wait();

    REQUIRE(consumed.load() == 3);
}

// ============================================================================
// SECTION 4: 多计数 — 单个任务占用多个配额
// ============================================================================

/// @test [semaphore][multi-count] 占用 N 个配额的重任务无法与小任务并发运行。
TEST_CASE("Semaphore: Multi-count acquire", "[semaphore][multi-count]") {
    TestEnv env(4);
    tfl::Semaphore pool{4};

    std::atomic<int> active_quota{0};
    std::atomic<int> peak_quota{0};

    auto track_enter = [&](int quota) {
        int now = active_quota.fetch_add(quota) + quota;
        int prev = peak_quota.load();
        while (now > prev && !peak_quota.compare_exchange_weak(prev, now)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        active_quota.fetch_sub(quota);
    };

    tfl::Flow flow;
    auto heavy = flow.emplace([&] { track_enter(3); });
    auto light = flow.emplace([&] { track_enter(1); });
    auto light2 = flow.emplace([&] { track_enter(1); });

    heavy.acquire(pool, 3).release(pool, 3);
    light.acquire(pool).release(pool);
    light2.acquire(pool).release(pool);

    env.executor.deferred_async(flow).start().wait();

    REQUIRE(peak_quota.load() <= 4);
    REQUIRE(pool.value() == 4);
}

// ============================================================================
// SECTION 5: reset 重置
// ============================================================================

/// @test [semaphore][reset] 等待队列为空时，reset 可更改容量。
TEST_CASE("Semaphore: reset adjusts capacity", "[semaphore][reset]") {
    tfl::Semaphore sem{2};

    /// @section reset-max-full
    SECTION("reset(max) restores to full") {
        sem.reset(5);
        REQUIRE(sem.max_value() == 5);
        REQUIRE(sem.value() == 5);
    }

    /// @section reset-max-current
    SECTION("reset(max, current) explicit available") {
        sem.reset(5, 2);
        REQUIRE(sem.max_value() == 5);
        REQUIRE(sem.value() == 2);
    }
}

// ============================================================================
// SECTION 6: 名称设置
// ============================================================================

/// @test [semaphore][name] 运行时重命名。
TEST_CASE("Semaphore: name setter", "[semaphore][name]") {
    tfl::Semaphore sem{1};
    REQUIRE(sem.name().empty());
    sem.name("db_pool");
    REQUIRE(sem.name() == "db_pool");
}

// ============================================================================
// SECTION 7: 并发 stress —— 多 Flow 竞争同一信号量 (A9 扩展)
// ============================================================================

/// @test [semaphore][stress][mt] 多个 Flow 同时竞争同一信号量
/// @details 4 个独立 Flow 各有 100 个任务，共享 2 个信号量配额。
///          验证全局峰值并发不超过 2，所有任务完成。
TEST_CASE("Semaphore: multi-Flow contention stress", "[semaphore][stress][mt]") {
    TestEnv env(8);
    tfl::Semaphore gate{2};

    constexpr int kFlows = 4;
    constexpr int kTasksPerFlow = 100;
    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    std::atomic<int> total_done{0};

    auto make_flow = [&]() {
        tfl::Flow f;
        for (int i = 0; i < kTasksPerFlow; ++i) {
            auto t = f.emplace([&] {
                int now = active.fetch_add(1) + 1;
                int prev = peak.load();
                while (now > prev && !peak.compare_exchange_weak(prev, now)) {}
                // Brief "work" to encourage interleaving
                for (volatile int k = 0; k < 50; ++k) {}
                active.fetch_sub(1);
                total_done.fetch_add(1, std::memory_order_relaxed);
            });
            t.acquire(gate).release(gate);
        }
        return f;
    };

    // Submit all flows concurrently
    std::vector<tfl::Flow> flows;
    for (int i = 0; i < kFlows; ++i) {
        flows.push_back(make_flow());
    }

    // Start all in parallel
    std::vector<tfl::DeferredAsyncTask> tasks;
    for (auto& f : flows) {
        tasks.push_back(env.executor.deferred_async(f));
    }
    for (auto& t : tasks) t.start();
    for (auto& t : tasks) t.wait();

    REQUIRE(total_done.load() == kFlows * kTasksPerFlow);
    REQUIRE(peak.load() <= 2);
    REQUIRE(peak.load() > 0);
    REQUIRE(gate.value() == 2);
}

/// @test [semaphore][stress][mt] 大量任务竞争 1 配额 = 严格串行化
/// @details 256 tasks, 1 permit → must serialize; verify no task overlaps.
TEST_CASE("Semaphore: heavy serialization with 1 permit", "[semaphore][stress][mt]") {
    TestEnv env(8);
    tfl::Semaphore mutex{1};

    constexpr int N = 256;
    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    std::atomic<int> violations{0};

    tfl::Flow flow;
    for (int i = 0; i < N; ++i) {
        auto t = flow.emplace([&] {
            int now = active.fetch_add(1) + 1;
            if (now > 1) violations.fetch_add(1);  // should never happen
            int prev = peak.load();
            while (now > prev && !peak.compare_exchange_weak(prev, now)) {}
            for (volatile int k = 0; k < 20; ++k) {}
            active.fetch_sub(1);
        });
        t.acquire(mutex).release(mutex);
    }

    env.executor.deferred_async(flow).start().wait();

    REQUIRE(violations.load() == 0);
    REQUIRE(peak.load() == 1);
    REQUIRE(mutex.value() == 1);
}

/// @test [semaphore][stress][mt] 多计数获取 + 释放大规模验证
/// @details 混合重任务(3 permits)和轻任务(1 permit)，pool=6。
///          验证配额和始终 ≤ 6 且所有任务完成。
TEST_CASE("Semaphore: mixed heavy/light tasks under pool cap", "[semaphore][stress][mt]") {
    TestEnv env(8);
    tfl::Semaphore pool{6};

    constexpr int kHeavy = 30;
    constexpr int kLight = 90;
    std::atomic<int> active_quota{0};
    std::atomic<int> peak_quota{0};
    std::atomic<int> done{0};

    tfl::Flow flow;

    // Heavy tasks (3 permits each)
    for (int i = 0; i < kHeavy; ++i) {
        auto t = flow.emplace([&] {
            int now = active_quota.fetch_add(3) + 3;
            int prev = peak_quota.load();
            while (now > prev && !peak_quota.compare_exchange_weak(prev, now)) {}
            for (volatile int k = 0; k < 30; ++k) {}
            active_quota.fetch_sub(3);
            done.fetch_add(1, std::memory_order_relaxed);
        });
        t.acquire(pool, 3).release(pool, 3);
    }

    // Light tasks (1 permit each)
    for (int i = 0; i < kLight; ++i) {
        auto t = flow.emplace([&] {
            int now = active_quota.fetch_add(1) + 1;
            int prev = peak_quota.load();
            while (now > prev && !peak_quota.compare_exchange_weak(prev, now)) {}
            for (volatile int k = 0; k < 10; ++k) {}
            active_quota.fetch_sub(1);
            done.fetch_add(1, std::memory_order_relaxed);
        });
        t.acquire(pool).release(pool);
    }

    env.executor.deferred_async(flow).start().wait();

    REQUIRE(done.load() == kHeavy + kLight);
    REQUIRE(peak_quota.load() <= 6);
    REQUIRE(pool.value() == 6);
}

// ============================================================================
// SECTION 8: 错误路径 —— reset with waiters / acquire > max
// ============================================================================

/// @test [semaphore][error] reset 在 waiters 非空时抛异常
/// @details Semaphore::reset 要求等待队列为空，否则抛 tfl::Exception。
///          用"初始值=0 的信号量 + 被 acquire 阻塞的任务"构造等待者，
///          验证 reset 抛异常后，通过 release 任务唤醒被阻塞的任务完成清理。
TEST_CASE("Semaphore: reset with pending waiters throws", "[semaphore][error]") {
    TestEnv env(2);
    tfl::Semaphore gate{1, 0};  // max=1, current=0 —— 初始无配额

    std::atomic<bool> waiter_ran{false};

    tfl::Flow flow;
    auto waiter = flow.emplace([&] {
        waiter_ran.store(true);  // acquire 成功后才会执行
    });
    waiter.acquire(gate);  // 阻塞：current=0，无法获取

    auto task = env.executor.deferred_async(flow);
    task.start();

    // 给调度器时间把任务放入信号量等待队列
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // 核心断言：等待队列非空时 reset 必须抛异常
    REQUIRE_THROWS_AS(gate.reset(2), tfl::Exception);
    REQUIRE_THROWS_AS(gate.reset(2, 1), tfl::Exception);

    // 清理：提交新 Flow 释放 1 配额以唤醒被阻塞的任务
    tfl::Flow release_flow;
    auto releaser = release_flow.emplace([] {});
    releaser.release(gate);  // 释放 1 配额 → 唤醒 waiter
    env.executor.deferred_async(release_flow).start().wait();

    // waiter 现在应已完成
    task.wait();
    REQUIRE(waiter_ran.load());
}

/// @test [semaphore][boundary] 初始值为 0 时 value() 与 max_value() 独立
TEST_CASE("Semaphore: zero initial value boundaries", "[semaphore][boundary]") {
    SECTION("value=0, max=5") {
        tfl::Semaphore sem{5, 0};
        REQUIRE(sem.max_value() == 5);
        REQUIRE(sem.value() == 0);
    }

    SECTION("value=5, max=5") {
        tfl::Semaphore sem{5, 5};
        REQUIRE(sem.value() == 5);
    }

    SECTION("value clipped to max") {
        tfl::Semaphore sem{3, 10};
        REQUIRE(sem.max_value() == 3);
        REQUIRE(sem.value() == 3);
    }
}

/// @test [semaphore][boundary] acquire(0) / release(0) 边界
/// @details 不占用配额的 acquire/release 不应影响可用配额
TEST_CASE("Semaphore: zero-count acquire/release", "[semaphore][boundary]") {
    TestEnv env(2);
    tfl::Semaphore sem{3};

    // A task that acquires 0 should pass immediately and not change value
    std::atomic<int> hit{0};
    tfl::Flow flow;
    auto t = flow.emplace([&] { hit.store(1); });
    t.acquire(sem, 0).release(sem, 0);

    env.executor.deferred_async(flow).start().wait();
    REQUIRE(hit.load() == 1);
    REQUIRE(sem.value() == 3);  // unchanged
}

// ============================================================================
// SECTION 9: 并发 reset 安全
// ============================================================================

/// @test [semaphore][concurrent] 并发 release 不丢 wakeup
/// @details 信号量初始值=0（事件模式），所有消费者阻塞在 acquire 上。
///          生产者一次性 release N 个配额后，全部消费者被唤醒执行。
///          关键：必须用 Semaphore{max, initial} 双参数构造——max 决定容量上限，
///          release 时 value 钳位到 max。单参数 Semaphore{0} 会使 max=0 导致
///          release 的配额全部被钳掉，消费者永远无法获取。
TEST_CASE("Semaphore: concurrent release wakeup stress", "[semaphore][concurrent][stress]") {
    TestEnv env(8);
    tfl::Semaphore sem{ 200, 0 };  // max=200, current=0 —— 事件模式

    constexpr int N = 200;
    std::atomic<int> consumers_done{ 0 };

    tfl::Flow flow;
    for (int i = 0; i < N; ++i) {
        flow.emplace([&] {
            consumers_done.fetch_add(1, std::memory_order_relaxed);
            }).acquire(sem);
    }

    // 生产者一次性释放 N 个配额，唤醒全部消费者
    auto producer = flow.emplace([] {});
    producer.release(sem, static_cast<std::size_t>(N));

    env.executor.deferred_async(flow).start().wait();

    REQUIRE(consumers_done.load() == N);
    REQUIRE(sem.value() == 0);  // 全部消费完毕
}
