#include "catch_amalgamated.hpp"
#include <taskflowlite/core/small_vector.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <random>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

template <typename V>
auto position(V& values, std::size_t index) {
    return index == 0 ? values.begin() : values.begin() + index;
}

template <typename T, std::size_t N>
void check_values(const tfl::SmallVector<T, N>& actual, const std::vector<T>& expected) {
    REQUIRE(actual.size() == expected.size());
    CHECK(actual.capacity() >= actual.size());
    CHECK(actual.empty() == expected.empty());
    CHECK(std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()));
    CHECK(std::equal(actual.rbegin(), actual.rend(), expected.rbegin(), expected.rend()));
    if (!expected.empty()) {
        CHECK(actual.front() == expected.front());
        CHECK(actual.back() == expected.back());
    }
}

template <typename T>
T random_value(std::mt19937& random) {
    if constexpr (std::is_same_v<T, int>) {
        return static_cast<int>(random() % 1000) - 500;
    } else {
        return std::string(40 + random() % 30, static_cast<char>('a' + random() % 26))
             + std::to_string(random() % 1000);
    }
}

template <typename T, std::size_t N>
void differential_sequence() {
    std::mt19937 random(0x5A17u + static_cast<unsigned>(N));
    tfl::SmallVector<T, N> actual;
    std::vector<T> expected;
    for (unsigned step = 0; step < 1200; ++step) {
        const auto operation = random() % 13;
        const auto index = expected.empty() ? 0u : random() % (expected.size() + 1);
        const auto count = random() % 9;
        const auto value = random_value<T>(random);
        CAPTURE(N, step, operation, index, count);
        switch (operation) {
        case 0:
            actual.push_back(value);
            expected.push_back(value);
            break;
        case 1: {
            auto it = actual.insert(position(actual, index), value);
            expected.insert(position(expected, index), value);
            CHECK(it == position(actual, index));
            break;
        }
        case 2:
            actual.insert(position(actual, index), count, value);
            expected.insert(position(expected, index), count, value);
            break;
        case 3: {
            const auto finish = index + random() % (expected.size() - index + 1);
            auto it = actual.erase(position(actual, index), position(actual, finish));
            expected.erase(position(expected, index), position(expected, finish));
            CHECK(it == position(actual, index));
            break;
        }
        case 4:
            actual.resize(count * 3, value);
            expected.resize(count * 3, value);
            break;
        case 5:
            actual.assign(count * 2, value);
            expected.assign(count * 2, value);
            break;
        case 6:
            actual.reserve(count * 8);
            expected.reserve(count * 8);
            break;
        case 7:
            actual.shrink_to_fit();
            expected.shrink_to_fit();
            break;
        case 8:
            if (!actual.empty()) {
                actual.pop_back();
                expected.pop_back();
            }
            break;
        case 9: {
            std::vector<T> source(count, value);
            actual.insert(position(actual, index), source.begin(), source.end());
            expected.insert(position(expected, index), source.begin(), source.end());
            break;
        }
        case 10: {
            const std::array<T, 2> source{value, random_value<T>(random)};
            actual.append_range(source);
            expected.insert(expected.end(), source.begin(), source.end());
            break;
        }
        case 11:
            actual.resize(count);
            expected.resize(count);
            break;
        case 12: {
            tfl::SmallVector<T, N> copy(actual);
            check_values(copy, expected);
            actual = std::move(copy);
            CHECK(copy.empty());
            break;
        }
        }
        check_values(actual, expected);
    }
}

struct ConstructionFailure : std::runtime_error {
    ConstructionFailure() : std::runtime_error("injected element failure") {}
};

struct Tracked {
    static inline int live = 0;
    static inline int default_budget = -1;
    static inline int copy_budget = -1;
    static inline int assignment_budget = -1;
    int value = 0;

    static void consume(int& budget) {
        if (budget == 0) throw ConstructionFailure{};
        if (budget > 0) --budget;
    }
    static void disarm() {
        default_budget = copy_budget = assignment_budget = -1;
    }
    Tracked() { consume(default_budget); ++live; }
    explicit Tracked(int n) : value(n) { ++live; }
    Tracked(const Tracked& other) : value(other.value) { consume(copy_budget); ++live; }
    Tracked(Tracked&& other) noexcept(false) : value(other.value) {
        consume(copy_budget);
        other.value = -1;
        ++live;
    }
    Tracked& operator=(const Tracked& other) {
        consume(assignment_budget);
        value = other.value;
        return *this;
    }
    Tracked& operator=(Tracked&& other) noexcept(false) {
        consume(assignment_budget);
        value = other.value;
        other.value = -1;
        return *this;
    }
    ~Tracked() { --live; }
};

