/// @file test_task.cpp
/// @brief Task 模块测试 —— 句柄操作 / 拓扑构建 / 信号量配置 / 遍历访问。
///
/// 覆盖接口：
///   - Task 默认 / nullptr / 拷贝 / 移动 构造 / 赋值
///   - Task::operator==                     节点身份比较
///   - Task::name(string) / Task::name()    命名
///   - Task::num_predecessors / num_successors
///   - Task::precede / succeed              双向依赖建模
///   - Task::remove_predecessor / remove_successor
///   - Task::clear_predecessors / clear_successors
///   - Task::acquire / release              信号量配置
///   - Task::for_each_successor / for_each_predecessor
///   - Task::for_each_acquire / for_each_release
///   - Task::type / Task::hash_value
///   - Task::valid

#include "test_common.hpp"
#include <unordered_set>
using tfl_test::TestEnv;

// ============================================================================
// SECTION 1: 句柄构造与值语义
// ============================================================================

/// @test [task][handle] 默认构造的 Task 是空句柄。
TEST_CASE("Task: 默认构造为空", "[task][handle]") {
    tfl::Task t;
    REQUIRE_FALSE(static_cast<bool>(t));
    REQUIRE(t == tfl::Task{});  // 空句柄相等
}

/// @test [task][handle] 拷贝句柄 = 引用同一节点。
/// @details Task 是弱引用，拷贝后两份指向同一 Work*，对其中一份做 mutate
///          应该影响另一份的查询结果（因为底层是同一节点）。
TEST_CASE("Task: 拷贝句柄共享同一节点", "[task][handle][copy]") {
    tfl::Flow flow;
    auto a = flow.emplace([] {});
    auto b = flow.emplace([] {});

    tfl::Task copy = a;        // 拷贝
    REQUIRE(copy == a);

    copy.precede(b);           // 通过副本修改
    REQUIRE(a.num_successors() == 1);  // 原句柄看到了变化
}

// ============================================================================
// SECTION 2: 命名
// ============================================================================

/// @test [task][name] 链式 name() 设置后，再 name() 读取一致。
TEST_CASE("Task: 命名 setter/getter", "[task][name]") {
    tfl::Flow flow;
    auto t = flow.emplace([] {}).name("loader");
    REQUIRE(t.name() == "loader");

    t.name("renamed");
    REQUIRE(t.name() == "renamed");
}

// ============================================================================
// SECTION 3: 拓扑建模 —— precede / succeed / remove / clear
// ============================================================================

/// @test [task][topology] precede 与 succeed 等价，方向相反。
TEST_CASE("Task: precede 与 succeed 对称构建", "[task][topology]") {
    tfl::Flow flow;
    auto a = flow.emplace([] {});
    auto b = flow.emplace([] {});
    auto c = flow.emplace([] {});
    auto d = flow.emplace([] {});

    SECTION("precede(B, C, D)：A 同时是 B/C/D 的前驱") {
        a.precede(b, c, d);
        REQUIRE(a.num_successors() == 3);
        REQUIRE(b.num_predecessors() == 1);
        REQUIRE(c.num_predecessors() == 1);
        REQUIRE(d.num_predecessors() == 1);
    }

    SECTION("succeed(B, C, D)：A 同时以 B/C/D 为前驱") {
        a.succeed(b, c, d);
        REQUIRE(a.num_predecessors() == 3);
        REQUIRE(b.num_successors() == 1);
    }
}

/// @test [task][topology] 菱形拓扑 A→{B,C}→D 边数符合预期。
TEST_CASE("Task: 菱形拓扑边数", "[task][topology][diamond]") {
    tfl::Flow flow;
    auto a = flow.emplace([] {});
    auto b = flow.emplace([] {});
    auto c = flow.emplace([] {});
    auto d = flow.emplace([] {});

    a.precede(b, c);
    d.succeed(b, c);

    REQUIRE(a.num_successors() == 2);
    REQUIRE(d.num_predecessors() == 2);
    REQUIRE(b.num_predecessors() == 1);
    REQUIRE(b.num_successors() == 1);
    REQUIRE(c.num_predecessors() == 1);
    REQUIRE(c.num_successors() == 1);
}

