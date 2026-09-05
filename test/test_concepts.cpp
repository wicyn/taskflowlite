/// @file test_concepts.cpp
/// @brief Compile-time concept verification — accept models, reject non-models.
///
/// All callable types are named structs (not inline lambdas) to avoid relying on
/// C++20 "lambdas in unevaluated contexts" (P0315R4), which is not available in
/// every compiler configuration used by this project.
///
/// concepts tested:
///   predicate<P, Args...>       ✓ (models + non-models)
///   noexcept_predicate<P, Args> ✓ (models + non-models)
///   callback<C>                 ✓ (models + non-models)
///   capturable<Ts...>           ✓ (models + non-models)
///   basic_invocable<T> ✓
///   branch_invocable<T>   ✓
///   runtime_invocable<T>  ✓
///   jump_invocable<T>     ✓
///   multi_branch / multi_jump   ✓
///   graph_holder<Gh>            ✓
///   task_pack<T>                ✓
///   sem_count_sequence<Ts...>   ✓
///   async_future / async_task               ✓
///   detail::is_reference_wrapper ✓
///   detail::capture / borrow       ✓
///   pack CTAD                   ✓

#include "test_common.hpp"

#include <functional>
#include <memory>
#include <stop_token>
#include <type_traits>

using tfl_test::TestEnv;

// ============================================================================
// Named callable types — avoid C++20 lambda-in-decltype requirement
// ============================================================================

namespace {

struct EmptyCallable {
    void operator()() const {}
};

struct NoArgPred {
    bool operator()() const { return false; }
};

struct IntPred {
    bool operator()(int) const { return true; }
};

struct IntNoexceptPred {
    bool operator()(int) const noexcept { return true; }
};

struct IntThrowingPred {
    bool operator()(int) const { return true; }  // noexcept(false) implicitly
};

struct IntReturn {
    int operator()(int) const { return 0; }
};

struct StopCallable {
    void operator()(std::stop_token) const {}
};

struct IntStopCallable {
    void operator()(int, std::stop_token) const {}
};

struct BranchCallable {
    void operator()(tfl::Branch&) const {}
};

struct RuntimeCallable {
    void operator()(tfl::Runtime&) const {}
};

struct JumpCallable {
    void operator()(tfl::Jump&) const {}
};

struct MultiBranchCallable {
    void operator()(tfl::MultiBranch&) const {}
};

struct MultiJumpCallable {
    void operator()(tfl::MultiJump&) const {}
};

struct IntBranchCallable {
    void operator()(int, tfl::Branch&) const {}
};

struct IntRuntimeCallable {
    void operator()(int, tfl::Runtime&) const {}
};

struct IntJumpCallable {
    void operator()(int, tfl::Jump&) const {}
};

struct NotCallable {};

}  // namespace

// ============================================================================
// SECTION 1: predicate concept (P1)
// ============================================================================

/// @test [concepts][predicate] predicate accepts valid types, rejects invalid ones
TEST_CASE("Concepts: predicate<P, Args...>", "[concepts][predicate]") {
    // Models
    STATIC_REQUIRE(tfl::predicate<IntPred, int>);
    STATIC_REQUIRE(tfl::predicate<bool(*)(int), int>);
    STATIC_REQUIRE(tfl::predicate<std::less<>, int, int>);
    STATIC_REQUIRE(tfl::predicate<NoArgPred>);   // no args

    // Non-models
    STATIC_REQUIRE_FALSE(tfl::predicate<NotCallable, int>);
    STATIC_REQUIRE_FALSE(tfl::predicate<IntReturn, int>);  // returns int not bool
}

// ============================================================================
// SECTION 2: noexcept_predicate concept (P2)
// ============================================================================

/// @test [concepts][noexcept_predicate] noexcept predicate
TEST_CASE("Concepts: noexcept_predicate<P, Args...>", "[concepts][noexcept_predicate]") {
    STATIC_REQUIRE(tfl::noexcept_predicate<IntNoexceptPred, int>);
    STATIC_REQUIRE_FALSE(tfl::noexcept_predicate<IntThrowingPred, int>);
}

// ============================================================================
// SECTION 3: callback concept (P2)
// ============================================================================