struct DisarmOnExit {
    ~DisarmOnExit() { Tracked::disarm(); }
};

template <std::size_t N>
std::vector<int> tracked_values(const tfl::SmallVector<Tracked, N>& values) {
    std::vector<int> result;
    for (const auto& element : values) result.push_back(element.value);
    return result;
}

std::vector<std::unique_ptr<int>> owned_values(std::initializer_list<int> values) {
    std::vector<std::unique_ptr<int>> result;
    for (int value : values) result.push_back(std::make_unique<int>(value));
    return result;
}

template <std::size_t N>
std::vector<int> pointed_values(const tfl::SmallVector<std::unique_ptr<int>, N>& values) {
    std::vector<int> result;
    for (const auto& value : values) result.push_back(value ? *value : -1);
    return result;
}

template <std::size_t N>
void move_only_ranges() {
    auto source = owned_values({1, 2, 3});
    tfl::SmallVector<std::unique_ptr<int>, N> values(
        std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
    CHECK(pointed_values(values) == std::vector<int>{1, 2, 3});
    CHECK(std::all_of(source.begin(), source.end(), [](const auto& p) { return !p; }));

    source = owned_values({4, 5});
    values.assign(std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
    CHECK(pointed_values(values) == std::vector<int>{4, 5});

    source = owned_values({6, 7, 8});
    values.append(std::make_move_iterator(source.begin()), std::make_move_iterator(source.end()));
    CHECK(pointed_values(values) == std::vector<int>{4, 5, 6, 7, 8});

    source = owned_values({10, 11});
    values.insert(values.begin() + 1, std::make_move_iterator(source.begin()),
                  std::make_move_iterator(source.end()));
    CHECK(pointed_values(values) == std::vector<int>{4, 10, 11, 5, 6, 7, 8});

    values.reserve(32);
    source = owned_values({20, 21, 22});
    values.insert(values.end() - 1, std::make_move_iterator(source.begin()),
                  std::make_move_iterator(source.end()));
    CHECK(pointed_values(values) == std::vector<int>{4, 10, 11, 5, 6, 7, 20, 21, 22, 8});

    values.assign(std::make_move_iterator(values.rbegin()), std::make_move_iterator(values.rend()));
    CHECK(pointed_values(values) == std::vector<int>{8, 22, 21, 20, 7, 6, 5, 11, 10, 4});
    values.append(std::make_move_iterator(values.begin()), std::make_move_iterator(values.begin() + 2));
    CHECK(pointed_values(values) == std::vector<int>{-1, -1, 21, 20, 7, 6, 5, 11, 10, 4, 8, 22});
}

struct alignas(128) AlignedValue {
    std::array<std::uint64_t, 16> payload{};
    explicit AlignedValue(std::uint64_t n = 0) { payload[0] = n; }
};

}  // namespace

TEST_CASE("SmallVector integer operations match std::vector", "[small_vector][random]") {
    differential_sequence<int, 0>();
    differential_sequence<int, 2>();
    differential_sequence<int, 8>();
}

TEST_CASE("SmallVector string operations match std::vector", "[small_vector][random]") {
    differential_sequence<std::string, 0>();
    differential_sequence<std::string, 2>();
    differential_sequence<std::string, 8>();
}

TEST_CASE("SmallVector zero capacity empty operations and bounds", "[small_vector]") {
    tfl::SmallVector<int, 0> values;
    CHECK(values.data() == nullptr);
    CHECK(values.begin() == values.end());
    CHECK_FALSE(values.is_inline());
    CHECK_FALSE(values.is_heap());
    CHECK(values.erase(values.begin(), values.end()) == values.end());
    CHECK(values.insert(values.end(), 0, 4) == values.end());
    values.append(values.begin(), values.end());
    values.assign(values.begin(), values.end());
    tfl::SmallVector<int, 0> other;
    values = other;
    values.assign_range(other);
    values = std::initializer_list<int>{};
    values.assign(std::initializer_list<int>{});
    CHECK(values.empty());
    CHECK(values.begin() == values.end());
    values.truncate(0);
    values.pop_back_n(0);
    values.resize_for_overwrite(0);
    CHECK_THROWS_AS(values.at(0), std::out_of_range);
    CHECK_THROWS_AS(values.reserve(values.max_size() + 1), std::length_error);
    tfl::SmallVector<char, 0> bytes;
    CHECK(bytes.max_size() <= static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()));
    CHECK_THROWS_AS(bytes.reserve(bytes.max_size() + 1), std::length_error);
    values.push_back(7);
    values.clear();
    values.shrink_to_fit();
    CHECK(values.data() == nullptr);
    CHECK(values.capacity() == 0);
}

