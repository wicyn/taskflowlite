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
