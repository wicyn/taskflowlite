/// @file test_result_slot.cpp
/// @brief ResultSlot 测试 —— 值、引用、void、可选存储和异常。

#include "test_common.hpp"
#include "../taskflowlite/core/result_slot.hpp"

/// @test [result-slot][value] invoke、return_value、const ref 和 take。
TEST_CASE("ResultSlot: direct value storage", "[result-slot][value]") {
    tfl::ResultSlot<int> slot;
    slot.return_value(7);
    REQUIRE(slot.ref() == 7);
    slot.invoke([](int a, int b) { return a + b; }, 20, 22);
    STATIC_REQUIRE(std::same_as<decltype(std::as_const(slot).ref()), const int&>);
    REQUIRE(std::as_const(slot).ref() == 42);
    REQUIRE(std::move(slot).take() == 42);
}

/// @test [result-slot][optional] 无默认构造、不可赋值类型通过可选存储重建。
TEST_CASE("ResultSlot: optional storage supports nonassignable values", "[result-slot][optional]") {
    struct Value {
        const int number;
        explicit Value(int n) : number(n) {}
    };
    tfl::ResultSlot<Value> slot;
    slot.return_value(Value{17});
    REQUIRE(slot.ref().number == 17);
    slot.invoke([] { return Value{42}; });
    REQUIRE(slot.ref().number == 42);
}

/// @test [result-slot][move] take 可移出 move-only 结果。
TEST_CASE("ResultSlot: move-only result extraction", "[result-slot][move]") {
    tfl::ResultSlot<std::unique_ptr<int>> slot;
    slot.invoke([] { return std::make_unique<int>(42); });
    auto result = std::move(slot).take();
    REQUIRE(*result == 42);
    REQUIRE(slot.ref() == nullptr);
}

/// @test [result-slot][reference] 引用槽保留身份，不复制外部对象。
TEST_CASE("ResultSlot: reference and void specializations", "[result-slot][reference]") {
    int value = 7;
    tfl::ResultSlot<int&> reference;
    reference.return_value(value);
    REQUIRE(&reference.ref() == &value);
    reference.invoke([&]() -> int& { return value; });
    reference.take() = 42;
    REQUIRE(value == 42);
    tfl::ResultSlot<void> empty;
    empty.invoke([&] { ++value; });
    empty.return_void();
    empty.ref();
    empty.take();
    REQUIRE(value == 43);
}

/// @test [result-slot][exception] callable 抛出时不吞异常，旧直接存储保持。
TEST_CASE("ResultSlot: invoke propagates exceptions", "[result-slot][exception]") {
    tfl::ResultSlot<int> slot;
    slot.return_value(7);
    REQUIRE_THROWS_AS(slot.invoke([]() -> int { throw std::runtime_error("result"); }), std::runtime_error);
    REQUIRE(slot.ref() == 7);
}
