#include "catch_amalgamated.hpp"
#include <taskflowlite/core/small_vector.hpp>

#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {

template <typename T, std::size_t N>
void expect_values(const tfl::SmallVector<T, N>& actual, std::initializer_list<T> expected) {
    REQUIRE(std::ranges::equal(actual, expected));
}

enum class BaseKind { Void, Self, ThrowingPointer };

template <BaseKind Kind>
struct BaseIterator {
    using value_type = int;
    using difference_type = std::ptrdiff_t;
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;

    int* pointer{};
    inline static int base_calls = 0;

    int& operator*() const noexcept { return *pointer; }
    BaseIterator& operator++() noexcept { ++pointer; return *this; }
    BaseIterator operator++(int) noexcept {
        auto previous = *this;
        ++*this;
        return previous;
    }
    friend bool operator==(const BaseIterator&, const BaseIterator&) = default;

    auto base() const {
        ++base_calls;
        if constexpr (Kind == BaseKind::Void) {
            return;
        }
        else if constexpr (Kind == BaseKind::Self) {
            return *this;
        }
        else {
            if (base_calls > 0) {
                throw std::runtime_error("iterator base failed");
            }
            return pointer;
        }
    }
};

static_assert(std::forward_iterator<BaseIterator<BaseKind::Void>>);
static_assert(std::forward_iterator<BaseIterator<BaseKind::Self>>);
static_assert(std::forward_iterator<BaseIterator<BaseKind::ThrowingPointer>>);

template <BaseKind Kind>
void check_unrelated_base_is_ignored() {
    using It = BaseIterator<Kind>;
    It::base_calls = 0;
    int source[] = {7, 8, 9};
    tfl::SmallVector<int> values;
    values.append(It{source}, It{source + 3});
    expect_values(values, {7, 8, 9});
    values.assign(It{source}, It{source + 2});
    expect_values(values, {7, 8});
    values.insert(values.begin() + 1, It{source + 2}, It{source + 3});
    expect_values(values, {7, 9, 8});
    REQUIRE(It::base_calls == 0);
}

} // namespace

TEST_CASE("SmallVector zero-inline empty assignment terminates", "[small_vector][ranges][regression]") {
    tfl::SmallVector<int, 0> values;
    tfl::SmallVector<int, 0> empty;

    SECTION("copy from another empty container") {
        values = empty;
    }
    SECTION("assign own empty iterator pair") {
        values.assign(values.begin(), values.end());
    }
    SECTION("assign another empty iterator pair") {
        values.assign(empty.begin(), empty.end());
    }
    SECTION("assign null empty pair explicitly") {
        values.assign(static_cast<int*>(nullptr), static_cast<int*>(nullptr));
    }
    SECTION("empty range clears populated container") {
        values.append({1, 2});
        values.assign(empty.begin(), empty.end());
    }
    REQUIRE(values.empty());
    values.push_back(42);
    expect_values(values, {42});
}

TEST_CASE("SmallVector accepts volatile input pointers", "[small_vector][ranges][regression]") {
    volatile int source[] = {7, 8, 9};
    static_assert(std::input_iterator<volatile int*>);
    tfl::SmallVector<int> values{1, 2};

    SECTION("append") {
        values.append(source, source + 3);
        expect_values(values, {1, 2, 7, 8, 9});
    }
    SECTION("assign") {
        values.assign(source, source + 3);
        expect_values(values, {7, 8, 9});
    }
    SECTION("insert") {
        values.insert(values.begin() + 1, source, source + 3);
        expect_values(values, {1, 7, 8, 9, 2});
    }
}

TEST_CASE("SmallVector accepts wide iota difference types", "[small_vector][ranges][regression]") {
    auto range = std::views::iota(0ULL, 3ULL);
    SECTION("iterator constructor") {
        tfl::SmallVector<unsigned long long> values(range.begin(), range.end());
        expect_values(values, {0ULL, 1ULL, 2ULL});
    }
    SECTION("append range") {
        tfl::SmallVector<unsigned long long> values{8ULL};
        values.append_range(range);
        expect_values(values, {8ULL, 0ULL, 1ULL, 2ULL});
    }
    SECTION("assign range") {
        tfl::SmallVector<unsigned long long> values{8ULL};
        values.assign_range(range);
        expect_values(values, {0ULL, 1ULL, 2ULL});
    }
    SECTION("insert range") {
        tfl::SmallVector<unsigned long long> values{8ULL};
        values.insert_range(values.begin(), range);
        expect_values(values, {0ULL, 1ULL, 2ULL, 8ULL});
    }
}

TEST_CASE("SmallVector recognizes counted self ranges", "[small_vector][ranges][regression]") {
    tfl::SmallVector<int, 4> values{1, 2, 3, 4};

    SECTION("self insert within existing capacity") {
        values.reserve(12);
        values.insert(values.begin() + 1,
                      std::counted_iterator(values.begin(), 2), std::default_sentinel);
        expect_values(values, {1, 1, 2, 2, 3, 4});
    }
    SECTION("self insert with allocation") {
        values.insert(values.begin() + 1,
                      std::counted_iterator(values.begin(), 2), std::default_sentinel);
        expect_values(values, {1, 1, 2, 2, 3, 4});
    }
    SECTION("self append with allocation") {
        values.append(std::counted_iterator(values.begin(), 3), std::default_sentinel);
        expect_values(values, {1, 2, 3, 4, 1, 2, 3});
    }
    SECTION("reverse counted assignment") {
        values.assign(std::counted_iterator(values.rbegin(), 4), std::default_sentinel);
        expect_values(values, {4, 3, 2, 1});
    }
    SECTION("ordinary reverse assignment") {
        values.assign(values.rbegin(), values.rend());
        expect_values(values, {4, 3, 2, 1});
    }
}

