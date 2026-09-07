#include "catch_amalgamated.hpp"
#include <taskflowlite/core/small_vector.hpp>

#include <array>
#include <ranges>
#include <sstream>
#include <type_traits>

namespace {
struct Source { int value; };

struct Target {
    int value;
    Target(Source source) : value(source.value) {}
    Target(const Target&) = default;
    Target(Target&&) = default;
    Target& operator=(const Target&) = delete;
    Target& operator=(Target&&) = delete;
    Target& operator=(Source source) {
        value = source.value;
        return *this;
    }
};

static_assert(std::constructible_from<Target, Source&>);
static_assert(std::is_assignable_v<Target&, Source&>);
static_assert(!std::is_copy_assignable_v<Target>);
static_assert(!std::is_move_assignable_v<Target>);

template <std::size_t N>
void check_forward_assignment() {
    tfl::SmallVector<Target, N> values;
    std::array<Source, 2> initial{{{1}, {2}}};
    values.assign(initial.begin(), initial.end());
    REQUIRE(values.size() == 2);
    REQUIRE(values[0].value == 1);
    REQUIRE(values[1].value == 2);

    auto self = values | std::views::transform([](const Target& value) {
        return Source{value.value * 10};
    });
    values.assign_range(self);
    REQUIRE(values[0].value == 10);
    REQUIRE(values[1].value == 20);

    std::array<Source, 6> larger{{{3}, {4}, {5}, {6}, {7}, {8}}};
    values.assign_range(larger);
    REQUIRE(values.size() == larger.size());
    for (std::size_t i = 0; i < larger.size(); ++i) {
        REQUIRE(values[i].value == larger[i].value);
    }

    values.assign(initial.begin(), initial.begin() + 1);
    REQUIRE(values.size() == 1);
    REQUIRE(values[0].value == 1);
    values.assign(initial.begin(), initial.begin());
    REQUIRE(values.empty());
}

template <std::size_t N>
void check_input_assignment() {
    tfl::SmallVector<Target, N> values;
    std::array<Source, 1> initial{{{99}}};
    values.assign_range(initial);
    std::istringstream stream("10 20 30 40 50");
    auto input = std::ranges::istream_view<int>(stream)
        | std::views::transform([](int value) { return Source{value}; });
    values.assign_range(input);
    REQUIRE(values.size() == 5);
    for (std::size_t i = 0; i < values.size(); ++i) {
        REQUIRE(values[i].value == static_cast<int>((i + 1) * 10));
    }
}
} // namespace

TEST_CASE("SmallVector assigns converting ranges without same-type assignment", "[small_vector][llvm-regression]") {
    check_forward_assignment<0>();
    check_forward_assignment<4>();
}

TEST_CASE("SmallVector assigns converting input ranges without same-type assignment", "[small_vector][llvm-regression]") {
    check_input_assignment<0>();
    check_input_assignment<4>();
}
