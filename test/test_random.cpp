/// @file test_random.cpp
/// @brief SplitMix64 与高位乘法测试 —— 确定性、范围绑定与边界。

#include "test_common.hpp"
#include "../taskflowlite/core/random.hpp"
#include <limits>

/// @test [random][seed] 相同种子得到相同序列，重新 seed 重置序列。
TEST_CASE("SplitMix64: deterministic sequence and reseeding", "[random][seed]") {
    tfl::SplitMix64 first, second;
    first.seed(42);
    second.seed(42);
    const auto initial = first.next();
    REQUIRE(initial == second.next());
    for (int i = 0; i < 100; ++i) REQUIRE(first.next() == second.next());
    first.seed(42);
    REQUIRE(first.next() == initial);
}

/// @test [random][bounds] 无参/单参为半开区间，双参为闭区间。
TEST_CASE("SplitMix64: bound overloads and full integer range", "[random][bounds]") {
    tfl::SplitMix64 random;
    random.seed(7, 3);
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(random() < 3);
        REQUIRE(random(17) < 17);
        const auto value = random(10, 20);
        REQUIRE(value >= 10);
        REQUIRE(value <= 20);
    }
    random.bind(1);
    REQUIRE(random() == 0);
    REQUIRE(random(7, 7) == 7);
    tfl::SplitMix64 raw;
    random.seed(42);
    raw.seed(42);
    REQUIRE(random(0, std::numeric_limits<std::uint32_t>::max()) == static_cast<std::uint32_t>(raw.next()));
}

/// @test [random][mulhi] 软件高位乘法与平台实现一致。
TEST_CASE("Random: high multiplication boundary values", "[random][mulhi]") {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    STATIC_REQUIRE(tfl::detail::mulhi64_soft(max, max) == max - 1);
    STATIC_REQUIRE(tfl::detail::mulhi64_soft(max, 2) == 1);
    tfl::SplitMix64 random;
    for (int i = 0; i < 1000; ++i) {
        const auto a = random.next(), b = random.next();
        REQUIRE(tfl::detail::mulhi64(a, b) == tfl::detail::mulhi64_soft(a, b));
    }
}