TEST_CASE("SmallVector protects element references during mutations", "[small_vector][alias]") {
    tfl::SmallVector<std::string, 2> values{"alpha", "beta"};
    values.push_back(values.front());
    check_values(values, {"alpha", "beta", "alpha"});
    values.shrink_to_fit();
    values.emplace(values.begin() + 1, values.back());
    check_values(values, {"alpha", "alpha", "beta", "alpha"});
    values.resize(9, values[2]);
    check_values(values, {"alpha", "alpha", "beta", "alpha", "beta", "beta", "beta", "beta", "beta"});
    values.assign(3, values[1]);
    check_values(values, {"alpha", "alpha", "alpha"});
    values[1] = "inside";
    values.insert(values.begin(), 6, values[1]);
    check_values(values, {"inside", "inside", "inside", "inside", "inside", "inside", "alpha", "inside", "alpha"});
    values.shrink_to_fit();
    values.append(2, values[6]);
    CHECK(values[9] == "alpha");
    CHECK(values[10] == "alpha");
}

TEST_CASE("SmallVector supports overlapping forward and reverse ranges", "[small_vector][alias]") {
    for (bool spare_capacity : {false, true}) {
        CAPTURE(spare_capacity);
        tfl::SmallVector<std::string, 2> values{"a", "b", "c", "d"};
        if (spare_capacity) values.reserve(64);
        values.insert(values.begin() + 1, values.rbegin(), values.rend());
        check_values(values, {"a", "d", "c", "b", "a", "b", "c", "d"});
        values.assign(values.rbegin(), values.rend());
        check_values(values, {"d", "c", "b", "a", "b", "c", "d", "a"});
        values.assign(values.begin() + 2, values.begin() + 5);
        check_values(values, {"b", "a", "b"});
        values.append_range(values | std::views::reverse);
        check_values(values, {"b", "a", "b", "b", "a", "b"});
        values.insert_range(values.end(), std::ranges::subrange(values.begin(), values.begin() + 2));
        check_values(values, {"b", "a", "b", "b", "a", "b", "b", "a"});
    }
}

TEST_CASE("SmallVector consumes move only iterator ranges", "[small_vector][move]") {
    move_only_ranges<0>();
    move_only_ranges<2>();
    move_only_ranges<8>();
}

TEST_CASE("SmallVector accepts single pass iterators and distinct sentinels", "[small_vector][range]") {
    std::istringstream initial("1 2 3");
    tfl::SmallVector<int, 2> values{std::istream_iterator<int>(initial), std::istream_iterator<int>()};
    check_values(values, {1, 2, 3});
    std::istringstream inserted("8 9");
    values.insert(values.begin() + 1, std::istream_iterator<int>(inserted), std::istream_iterator<int>());
    check_values(values, {1, 8, 9, 2, 3});
    std::istringstream appended("4 5 6");
    values.append(std::istream_iterator<int>(appended), std::istream_iterator<int>());
    check_values(values, {1, 8, 9, 2, 3, 4, 5, 6});
    std::istringstream assigned("10 11");
    values.assign(std::istream_iterator<int>(assigned), std::istream_iterator<int>());
    check_values(values, {10, 11});
    int source[]{20, 21, 22};
    values.assign(std::counted_iterator(source, 3), std::default_sentinel);
    check_values(values, {20, 21, 22});
    values.append(std::counted_iterator(source, 2), std::default_sentinel);
    check_values(values, {20, 21, 22, 20, 21});
}