/// @test [concepts][callback] callback concept
TEST_CASE("Concepts: callback<C>", "[concepts][callback]") {
    STATIC_REQUIRE(tfl::callback<EmptyCallable>);
    STATIC_REQUIRE(tfl::callback<void(*)()>);
    STATIC_REQUIRE_FALSE(tfl::callback<NotCallable>);
}

// ============================================================================
// SECTION 4: capturable concept (P1 — critical)
// ============================================================================

/// @test [concepts][capturable] capturable: values, refs, unique_ptr lvalue rejection
TEST_CASE("Concepts: capturable<Ts...>", "[concepts][capturable]") {
    // Models: values
    STATIC_REQUIRE(tfl::capturable<int>);
    STATIC_REQUIRE(tfl::capturable<int, double>);
    STATIC_REQUIRE(tfl::capturable<std::string>);

    // Models: rvalue (movable)
    STATIC_REQUIRE(tfl::capturable<std::unique_ptr<int>>);

    // Models: reference_wrapper
    STATIC_REQUIRE(tfl::capturable<std::reference_wrapper<int>>);

    // Models: rvalue of callable (move-constructible)
    STATIC_REQUIRE(tfl::capturable<EmptyCallable>);

    // Non-model: lvalue of non-copyable object
    using UPtrLRef = std::unique_ptr<int>&;
    STATIC_REQUIRE_FALSE(tfl::capturable<UPtrLRef>);
}

// ============================================================================
// SECTION 5: basic_invocable concept
// ============================================================================

/// @test [concepts][invocable] basic_invocable
TEST_CASE("Concepts: basic_invocable<T>", "[concepts][invocable]") {
    // Plain — no args
    STATIC_REQUIRE(tfl::basic_invocable<EmptyCallable>);

    // Plain — with args
    STATIC_REQUIRE(tfl::basic_invocable<decltype(std::bind_front(IntReturn{}, 1))>);

    // stop_token 不再自动注入，只有 token 参数的 callable 不满足当前接口。
    STATIC_REQUIRE_FALSE(tfl::basic_invocable<IntStopCallable>);
    STATIC_REQUIRE_FALSE(tfl::basic_invocable<StopCallable>);

    // Non-model — wrong arity
    STATIC_REQUIRE_FALSE(tfl::basic_invocable<IntReturn>);  // expects int arg
}

// ============================================================================
// SECTION 6: branch_invocable concept
// ============================================================================

/// @test [concepts][invocable] branch_invocable
TEST_CASE("Concepts: branch_invocable<T>", "[concepts][invocable]") {
    STATIC_REQUIRE(tfl::branch_invocable<BranchCallable>);
    STATIC_REQUIRE(tfl::branch_invocable<decltype(std::bind_front(IntBranchCallable{}, 1))>);
    STATIC_REQUIRE_FALSE(tfl::branch_invocable<EmptyCallable>);
}

// ============================================================================
// SECTION 7: runtime_invocable concept
// ============================================================================

/// @test [concepts][invocable] runtime_invocable
TEST_CASE("Concepts: runtime_invocable<T>", "[concepts][invocable]") {
    STATIC_REQUIRE(tfl::runtime_invocable<RuntimeCallable>);
    STATIC_REQUIRE(tfl::runtime_invocable<decltype(std::bind_front(IntRuntimeCallable{}, 1))>);
    STATIC_REQUIRE_FALSE(tfl::runtime_invocable<EmptyCallable>);
}

// ============================================================================
// SECTION 8: jump_invocable concept
// ============================================================================

/// @test [concepts][invocable] jump_invocable
TEST_CASE("Concepts: jump_invocable<T>", "[concepts][invocable]") {
    STATIC_REQUIRE(tfl::jump_invocable<JumpCallable>);
    STATIC_REQUIRE(tfl::jump_invocable<decltype(std::bind_front(IntJumpCallable{}, 1))>);
    STATIC_REQUIRE_FALSE(tfl::jump_invocable<EmptyCallable>);
}

// ============================================================================
// SECTION 9: multi_branch_invocable / multi_jump_invocable
// ============================================================================

