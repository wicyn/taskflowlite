/// @file test_containers.cpp
/// @brief 内部容器测试 —— SmallVector（LLVM 风格小向量，广泛用于框架内部）。
///
/// 覆盖的不变量:
///   - push_back / pop_back 正确增减
///   - size / capacity / empty 一致性
///   - 栈内 / 堆分配切换后行为
///   - 迭代器遍历
///   - clear / resize / reserve
///   - 拷贝 / 移动语义
///   - pop_back_val
///   - operator[] / front / back / data

#include "test_common.hpp"

#include <algorithm>
#include <string>
#include <vector>

using tfl_test::TestEnv;

// ============================================================================
// 第 1 节: 构造与基本属性
// ============================================================================

/// @test [container][smallvec][basic] 默认构造空 SmallVector
TEST_CASE("SmallVector: default constructed is empty", "[container][smallvec][basic]") {
    tfl::SmallVector<int, 8> v;
    REQUIRE(v.empty());
    REQUIRE(v.size() == 0);
    REQUIRE(v.begin() == v.end());
}

/// @test [container][smallvec][basic] 带初始大小构造
TEST_CASE("SmallVector: sized construction", "[container][smallvec][basic]") {
    SECTION("zero-sized initial") {
        tfl::SmallVector<int, 8> v(0);
        REQUIRE(v.empty());
        REQUIRE(v.size() == 0);
    }

    SECTION("n elements (within stack capacity)") {
        tfl::SmallVector<int, 8> v(3);
        REQUIRE_FALSE(v.empty());
        REQUIRE(v.size() == 3);
    }

    SECTION("n elements (beyond stack capacity)") {
        tfl::SmallVector<int, 4> v(10);
        REQUIRE(v.size() == 10);
    }
}

// ============================================================================
// 第 2 节: push_back / pop_back
// ============================================================================

/// @test [container][smallvec][pushpop] push_back 递增 size
TEST_CASE("SmallVector: push_back grows and makes accessible", "[container][smallvec][pushpop]") {
    tfl::SmallVector<int, 4> v;

    v.push_back(10);
    REQUIRE(v.size() == 1);
    REQUIRE(v[0] == 10);
    REQUIRE(v.front() == 10);
    REQUIRE(v.back() == 10);

    v.push_back(20);
    REQUIRE(v.size() == 2);
    REQUIRE(v[1] == 20);

    v.push_back(30);
    REQUIRE(v.size() == 3);
    REQUIRE(v.back() == 30);
}

/// @test [container][smallvec][pushpop] push_back 超栈容量触发堆分配
TEST_CASE("SmallVector: push_back beyond stack capacity", "[container][smallvec][pushpop]") {
    tfl::SmallVector<int, 3> v;

    for (int i = 0; i < 3; ++i) v.push_back(i);
    REQUIRE(v.size() == 3);

    // 再推一个 → 触发堆分配
    v.push_back(100);
    REQUIRE(v.size() == 4);
    REQUIRE(v[3] == 100);
    REQUIRE(v.capacity() >= 4);

    // 原有元素应保留
    for (int i = 0; i < 3; ++i) REQUIRE(v[i] == i);
}

/// @test [container][smallvec][pushpop] pop_back 递减 size
TEST_CASE("SmallVector: pop_back reduces size", "[container][smallvec][pushpop]") {
    tfl::SmallVector<int, 8> v;

    for (int i = 0; i < 5; ++i) v.push_back(i);
    REQUIRE(v.size() == 5);

    v.pop_back();
    REQUIRE(v.size() == 4);
    REQUIRE(v.back() == 3);

    while (!v.empty()) v.pop_back();
    REQUIRE(v.empty());
}

/// @test [container][smallvec][pushpop] pop_back_val 返回移除的值
TEST_CASE("SmallVector: pop_back_val returns removed value", "[container][smallvec][pushpop]") {
    tfl::SmallVector<std::string, 4> v;
    v.push_back("hello");
    v.push_back("world");

    std::string removed = v.pop_back_val();
    REQUIRE(removed == "world");
    REQUIRE(v.size() == 1);
    REQUIRE(v.back() == "hello");
}

