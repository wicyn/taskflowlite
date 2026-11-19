/// @brief Stacklet LIFO allocation and worker-local cache ownership.
#include "test_common.hpp"
#include <cstdint>
#include <limits>

TEST_CASE("Stacklet: default new alignment", "[stack]") {
    static_assert(tfl::Stacklet::alignment == __STDCPP_DEFAULT_NEW_ALIGNMENT__);

    tfl::Stack stack;
    auto* segment = stack.acquire(128);

    void* a = segment->try_allocate(1);
    void* b = segment->try_allocate(7);
    void* c = segment->try_allocate(17);

    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE(c != nullptr);

    constexpr auto alignment = tfl::Stacklet::alignment;
    REQUIRE(reinterpret_cast<std::uintptr_t>(a) % alignment == 0);
    REQUIRE(reinterpret_cast<std::uintptr_t>(b) % alignment == 0);
    REQUIRE(reinterpret_cast<std::uintptr_t>(c) % alignment == 0);

    segment->deallocate(c);
    segment->deallocate(b);
    segment->deallocate(a);

    REQUIRE(segment->empty());
    stack.release(segment);
}

TEST_CASE("Stack: aligned LIFO allocation restores capacity", "[stack][coro]") {
    tfl::Stack stack;
    auto* segment = stack.acquire(1);
    REQUIRE(segment->empty());
    const auto capacity = segment->capacity();
    auto* first = segment->try_allocate(1);
    auto* second = segment->try_allocate(17);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(reinterpret_cast<std::uintptr_t>(first) % tfl::Stacklet::alignment == 0);
    REQUIRE(reinterpret_cast<std::uintptr_t>(second) % tfl::Stacklet::alignment == 0);
    const auto available = segment->available();
    REQUIRE(segment->try_allocate(available + 1) == nullptr);
    REQUIRE(segment->available() == available);
    segment->deallocate(second);
    segment->deallocate(first);
    REQUIRE(segment->available() == capacity);
    stack.release(segment);
    REQUIRE(stack.try_acquire() == segment);
    REQUIRE(stack.try_acquire() == nullptr);
    stack.release(segment);
}

TEST_CASE("Stack: expanded segments detach before release and reject overflow", "[stack][coro]") {
    tfl::Stack stack;
    auto* root = stack.acquire(1);
    auto* large = stack.acquire(tfl::Stacklet::default_size * 2);
    REQUIRE(large->capacity() >= tfl::Stacklet::default_size * 2);
    REQUIRE(root->push(large) == large);
    REQUIRE(large->previous() == root);
    REQUIRE(large->pop() == root);
    REQUIRE(large->previous() == nullptr);
    stack.release(large);
    REQUIRE(stack.try_acquire() == nullptr); // Oversized segments are not cached.
    stack.release(root);
    REQUIRE_THROWS_AS(stack.acquire(std::numeric_limits<std::size_t>::max()), std::bad_alloc);
}