TEST_CASE("SmallVector failed construction and relocation release all elements", "[small_vector][exception]") {
    REQUIRE(Tracked::live == 0);
    DisarmOnExit reset;
    {
        Tracked seed(7);
        Tracked::copy_budget = 2;
        REQUIRE_THROWS_AS((tfl::SmallVector<Tracked, 2>(8, seed)), ConstructionFailure);
        CHECK(Tracked::live == 1);
        Tracked::disarm();
    }
    for (int budget = 0; budget < 3; ++budget) {
        CAPTURE(budget);
        tfl::SmallVector<Tracked, 2> values;
        values.emplace_back(10);
        values.emplace_back(20);
        values.emplace_back(30);
        auto* old_data = values.data();
        const auto old_capacity = values.capacity();
        Tracked::copy_budget = budget;
        REQUIRE_THROWS_AS(values.reserve(old_capacity + 10), ConstructionFailure);
        Tracked::disarm();
        CHECK(values.data() == old_data);
        CHECK(values.capacity() == old_capacity);
        CHECK(tracked_values(values) == std::vector<int>{10, 20, 30});
        CHECK(Tracked::live == 3);
    }
    CHECK(Tracked::live == 0);
    {
        tfl::SmallVector<Tracked, 2> values;
        values.reserve(10);
        values.emplace_back(4);
        values.emplace_back(5);
        auto* old_data = values.data();
        Tracked::copy_budget = 1;
        REQUIRE_THROWS_AS(values.shrink_to_fit(), ConstructionFailure);
        Tracked::disarm();
        CHECK(values.data() == old_data);
        CHECK(tracked_values(values) == std::vector<int>{4, 5});
        CHECK(Tracked::live == 2);
        values.shrink_to_fit();
        CHECK(values.is_inline());
    }
    CHECK(Tracked::live == 0);
}

TEST_CASE("SmallVector resize rolls back failing growth", "[small_vector][exception]") {
    REQUIRE(Tracked::live == 0);
    DisarmOnExit reset;
    for (bool spare_capacity : {false, true}) {
        for (int variant = 0; variant < 3; ++variant) {
            CAPTURE(spare_capacity, variant);
            tfl::SmallVector<Tracked, 2> values;
            values.emplace_back(10);
            values.emplace_back(20);
            if (spare_capacity) values.reserve(12);
            const auto* old_data = values.data();
            const auto old_capacity = values.capacity();
            if (variant == 0) {
                Tracked::default_budget = 1;
                REQUIRE_THROWS_AS(values.resize(7), ConstructionFailure);
            } else if (variant == 1) {
                Tracked::copy_budget = 1;
                REQUIRE_THROWS_AS(values.resize(7, values.front()), ConstructionFailure);
            } else {
                Tracked::default_budget = 1;
                REQUIRE_THROWS_AS(values.resize_for_overwrite(7), ConstructionFailure);
            }
            Tracked::disarm();
            CHECK(values.data() == old_data);
            CHECK(values.capacity() == old_capacity);
            CHECK(tracked_values(values) == std::vector<int>{10, 20});
            CHECK(Tracked::live == 2);
        }
    }
    CHECK(Tracked::live == 0);
}

TEST_CASE("SmallVector append rolls back failing element construction", "[small_vector][exception]") {
    REQUIRE(Tracked::live == 0);
    DisarmOnExit reset;
    for (bool spare_capacity : {false, true}) {
        for (bool fill : {false, true}) {
            CAPTURE(spare_capacity, fill);
            tfl::SmallVector<Tracked, 2> values;
            values.emplace_back(10);
            values.emplace_back(20);
            if (spare_capacity) values.reserve(12);
            const auto* old_data = values.data();
            const auto old_capacity = values.capacity();
            const std::array<Tracked, 3> source{Tracked(7), Tracked(8), Tracked(9)};
            // With growth this budget allows relocation of both old elements and
            // at least one new element, regardless of which group is built first.
            Tracked::copy_budget = spare_capacity ? 1 : 3;
            if (fill) REQUIRE_THROWS_AS(values.append(4, source[0]), ConstructionFailure);
            else REQUIRE_THROWS_AS(values.append(source.begin(), source.end()), ConstructionFailure);
            Tracked::disarm();
            CHECK(values.data() == old_data);
            CHECK(values.capacity() == old_capacity);
            CHECK(tracked_values(values) == std::vector<int>{10, 20});
            CHECK(Tracked::live == 5);
        }
    }
    CHECK(Tracked::live == 0);
}

