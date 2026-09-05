/// @file test_task.cpp
/// @brief Task 模块测试 —— 句柄操作 / 拓扑构建 / 信号量配置 / 遍历访问。
///
/// 覆盖的接口：
///   - Task 默认 / nullptr / 拷贝 / 移动构造 / 赋值
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
TEST_CASE("Task: default constructed is empty", "[task][handle]") {
    tfl::Task t;
    REQUIRE_FALSE(static_cast<bool>(t));
    REQUIRE(t == tfl::Task{});  // 空句柄比较相等
}

/// @test [task][handle] 拷贝的句柄引用同一个节点。
/// @details Task 是弱引用；拷贝后两者指向同一个 Work*。
///          修改其中一个应该影响另一个的查询结果
///          （因为它们共享同一个底层节点）。
TEST_CASE("Task: copied handles share the same node", "[task][handle][copy]") {
    tfl::Flow flow;
    auto a = flow.emplace([] {});
    auto b = flow.emplace([] {});

    tfl::Task copy = a;        // 拷贝
    REQUIRE(copy == a);

    copy.precede(b);           // 通过拷贝修改
    REQUIRE(a.num_successors() == 1);  // 原始句柄能看到变化
}

// ============================================================================
// SECTION 2: 命名
// ============================================================================

/// @test [task][name] 链式 name() 设置后 name() 获取结果一致。
TEST_CASE("Task: name setter/getter", "[task][name]") {
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
TEST_CASE("Task: precede and succeed symmetric construction", "[task][topology]") {
    tfl::Flow flow;
    auto a = flow.emplace([] {});
    auto b = flow.emplace([] {});
    auto c = flow.emplace([] {});
    auto d = flow.emplace([] {});

/// @section precede-multi
    SECTION("precede(B, C, D): A is simultaneously predecessor of B/C/D") {
        a.precede(b, c, d);
        REQUIRE(a.num_successors() == 3);
        REQUIRE(b.num_predecessors() == 1);
        REQUIRE(c.num_predecessors() == 1);
        REQUIRE(d.num_predecessors() == 1);
    }

/// @section succeed-multi
    SECTION("succeed(B, C, D): A has B/C/D as predecessors simultaneously") {
        a.succeed(b, c, d);
        REQUIRE(a.num_predecessors() == 3);
        REQUIRE(b.num_successors() == 1);
    }
}

/// @test [task][topology] 菱形拓扑 A→{B,C}→D 边计数符合预期。
TEST_CASE("Task: diamond topology edge count", "[task][topology][diamond]") {
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

/// @test [task][topology] remove_successor / remove_predecessor 移除单条边。
/// @details 验证增量图编辑：构建一条边然后移除它；
///          前驱/后继计数归零。
TEST_CASE("Task: remove_successor bidirectional edge removal", "[task][topology][remove]") {
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

/// @test [task][topology] clear_successors / clear_predecessors 一次性移除所有邻接边。
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

/// @test [task][type] 插入各种签名后，type() 正确反映任务类型。
TEST_CASE("Task: type() reflects task type", "[task][type]") {
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

/// @test [task][hash] 指向同一节点的两个句柄哈希值相同；不同节点的哈希值不同。
TEST_CASE("Task: hash_value based on underlying node identity", "[task][hash]") {
    tfl::Flow flow;
    auto a = flow.emplace([] {});
    auto b = flow.emplace([] {});
    auto a_copy = a;

    REQUIRE(a.hash_value() == a_copy.hash_value());
    REQUIRE(a.hash_value() != b.hash_value());
}

/// @test [task][hash] Task 可用作 std::unordered_set 的键。
TEST_CASE("Task: can be placed in unordered_set", "[task][hash][container]") {
    tfl::Flow flow;
    auto a = flow.emplace([] {});
    auto b = flow.emplace([] {});

    std::unordered_set<tfl::Task> set;
    set.insert(a);
    set.insert(b);
    set.insert(a);  // 重复插入应被去重

    REQUIRE(set.size() == 2);
    REQUIRE(set.contains(a));
    REQUIRE(set.contains(b));
}

// ============================================================================
// SECTION 6: 遍历访问 —— for_each_successor / predecessor
// ============================================================================

/// @test [task][traversal] for_each_successor 访问者接收 Task 句柄并能读取其属性。
/// @warning 注意：访问者参数类型是 tfl::Task，不是 tfl::TaskView！
///          TaskView 是 TaskObserver 回调中使用的只读切片，
///          Task::for_each_successor 提供可写句柄。
TEST_CASE("Task: for_each_successor iterates successors", "[task][traversal]") {
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

/// @test [task][traversal] for_each_predecessor 遍历前驱节点。
TEST_CASE("Task: for_each_predecessor iterates predecessors", "[task][traversal]") {
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

/// @test [task][semaphore] acquire/release 配置可被外部观测。
TEST_CASE("Task: acquire/release semaphore configuration", "[task][semaphore]") {
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

/// @test [task][semaphore] 用链式 acquire(sem, count) 配置多个信号量。
TEST_CASE("Task: multi-count acquire", "[task][semaphore][multi-count]") {
    tfl::Flow flow;
    tfl::Semaphore heavy_sem{10};
    tfl::Semaphore light_sem{5};

    auto t = flow.emplace([] {});
    // 3 个单位的 heavy_sem + 2 个单位的 light_sem
    t.acquire(heavy_sem, 3).acquire(light_sem, 2)
     .release(heavy_sem, 3).release(light_sem, 2);

    std::vector<std::pair<tfl::Semaphore*, std::size_t>> got;
    t.for_each_acquire([&](tfl::Semaphore& s, std::size_t& c) {
        got.emplace_back(&s, c);
    });
    REQUIRE(got.size() == 2);
    // 验证配额计数（顺序遵循声明顺序）
    REQUIRE(got[0].second == 3);
    REQUIRE(got[1].second == 2);
}

/// @test [task][semaphore] clear_acquires / clear_releases 一次性全部清除。
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

// ============================================================================
// SECTION 8: work 重绑定的 callable 协议
// ============================================================================

/// @test [task][work] 占位符可重绑各协议，依赖边保持不变。
TEST_CASE("Task: work rebinds callable protocols without losing edges", "[task][work]") {
    TestEnv env;
    const int kind = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8);
    tfl::Flow inner, flow;
    int count = 0;
    (void)inner.emplace([&] { ++count; });
    auto task = flow.placeholder();
    auto finish = flow.emplace([&] { count += 10; });
    task.precede(finish);
    switch (kind) {
    case 0: task.work([&] { ++count; }); break;
    case 1: task.work([&](tfl::Runtime&) { ++count; }); break;
    case 2: task.work([&](tfl::Branch& branch) { ++count; branch.select(0); }); break;
    case 3: task.work([&](tfl::MultiBranch& branch) { ++count; branch.select(0); }); break;
    case 4: task.work([&](tfl::Jump& jump) { ++count; jump.select(0); }); break;
    case 5: task.work([&](tfl::MultiJump& jump) { ++count; jump.select(0); }); break;
    case 6: task.work([&](tfl::SubFlow& sf) { (void)sf.emplace([&] { ++count; }); sf.run(); }); break;
    case 7: task.work(inner); break;
    case 8: task.work(inner, 2ULL); break;
    }
    REQUIRE(task.num_successors() == 1);
    env.executor.async(flow).get();
    REQUIRE(count == (kind == 8 ? 12 : 11));
}