/// @test [concepts][invocable] multi_branch / multi_jump invocable
TEST_CASE("Concepts: multi_branch_invocable / multi_jump_invocable", "[concepts][invocable]") {
    STATIC_REQUIRE(tfl::multi_branch_invocable<MultiBranchCallable>);
    STATIC_REQUIRE(tfl::multi_jump_invocable<MultiJumpCallable>);
    STATIC_REQUIRE_FALSE(tfl::multi_branch_invocable<EmptyCallable>);
}

// ============================================================================
// SECTION 10: graph_holder concept
// ============================================================================

/// @test [concepts][graph_holder] graph_holder accepts Flow and derivatives
TEST_CASE("Concepts: graph_holder<Gh>", "[concepts][graph_holder]") {
    STATIC_REQUIRE(tfl::graph_holder<tfl::Flow>);
    STATIC_REQUIRE(tfl::graph_holder<tfl::Flow&>);
    STATIC_REQUIRE(tfl::graph_holder<const tfl::Flow>);

    STATIC_REQUIRE_FALSE(tfl::graph_holder<int>);
    STATIC_REQUIRE_FALSE(tfl::graph_holder<std::string>);
}

// ============================================================================
// SECTION 11: task_pack concept
// ============================================================================

/// @test [concepts][task_pack] task_pack only accepts tfl::pack specializations
TEST_CASE("Concepts: task_pack<T>", "[concepts][task_pack]") {
    STATIC_REQUIRE(tfl::task_pack<tfl::pack<int, int>>);
    STATIC_REQUIRE(tfl::task_pack<tfl::pack<EmptyCallable>>);

    // Non-model
    STATIC_REQUIRE_FALSE(tfl::task_pack<std::tuple<int, int>>);
    STATIC_REQUIRE_FALSE(tfl::task_pack<int>);
}

// ============================================================================
// SECTION 12: sem_count_sequence concept
// ============================================================================

/// @test [concepts][semaphore] sem_count_sequence alternates Semaphore&, size_t
TEST_CASE("Concepts: sem_count_sequence<Ts...>", "[concepts][semaphore]") {
    // Models
    STATIC_REQUIRE(tfl::sem_count_sequence<tfl::Semaphore&, std::size_t>);
    STATIC_REQUIRE(tfl::sem_count_sequence<tfl::Semaphore&, std::size_t,
                                            tfl::Semaphore&, std::size_t>);

    // Non-models
    STATIC_REQUIRE_FALSE(tfl::sem_count_sequence<tfl::Semaphore&, std::string>);
    STATIC_REQUIRE_FALSE(tfl::sem_count_sequence<int, std::size_t>);
    STATIC_REQUIRE_FALSE(tfl::sem_count_sequence<tfl::Semaphore&>);
}

// ============================================================================
// SECTION 13: async_future / async_task concepts
// ============================================================================

/// @test [concepts][async] Future 和延迟任务分别满足对应约束。
TEST_CASE("Concepts: async_future and async_task", "[concepts][async]") {
    STATIC_REQUIRE(tfl::async_future<tfl::AsyncFuture<int>>);
    STATIC_REQUIRE(tfl::async_future<const tfl::AsyncTask<void>&>);
    STATIC_REQUIRE(tfl::async_task<tfl::AsyncTask<int>&>);
    STATIC_REQUIRE_FALSE(tfl::async_task<tfl::AsyncFuture<int>>);
    STATIC_REQUIRE_FALSE(tfl::async_future<int>);
    STATIC_REQUIRE_FALSE(tfl::async_task<tfl::Task>);
}

// ============================================================================
// SECTION 14: detail utilities — reference_wrapper detection
// ============================================================================

/// @test [concepts][detail] detail::is_reference_wrapper_v
TEST_CASE("Concepts: detail::is_reference_wrapper_v", "[concepts][detail]") {
    int x = 0;
    auto ref = std::ref(x);
    STATIC_REQUIRE(tfl::detail::is_reference_wrapper_v<decltype(ref)>);
    STATIC_REQUIRE_FALSE(tfl::detail::is_reference_wrapper_v<int>);
    STATIC_REQUIRE_FALSE(tfl::detail::is_reference_wrapper_v<int&>);
}