TEST_CASE("SmallVector insertion cleans partial construction and throwing assignment", "[small_vector][exception]") {
    REQUIRE(Tracked::live == 0);
    DisarmOnExit reset;
    for (int budget = 0; budget < 5; ++budget) {
        CAPTURE(budget);
        tfl::SmallVector<Tracked, 2> values;
        values.emplace_back(1);
        values.emplace_back(2);
        const std::array<Tracked, 3> source{Tracked(10), Tracked(11), Tracked(12)};
        auto* old_data = values.data();
        const auto old_capacity = values.capacity();
        Tracked::copy_budget = budget;
        REQUIRE_THROWS_AS(values.insert(values.begin() + 1, source.begin(), source.end()), ConstructionFailure);
        Tracked::disarm();
        CHECK(values.data() == old_data);
        CHECK(values.capacity() == old_capacity);
        CHECK(tracked_values(values) == std::vector<int>{1, 2});
        CHECK(Tracked::live == 5);
    }
    {
        tfl::SmallVector<Tracked, 8> values;
        for (int n = 0; n < 4; ++n) values.emplace_back(n);
        Tracked::assignment_budget = 0;
        REQUIRE_THROWS_AS(values.emplace(values.begin() + 1, 99), ConstructionFailure);
        Tracked::disarm();
        // In-place assignment promises the basic guarantee: a valid, fully
        // destructible container, even when element values have changed.
        CHECK(values.capacity() >= values.size());
        CHECK(Tracked::live == static_cast<int>(values.size()));
        values.clear();
        CHECK(Tracked::live == 0);
        values.emplace_back(42);
        CHECK(values.front().value == 42);
    }
    CHECK(Tracked::live == 0);
}

TEST_CASE("SmallVector respects over aligned inline and heap storage", "[small_vector][alignment]") {
    tfl::SmallVector<AlignedValue, 2> values;
    auto check_alignment = [&] {
        REQUIRE(values.data() != nullptr);
        CHECK(reinterpret_cast<std::uintptr_t>(values.data()) % alignof(AlignedValue) == 0);
        for (const auto& value : values) {
            CHECK(reinterpret_cast<std::uintptr_t>(std::addressof(value)) % alignof(AlignedValue) == 0);
        }
    };
    check_alignment();
    values.emplace_back(7);
    values.emplace_back(8);
    values.emplace_back(9);
    check_alignment();
    values.reserve(40);
    check_alignment();
    CHECK(values[0].payload[0] == 7);
    CHECK(values[2].payload[0] == 9);
    values.resize(2);
    values.shrink_to_fit();
    CHECK(values.is_inline());
    check_alignment();
    CHECK(values[1].payload[0] == 8);
    tfl::SmallVector<AlignedValue, 0> heap;
    heap.emplace_back(123);
    CHECK(reinterpret_cast<std::uintptr_t>(heap.data()) % alignof(AlignedValue) == 0);
    CHECK(heap.front().payload[0] == 123);
}

TEST_CASE("SmallVector moves between capacities and swaps storage states", "[small_vector][move]") {
    tfl::SmallVector<std::string, 8> large{"a", "b", "c"};
    tfl::SmallVector<std::string, 2> small(std::move(large));
    check_values(small, {"a", "b", "c"});
    CHECK(large.empty());
    CHECK(large.is_inline());
    tfl::SmallVector<std::string, 0> heap(std::move(small));
    check_values(heap, {"a", "b", "c"});
    CHECK(small.empty());
    auto* heap_data = heap.data();
    large = std::move(heap);
    CHECK(large.data() == heap_data);
    check_values(large, {"a", "b", "c"});
    CHECK(heap.empty());
    heap.push_back("reused");
    CHECK(heap.front() == "reused");

    for (bool left_heap : {false, true}) {
        for (bool right_heap : {false, true}) {
            CAPTURE(left_heap, right_heap);
            tfl::SmallVector<std::string, 2> left{"left"};
            tfl::SmallVector<std::string, 2> right{"right", "two"};
            if (left_heap) left.reserve(10);
            if (right_heap) right.reserve(10);
            using std::swap;
            swap(left, right);
            check_values(left, {"right", "two"});
            check_values(right, {"left"});
            left.swap(left);
            check_values(left, {"right", "two"});
        }
    }
    tfl::SmallVector<std::string, 0> empty;
    large = std::move(empty);
    CHECK(large.empty());
    CHECK(large.begin() == large.end());
    CHECK(large.is_inline());
    CHECK(large.capacity() == 8);
    tfl::SmallVector<std::string, 8> from_empty(std::move(empty));
    CHECK(from_empty.empty());
    CHECK(from_empty.begin() == from_empty.end());
    CHECK(from_empty.is_inline());
    CHECK(from_empty.capacity() == 8);
    large.push_back("after empty move");
    CHECK(large.front() == "after empty move");
}

