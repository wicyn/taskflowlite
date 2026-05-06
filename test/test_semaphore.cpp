/// @file test_semaphore.cpp
/// @brief Semaphore 模块测试 —— 限流、序列化、事件信号、多配额、reset。
///
/// 覆盖接口：
///   - Semaphore(max_value, name?)             默认构造
///   - Semaphore(max_value, current_value, name?)  指定初值（可 0）
///   - Semaphore::value()                       当前剩余可用计数
///   - Semaphore::max_value()                   最大容量
///   - Semaphore::reset(max)                    重置（恢复满）
///   - Semaphore::reset(max, current)           重置（指定可用）
///   - Semaphore::name() / Semaphore::name(s)   命名
///   - Task::acquire(sem) / acquire(sem, count) 配置
///   - Task::release(sem) / release(sem, count) 配置
///
/// 关键不变量：
///   1. 任务并发数永不超过 sem.max_value()；
///   2. acquire 失败的任务被"停车"（不占 worker），release 后唤醒；
///   3. 等待队列非空时 reset() 抛异常。

#include "test_common.hpp"

using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: 基础属性 —— value / max_value / name
// ============================================================================

/// @test [semaphore][basic] 默认构造的容量与可用计数关系。
TEST_CASE("Semaphore: 基础属性", "[semaphore][basic]") {
    SECTION("初值 = 容量") {
        tfl::Semaphore sem{3};
        REQUIRE(sem.max_value() == 3);
        REQUIRE(sem.value() == 3);
        REQUIRE(sem.name().empty());
    }

    SECTION("显式指定 current_value") {
        tfl::Semaphore sem{5, 2};
        REQUIRE(sem.max_value() == 5);
        REQUIRE(sem.value() == 2);
    }

    SECTION("命名信号量") {
        tfl::Semaphore sem{1, "io_lock"};
        REQUIRE(sem.name() == "io_lock");
    }

    SECTION("current_value 自动裁剪到 max_value 上限") {
        tfl::Semaphore sem{3, 10};  // 写 10 > 3，自动裁到 3
        REQUIRE(sem.max_value() == 3);
        REQUIRE(sem.value() == 3);
    }
}

// ============================================================================
// SECTION 2: 限流 —— 并发上限
// ============================================================================

/// @test [semaphore][throttle] 容量 K 时，最大并发不超过 K。
/// @details 启动 N 个任务 (N >> K)，每个任务 acquire+release 信号量。
///          worker 里不调 REQUIRE —— 用原子 max 跟踪历史峰值，主线程断言。
TEST_CASE("Semaphore: 并发上限限流", "[semaphore][throttle]") {
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
    REQUIRE(sem.value() == K);  // 全部释放后回到满状态
}

/// @test [semaphore][serialize] 容量 1 = 强制串行化。
TEST_CASE("Semaphore: 容量 1 实现强制串行", "[semaphore][serialize]") {
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
// SECTION 3: 事件信号 —— 初值 0 的 Semaphore
// ============================================================================

/// @test [semaphore][event] 初值 0 的信号量充当"事件"，消费者等到 release 才解封。
TEST_CASE("Semaphore: 事件信号模式 (初值 0)", "[semaphore][event]") {
    TestEnv env(4);
    tfl::Semaphore event_sem{3, 0};   // 容量 3，初始可用 0

    std::atomic<int> consumed{0};

    tfl::Flow flow;

    // 3 个消费者：各 acquire 一份配额（初始没有，全被挂起）
    for (int i = 0; i < 3; ++i) {
        flow.emplace([&] { consumed.fetch_add(1); }).acquire(event_sem);
    }

    // 1 个生产者：先睡一会再 release 3 份
    auto producer = flow.emplace([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });
    producer.release(event_sem, 3);

    // 注意：消费者和生产者之间没有 precede 边 —— 全靠 sem 同步
    env.executor.deferred_async(flow).start().wait();

    REQUIRE(consumed.load() == 3);
}

// ============================================================================
// SECTION 4: 多配额 —— 单任务占多份资源
// ============================================================================

/// @test [semaphore][multi-count] heavy 任务占 N 份配额时，不能与小任务并发。
TEST_CASE("Semaphore: 多配额 acquire", "[semaphore][multi-count]") {
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
// SECTION 5: reset
// ============================================================================

/// @test [semaphore][reset] 等待队列空时 reset 改容量。
TEST_CASE("Semaphore: reset 调整容量", "[semaphore][reset]") {
    tfl::Semaphore sem{2};

    SECTION("reset(max) 恢复到满") {
        sem.reset(5);
        REQUIRE(sem.max_value() == 5);
        REQUIRE(sem.value() == 5);
    }

    SECTION("reset(max, current) 显式指定可用") {
        sem.reset(5, 2);
        REQUIRE(sem.max_value() == 5);
        REQUIRE(sem.value() == 2);
    }
}

// ============================================================================
// SECTION 6: 命名 setter
// ============================================================================

/// @test [semaphore][name] 运行时改名。
TEST_CASE("Semaphore: name setter", "[semaphore][name]") {
    tfl::Semaphore sem{1};
    REQUIRE(sem.name().empty());
    sem.name("db_pool");
    REQUIRE(sem.name() == "db_pool");
}