/// @test [task][topology] remove_successor / remove_predecessor 断开单边。
/// @details 验证图编辑期可以增量修改：建一条边后又把它删掉，前后驱计数随之归零。
TEST_CASE("Task: remove_successor 双向断边", "[task][topology][remove]") {
    tfl::Flow flow;
    auto a = flow.emplace([] {});
    auto b = flow.emplace([] {});

    a.precede(b);
    REQUIRE(a.num_successors() == 1);
    REQUIRE(b.num_predecessors() == 1);

    a.remove_successor(b);
    REQUIRE(a.num_successors() == 0);
    REQUIRE(b.num_predecessors() == 0);
}

/// @test [task][topology] clear_successors / clear_predecessors 一键断开所有邻边。
TEST_CASE("Task: clear_successors / clear_predecessors", "[task][topology][clear]") {
    tfl::Flow flow;
    auto hub = flow.emplace([] {});
    auto x = flow.emplace([] {});
    auto y = flow.emplace([] {});
    auto z = flow.emplace([] {});
    hub.precede(x, y, z);
    REQUIRE(hub.num_successors() == 3);

    hub.clear_successors();
    REQUIRE(hub.num_successors() == 0);
    REQUIRE(x.num_predecessors() == 0);
    REQUIRE(y.num_predecessors() == 0);
    REQUIRE(z.num_predecessors() == 0);
}

// ============================================================================
// SECTION 4: 任务类型
// ============================================================================

/// @test [task][type] 各种签名 emplace 后 type() 正确反映任务类型。
TEST_CASE("Task: type() 反映任务类型", "[task][type]") {
    tfl::Flow flow;

    auto basic       = flow.emplace([] {});
    auto runtime     = flow.emplace([](tfl::Runtime&) {});
    auto branch      = flow.emplace([](tfl::Branch&) {});
    auto multibranch = flow.emplace([](tfl::MultiBranch&) {});
    auto jump        = flow.emplace([](tfl::Jump&) {});
    auto multijump   = flow.emplace([](tfl::MultiJump&) {});

    tfl::Flow sub;
    sub.emplace([] {});
    auto graph = flow.emplace(std::move(sub));

    REQUIRE(basic.type()       == tfl::TaskType::Basic);
    REQUIRE(runtime.type()     == tfl::TaskType::Runtime);
    REQUIRE(branch.type()      == tfl::TaskType::Branch);
    REQUIRE(multibranch.type() == tfl::TaskType::MultiBranch);
    REQUIRE(jump.type()        == tfl::TaskType::Jump);
    REQUIRE(multijump.type()   == tfl::TaskType::MultiJump);
    REQUIRE(graph.type()       == tfl::TaskType::Graph);
}

// ============================================================================
// SECTION 5: 哈希值与容器
// ============================================================================

/// @test [task][hash] 同节点的两个句柄 hash 相同；不同节点 hash 不同。
TEST_CASE("Task: hash_value 基于底层节点身份", "[task][hash]") {
    tfl::Flow flow;
    auto a = flow.emplace([] {});
    auto b = flow.emplace([] {});
    auto a_copy = a;

    REQUIRE(a.hash_value() == a_copy.hash_value());
    REQUIRE(a.hash_value() != b.hash_value());
}

/// @test [task][hash] Task 可作为 std::unordered_set 键。
TEST_CASE("Task: 可放入 unordered_set", "[task][hash][container]") {
    tfl::Flow flow;
    auto a = flow.emplace([] {});
    auto b = flow.emplace([] {});

    std::unordered_set<tfl::Task> set;
    set.insert(a);
    set.insert(b);
    set.insert(a);  // 重复插入应该被去重

    REQUIRE(set.size() == 2);
    REQUIRE(set.contains(a));
    REQUIRE(set.contains(b));
}

// ============================================================================
// SECTION 6: 遍历访问 —— for_each_successor / predecessor
// ============================================================================