// ============================================================================
// 第 3 节: 迭代器
// ============================================================================

/// @test [container][smallvec][iter] 迭代器遍历
TEST_CASE("SmallVector: iterator traversal", "[container][smallvec][iter]") {
    tfl::SmallVector<int, 4> v;
    for (int i = 0; i < 5; ++i) v.push_back(i * 10);

    int expected = 0;
    for (auto it = v.begin(); it != v.end(); ++it) {
        REQUIRE(*it == expected);
        expected += 10;
    }
    REQUIRE(expected == 50);
}

/// @test [container][smallvec][iter] range-for 遍历
TEST_CASE("SmallVector: range-for works", "[container][smallvec][iter]") {
    tfl::SmallVector<int, 4> v;
    for (int i = 0; i < 5; ++i) v.push_back(i);

    int sum = 0;
    for (int x : v) sum += x;
    REQUIRE(sum == 10);  // 0+1+2+3+4
}

/// @test [container][smallvec][iter] const 迭代器
TEST_CASE("SmallVector: const iterators", "[container][smallvec][iter]") {
    tfl::SmallVector<int, 4> v;
    v.push_back(1);
    v.push_back(2);

    const auto& cv = v;
    int sum = 0;
    for (auto it = cv.begin(); it != cv.end(); ++it) sum += *it;
    REQUIRE(sum == 3);
}

// ============================================================================
// 第 4 节: clear / resize / reserve
// ============================================================================

/// @test [container][smallvec][management] clear 清空但保留容量
TEST_CASE("SmallVector: clear empties but preserves capacity", "[container][smallvec][management]") {
    tfl::SmallVector<int, 4> v;
    for (int i = 0; i < 10; ++i) v.push_back(i);
    auto cap = v.capacity();

    v.clear();
    REQUIRE(v.empty());
    REQUIRE(v.size() == 0);
    REQUIRE(v.capacity() == cap);
}

/// @test [container][smallvec][management] resize 改变大小
TEST_CASE("SmallVector: resize changes size", "[container][smallvec][management]") {
    tfl::SmallVector<int, 8> v;

    SECTION("resize larger") {
        v.resize(5);
        REQUIRE(v.size() == 5);
    }

    SECTION("resize smaller") {
        for (int i = 0; i < 8; ++i) v.push_back(i);
        v.resize(3);
        REQUIRE(v.size() == 3);
        REQUIRE(v[2] == 2);
    }

    SECTION("resize with fill value") {
        v.resize(4, 42);
        REQUIRE(v.size() == 4);
        for (std::size_t i = 0; i < 4; ++i) REQUIRE(v[i] == 42);
    }
}

/// @test [container][smallvec][management] reserve 预分配
TEST_CASE("SmallVector: reserve grows capacity", "[container][smallvec][management]") {
    tfl::SmallVector<int, 4> v;
    v.reserve(100);
    REQUIRE(v.capacity() >= 100);
    REQUIRE(v.size() == 0);

    v.push_back(1);
    REQUIRE(v[0] == 1);
}

// ============================================================================
// 第 5 节: 元素访问
// ============================================================================

/// @test [container][smallvec][access] operator[] / front / back / data
TEST_CASE("SmallVector: element access methods", "[container][smallvec][access]") {
    tfl::SmallVector<int, 4> v;
    for (int i = 1; i <= 5; ++i) v.push_back(i);

    REQUIRE(v.front() == 1);
    REQUIRE(v.back() == 5);
    REQUIRE(v[0] == 1);
    REQUIRE(v[4] == 5);
    REQUIRE(v.data()[0] == 1);

    // 可修改
    v[2] = 99;
    REQUIRE(v[2] == 99);
}

// ============================================================================
// 第 6 节: 拷贝与移动
// ============================================================================