TEST_CASE("SmallVector forwards move-only input iterators", "[small_vector][ranges][regression]") {
    std::istringstream stream("7 8 9");
    auto range = std::ranges::istream_view<int>(stream);
    static_assert(std::input_iterator<decltype(range.begin())>);
    static_assert(!std::copy_constructible<decltype(range.begin())>);

    SECTION("iterator constructor") {
        tfl::SmallVector<int> values(range.begin(), range.end());
        expect_values(values, {7, 8, 9});
    }
    SECTION("append iterator pair") {
        tfl::SmallVector<int> values{1, 2};
        values.append(range.begin(), range.end());
        expect_values(values, {1, 2, 7, 8, 9});
    }
    SECTION("assign iterator pair") {
        tfl::SmallVector<int> values{1, 2};
        values.assign(range.begin(), range.end());
        expect_values(values, {7, 8, 9});
    }
    SECTION("insert iterator pair") {
        tfl::SmallVector<int> values{1, 2};
        values.insert(values.begin() + 1, range.begin(), range.end());
        expect_values(values, {1, 7, 8, 9, 2});
    }
    SECTION("append range wrapper") {
        tfl::SmallVector<int> values{1};
        values.append_range(range);
        expect_values(values, {1, 7, 8, 9});
    }
    SECTION("assign range wrapper") {
        tfl::SmallVector<int> values{1};
        values.assign_range(range);
        expect_values(values, {7, 8, 9});
    }
    SECTION("insert range wrapper") {
        tfl::SmallVector<int> values{1, 2};
        values.insert_range(values.begin() + 1, range);
        expect_values(values, {1, 7, 8, 9, 2});
    }
}

TEST_CASE("SmallVector ignores unrelated iterator base members", "[small_vector][ranges][regression]") {
    SECTION("void-returning base") {
        check_unrelated_base_is_ignored<BaseKind::Void>();
    }
    SECTION("same-type base") {
        check_unrelated_base_is_ignored<BaseKind::Self>();
    }
}

TEST_CASE("SmallVector propagates iterator base exceptions", "[small_vector][ranges][regression]") {
    using It = BaseIterator<BaseKind::ThrowingPointer>;
    It::base_calls = 0;
    int source[] = {7, 8};
    tfl::SmallVector<int> values{1, 2};

    SECTION("append") {
        REQUIRE_THROWS_AS(values.append(It{source}, It{source + 2}), std::runtime_error);
    }
    SECTION("assign") {
        REQUIRE_THROWS_AS(values.assign(It{source}, It{source + 2}), std::runtime_error);
    }
    SECTION("insert") {
        REQUIRE_THROWS_AS(values.insert(values.begin(), It{source}, It{source + 2}), std::runtime_error);
    }
    expect_values(values, {1, 2});
}

namespace {

struct ConversionSource {
    int n;
};

struct TrivialConversionTarget {
    int n;

    TrivialConversionTarget() = default;
    TrivialConversionTarget(const TrivialConversionTarget&) = default;
    TrivialConversionTarget(TrivialConversionTarget&&) = default;
    TrivialConversionTarget& operator=(const TrivialConversionTarget&) = default;
    TrivialConversionTarget& operator=(TrivialConversionTarget&&) = default;

    TrivialConversionTarget(ConversionSource source) : n(source.n + 100) {}
    TrivialConversionTarget& operator=(ConversionSource source) {
        n = source.n + 200;
        return *this;
    }
};

static_assert(std::is_trivially_copyable_v<TrivialConversionTarget>);
static_assert(std::is_trivially_move_constructible_v<TrivialConversionTarget>);
static_assert(std::is_trivially_destructible_v<TrivialConversionTarget>);

} // namespace

TEST_CASE("SmallVector constructs new trivial slots from different source types", "[small_vector][ranges][regression]") {
    using T = TrivialConversionTarget;
    ConversionSource source[] = {{1}, {2}, {3}};

    SECTION("insert into empty inline storage calls converting constructor") {
        tfl::SmallVector<T, 4> values;
        values.insert(values.end(), std::begin(source), std::begin(source) + 1);
        REQUIRE(values.size() == 1);
        REQUIRE(values[0].n == 101);
    }
    SECTION("insert at end calls converting constructor") {
        tfl::SmallVector<T, 4> values;
        values.emplace_back(ConversionSource{10});
        values.insert(values.end(), std::begin(source), std::begin(source) + 1);
        REQUIRE(values.size() == 2);
        REQUIRE(values[0].n == 110);
        REQUIRE(values[1].n == 101);
    }
    SECTION("middle insert assigns live slots and constructs added slots") {
        tfl::SmallVector<T, 8> values;
        values.emplace_back(ConversionSource{10});
        values.emplace_back(ConversionSource{20});
        values.insert(values.begin() + 1, std::begin(source), std::end(source));
        REQUIRE(values.size() == 5);
        REQUIRE(values[0].n == 110);
        REQUIRE(values[1].n == 201);
        REQUIRE(values[2].n == 102);
        REQUIRE(values[3].n == 103);
        REQUIRE(values[4].n == 120);
    }
}