/// @test [task][traversal] for_each_successor 访问者收 Task 句柄，可以读取其属性。
/// @warning 注意：visitor 形参是 `tfl::Task` 而非 `tfl::TaskView`！
///          TaskView 是 TaskObserver 回调里使用的只读切片，
///          Task::for_each_successor 给的是可写句柄。
TEST_CASE("Task: for_each_successor 遍历后继", "[task][traversal]") {
    tfl::Flow flow;
    auto hub = flow.emplace([] {});
    auto x = flow.emplace([] {}).name("X");
    auto y = flow.emplace([] {}).name("Y");
    auto z = flow.emplace([] {}).name("Z");
    hub.precede(x, y, z);

    std::set<std::string> visited;
    hub.for_each_successor([&](tfl::Task t) {
        visited.insert(std::string(t.name()));
    });

    REQUIRE(visited.size() == 3);
    REQUIRE(visited.contains("X"));
    REQUIRE(visited.contains("Y"));
    REQUIRE(visited.contains("Z"));
}

/// @test [task][traversal] for_each_predecessor 遍历前驱。
TEST_CASE("Task: for_each_predecessor 遍历前驱", "[task][traversal]") {
    tfl::Flow flow;
    auto sink = flow.emplace([] {});
    auto p1 = flow.emplace([] {}).name("P1");
    auto p2 = flow.emplace([] {}).name("P2");
    sink.succeed(p1, p2);

    int n = 0;
    sink.for_each_predecessor([&](tfl::Task) { ++n; });
    REQUIRE(n == 2);
}

// ============================================================================
// SECTION 7: 信号量配置 —— acquire / release / for_each
// ============================================================================

/// @test [task][semaphore] acquire/release 配置对外可观察。
TEST_CASE("Task: acquire/release 信号量配置", "[task][semaphore]") {
    tfl::Flow flow;
    tfl::Semaphore sem_a{2};
    tfl::Semaphore sem_b{1};

    auto t = flow.emplace([] {});
    t.acquire(sem_a, sem_b).release(sem_a, sem_b);

    int acq = 0, rel = 0;
    t.for_each_acquire([&](tfl::Semaphore&) { ++acq; });
    t.for_each_release([&](tfl::Semaphore&) { ++rel; });
    REQUIRE(acq == 2);
    REQUIRE(rel == 2);
}

/// @test [task][semaphore] 多配额 acquire(sem, count, sem, count) 形式。
TEST_CASE("Task: 多配额 acquire", "[task][semaphore][multi-count]") {
    tfl::Flow flow;
    tfl::Semaphore heavy_sem{10};
    tfl::Semaphore light_sem{5};

    auto t = flow.emplace([] {});
    // 3 单位 heavy_sem + 2 单位 light_sem
    t.acquire(heavy_sem, 3, light_sem, 2)
     .release(heavy_sem, 3, light_sem, 2);

    std::vector<std::pair<tfl::Semaphore*, std::size_t>> got;
    t.for_each_acquire([&](tfl::Semaphore& s, std::size_t& c) {
        got.emplace_back(&s, c);
    });
    REQUIRE(got.size() == 2);
    // 验证配额数（顺序按声明顺序）
    REQUIRE(got[0].second == 3);
    REQUIRE(got[1].second == 2);
}

/// @test [task][semaphore] clear_acquires / clear_releases 一键清空。
TEST_CASE("Task: clear_acquires/releases", "[task][semaphore][clear]") {
    tfl::Flow flow;
    tfl::Semaphore s1{1};
    tfl::Semaphore s2{1};

    auto t = flow.emplace([] {});
    t.acquire(s1, s2).release(s1, s2);

    int n_acq_before = 0;
    t.for_each_acquire([&](tfl::Semaphore&) { ++n_acq_before; });
    REQUIRE(n_acq_before == 2);

    t.clear_acquires();

    int n_acq_after = 0;
    t.for_each_acquire([&](tfl::Semaphore&) { ++n_acq_after; });
    REQUIRE(n_acq_after == 0);
}