/// @test [concepts][detail] detail::is_reference_wrapper_v
TEST_CASE("Concepts: reference_wrapper detection removes cvref", "[concepts][detail]") {
    STATIC_REQUIRE(tfl::detail::is_reference_wrapper_v<const std::reference_wrapper<int>&>);
    STATIC_REQUIRE_FALSE(tfl::detail::is_reference_wrapper_v<int&>);
}

// ============================================================================
// SECTION 15: detail::capture / borrow runtime functions
// ============================================================================

/// @test [concepts][detail] detail::capture 与 detail::borrow
TEST_CASE("Concepts: detail::capture/borrow runtime", "[concepts][detail]") {
    SECTION("wrap lvalue produces reference_wrapper") {
        int x = 42;
        auto w = tfl::detail::capture(x);
        STATIC_REQUIRE(std::same_as<decltype(w), std::reference_wrapper<int>>);
        REQUIRE(w.get() == 42);
    }

    SECTION("wrap rvalue forwards as-is") {
        auto w = tfl::detail::capture(42);
        STATIC_REQUIRE(std::same_as<decltype(w), int>);
        REQUIRE(w == 42);
    }

    SECTION("borrow reference_wrapper returns reference") {
        int x = 100;
        auto ref = std::ref(x);
        decltype(auto) u = tfl::detail::borrow(ref);
        STATIC_REQUIRE(std::same_as<decltype(u), int&>);
        REQUIRE(u == 100);
    }

    SECTION("borrow preserves const lvalue references") {
        const int value = 42;
        STATIC_REQUIRE(std::same_as<decltype(tfl::detail::borrow(value)), const int&>);
        REQUIRE(&tfl::detail::borrow(value) == &value);
    }
}

// ============================================================================
// SECTION 16: pack<Ts...> deduction guide (CTAD)
// ============================================================================

/// @test [concepts][pack] pack CTAD
TEST_CASE("Concepts: pack CTAD deduction", "[concepts][pack]") {
    int x = 0;
    auto my_pack = tfl::pack{std::ref(x), 42, std::string("hello")};

    // CTAD deduces: pack<reference_wrapper<int>, int, std::string>
    using PackType = decltype(my_pack);
    STATIC_REQUIRE(std::same_as<PackType,
        tfl::pack<std::reference_wrapper<int>, int, std::string>>);

    // Runtime: inner tuple elements stored correctly
    REQUIRE(std::get<0>(my_pack.data).get() == 0);
    REQUIRE(std::get<1>(my_pack.data) == 42);
    REQUIRE(std::get<2>(my_pack.data) == "hello");

    SUCCEED("CTAD succeeded");
}

// ============================================================================
// 覆盖矩阵
// ============================================================================
//   predicate             ✓ "Concepts: predicate<P, Args...>"
//   noexcept_predicate    ✓ "Concepts: noexcept_predicate<P, Args...>"
//   callback              ✓ "Concepts: callback<C>"
//   capturable            ✓ "Concepts: capturable<Ts...>"
//   basic_invocable       ✓ "Concepts: basic_invocable<T>"
//   branch_invocable      ✓ "Concepts: branch_invocable<T>"
//   multi_branch_invocable ✓ "Concepts: multi_branch_invocable / multi_jump_invocable"
//   jump_invocable        ✓ "Concepts: jump_invocable<T>"
//   multi_jump_invocable  ✓ "Concepts: multi_branch_invocable / multi_jump_invocable"
//   runtime_invocable     ✓ "Concepts: runtime_invocable<T>"
//   graph_holder          ✓ "Concepts: graph_holder<Gh>"
//   task_pack             ✓ "Concepts: task_pack<T>"
//   sem_count_sequence    ✓ "Concepts: sem_count_sequence<Ts...>"
//   async_future/async_task ✓ "Concepts: async_future and async_task"
//   detail::is_reference_wrapper ✓ "Concepts: detail::is_reference_wrapper_v"
//   detail::capture/borrow    ✓ "Concepts: detail::capture/borrow runtime"
//   pack CTAD              ✓ "Concepts: pack CTAD deduction"