/// @test [container][smallvec][copy] 拷贝构造独立于源
TEST_CASE("SmallVector: copy construction", "[container][smallvec][copy]") {
    tfl::SmallVector<int, 4> v;
    for (int i = 0; i < 5; ++i) v.push_back(i);

    tfl::SmallVector<int, 4> copy = v;
    REQUIRE(copy.size() == v.size());
    for (std::size_t i = 0; i < v.size(); ++i) REQUIRE(copy[i] == v[i]);

    v[0] = 999;
    REQUIRE(copy[0] == 0);  // 独立
}

/// @test [container][smallvec][move] 移动构造
TEST_CASE("SmallVector: move construction", "[container][smallvec][move]") {
    tfl::SmallVector<int, 4> v;
    for (int i = 0; i < 5; ++i) v.push_back(i);

    tfl::SmallVector<int, 4> moved = std::move(v);
    REQUIRE(moved.size() == 5);
    for (int i = 0; i < 5; ++i) REQUIRE(moved[i] == i);
}

// ============================================================================
// 第 7 节: 栈/堆边界反复跨越
// ============================================================================

/// @test [container][smallvec][boundary] 反复跨越栈/堆边界，数据不丢失
TEST_CASE("SmallVector: crossing stack/heap boundary repeatedly", "[container][smallvec][boundary]") {
    tfl::SmallVector<int, 4> v;

    for (int i = 0; i < 4; ++i) v.push_back(i);
    // 推到堆
    v.push_back(100);
    REQUIRE(v.size() == 5);
    // 退回栈
    v.pop_back();
    REQUIRE(v.size() == 4);
    REQUIRE(v.back() == 3);
    // 再次推到堆
    v.push_back(200);
    REQUIRE(v.size() == 5);
    REQUIRE(v.back() == 200);

    for (int i = 0; i < 4; ++i) REQUIRE(v[i] == i);
}

// ============================================================================
// 第 8 节: 大量 push 压力
// ============================================================================

/// @test [container][smallvec][stress] 100 轮 push-pop 压力
TEST_CASE("SmallVector: 10K push/pop stress", "[container][smallvec][stress]") {
    tfl::SmallVector<int, 8> v;

    for (int trial = 0; trial < 100; ++trial) {
        for (int i = 0; i < 100; ++i) v.push_back(i);
        auto base = trial * 100;
        for (int i = 0; i < 100; ++i) {
            REQUIRE(v[static_cast<std::size_t>(base + i)] == i);
        }
    }

    while (!v.empty()) v.pop_back();
    REQUIRE(v.size() == 0);
}

// ============================================================================
// 第 9 节: std::string 类型（非 POD）
// ============================================================================

/// @test [container][smallvec][string] std::string 元素生命周期
TEST_CASE("SmallVector: std::string elements", "[container][smallvec][string]") {
    tfl::SmallVector<std::string, 3> v;

    v.push_back("hello");
    v.push_back("world");
    v.push_back("test");
    v.push_back("overflow");  // 超栈容量

    REQUIRE(v.size() == 4);
    REQUIRE(v[0] == "hello");
    REQUIRE(v[1] == "world");
    REQUIRE(v[2] == "test");
    REQUIRE(v[3] == "overflow");

    v.pop_back();
    REQUIRE(v.size() == 3);

    v.clear();
    REQUIRE(v.empty());
}

// ============================================================================
// 覆盖矩阵
// ============================================================================
//   构造         ✓ "SmallVector: default constructed is empty" / "sized construction"
//   push/pop    ✓ "push_back grows" / "beyond stack capacity" / "pop_back reduces" / "pop_back_val"
//   迭代器       ✓ "iterator traversal" / "range-for works" / "const iterators"
//   管理         ✓ "clear empties but preserves capacity" / "resize" / "reserve"
//   访问         ✓ "element access methods"
//   拷贝/移动    ✓ "copy construction" / "move construction"
//   边界         ✓ "crossing stack/heap boundary repeatedly"
//   压力         ✓ "10K push/pop stress"
//   非 POD      ✓ "std::string elements"