TEST_CASE("SmallVector exposes coherent comparison and extension operations", "[small_vector]") {
    tfl::SmallVector<int, 2> values{1, 2, 3, 2, 5};
    tfl::SmallVector<int, 8> copy(values);
    CHECK(values == copy);
    CHECK(((values <=> copy) == 0));
    CHECK(tfl::erase(values, 2) == 2);
    check_values(values, {1, 3, 5});
    CHECK(tfl::erase_if(values, [](int n) { return n > 3; }) == 1);
    check_values(values, {1, 3});
    CHECK(values.size_in_bytes() == 2 * sizeof(int));
    CHECK(tfl::capacity_in_bytes(values) == values.capacity() * sizeof(int));
    values.resize_for_overwrite(5);
    for (std::size_t i = 2; i < values.size(); ++i) values[i] = static_cast<int>(i + 10);
    check_values(values, {1, 3, 12, 13, 14});
    CHECK(values.pop_back_val() == 14);
    values.pop_back_n(2);
    check_values(values, {1, 3});
    values.truncate(1);
    CHECK(values.front() == 1);
    values.clear();
    values.shrink_to_fit();
    CHECK(values.is_inline());
    CHECK(values.capacity() == 2);
}

namespace {

struct ThrowingMoveOnly {
    static inline int live = 0;
    static inline int move_budget = -1;
    int value = 0;

    explicit ThrowingMoveOnly(int n = 0) : value(n) { ++live; }
    ThrowingMoveOnly(const ThrowingMoveOnly&) = delete;
    ThrowingMoveOnly& operator=(const ThrowingMoveOnly&) = delete;

    static void consume_move() {
        if (move_budget == 0) throw ConstructionFailure{};
        if (move_budget > 0) --move_budget;
    }

    ThrowingMoveOnly(ThrowingMoveOnly&& other) noexcept(false) : value(other.value) {
        consume_move();
        other.value = -1;
        ++live;
    }

    ThrowingMoveOnly& operator=(ThrowingMoveOnly&& other) noexcept(false) {
        consume_move();
        value = other.value;
        other.value = -1;
        return *this;
    }

    ~ThrowingMoveOnly() { --live; }
};

struct DisarmMoveOnExit {
    ~DisarmMoveOnExit() { ThrowingMoveOnly::move_budget = -1; }
};

}  // namespace

TEST_CASE("SmallVector keeps throwing move only elements destructible after failed growth", "[small_vector][move][exception]") {
    REQUIRE(ThrowingMoveOnly::live == 0);
    DisarmMoveOnExit reset;
    for (int operation = 0; operation < 4; ++operation) {
        for (int budget = 0; budget < 2; ++budget) {
            CAPTURE(operation, budget);
            {
                tfl::SmallVector<ThrowingMoveOnly, 2> values;
                values.emplace_back(10);
                values.emplace_back(20);
                const auto* old_data = values.data();
                const auto old_capacity = values.capacity();
                const auto old_size = values.size();
                ThrowingMoveOnly::move_budget = budget;
                if (operation == 0) {
                    REQUIRE_THROWS_AS(values.reserve(old_capacity + 5), ConstructionFailure);
                } else if (operation == 1) {
                    REQUIRE_THROWS_AS(values.resize(7), ConstructionFailure);
                } else if (operation == 2) {
                    REQUIRE_THROWS_AS(values.emplace_back(77), ConstructionFailure);
                } else {
                    REQUIRE_THROWS_AS(values.emplace(values.begin() + 1, 77), ConstructionFailure);
                }
                ThrowingMoveOnly::move_budget = -1;
                CHECK(values.data() == old_data);
                CHECK(values.capacity() == old_capacity);
                CHECK(values.size() == old_size);
                CHECK(ThrowingMoveOnly::live == static_cast<int>(values.size()));
                // Successful moves preceding the failure may change old values.
                // All existing objects must still be destroyable and reusable.
                values.clear();
                CHECK(ThrowingMoveOnly::live == 0);
                values.emplace_back(42);
                CHECK(values.front().value == 42);
                CHECK(ThrowingMoveOnly::live == 1);
            }
            CHECK(ThrowingMoveOnly::live == 0);
        }
    }
}
