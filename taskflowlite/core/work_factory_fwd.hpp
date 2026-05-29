/// @file work_factory_fwd.hpp
/// @brief Work 节点工厂函数族 —— 仅声明，与 work_factory.hpp 配对。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn
///

#pragma once
#include <cmath>
#include <cstring>
#include "work.hpp"
namespace tfl {

namespace detail {

/// @brief 将锚点标签 A 映射为对应的 Implicit/Explicit 位掩码对。
///
/// 本函数为 consteval —— 所有锚点位均在编译期解析为常量，
/// 运行时零开销。映射规则：
///   - none_t     → {NONE, NONE}
///   - implicit_t → {ANCHORED, NONE}
///   - explicit_t → {NONE, ANCHORED}
///
/// @tparam A  锚点标签类型，必须为 anchor::none_t、anchor::implicit_t
///            或 anchor::explicit_t 之一。
template <typename A>
consteval std::pair<Work::Implicit::type, Work::Explicit::type> anchor_bits() noexcept {
    if constexpr (std::same_as<A, anchor::none_t>) {
        return {Work::Implicit::NONE,     Work::Explicit::NONE};
    } else if constexpr (std::same_as<A, anchor::implicit_t>) {
        return {Work::Implicit::ANCHORED, Work::Explicit::NONE};
    } else /* anchor::explicit_t */ {
        return {Work::Implicit::NONE,     Work::Explicit::ANCHORED};
    }
}

template <typename A>
consteval Work::Implicit::type anchor_implicit() noexcept { return anchor_bits<A>().first;  }
template <typename A>
consteval Work::Explicit::type anchor_explicit() noexcept { return anchor_bits<A>().second; }

} // namespace detail

// ============================================================================
//  前置声明 — Graph 内同步节点工厂
// ============================================================================

[[nodiscard]] inline Work* make_noop(const Graph* graph);

template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
[[nodiscard]] inline Work* make_basic(const Graph* graph, T&& t, Args&&... args);

template <typename T, typename... Args>
    requires (capturable<T, Args...> && branch_invocable<T, Args...>)
[[nodiscard]] inline Work* make_branch(const Graph* graph, T&& t, Args&&... args);

template <typename T, typename... Args>
    requires (capturable<T, Args...> && multi_branch_invocable<T, Args...>)
[[nodiscard]] inline Work* make_multi_branch(const Graph* graph, T&& t, Args&&... args);

template <typename T, typename... Args>
    requires (capturable<T, Args...> && jump_invocable<T, Args...>)
[[nodiscard]] inline Work* make_jump(const Graph* graph, T&& t, Args&&... args);

template <typename T, typename... Args>
    requires (capturable<T, Args...> && multi_jump_invocable<T, Args...>)
[[nodiscard]] inline Work* make_multi_jump(const Graph* graph, T&& t, Args&&... args);

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
[[nodiscard]] inline Work* make_runtime(const Graph* graph, T&& t, Args&&... args);

template <typename Gh, typename P>
    requires (capturable<P> && graph_holder<Gh> && predicate<P>)
[[nodiscard]] inline Work* make_subflow(const Graph* graph, Gh&& gh, P&& pred);

// ============================================================================
//  前置声明 — 独立异步任务工厂
// ============================================================================

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable_plain<T, Args...>)
[[nodiscard]] inline Work* make_detached_basic(Executor* exec, Work* parent, T&& t, Args&&... args);

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable_plain<T, Args...>)
[[nodiscard]] inline Work* make_detached_runtime(Executor* exec, Work* parent, T&& t, Args&&... args);

template <anchor_tag A, typename Gh, typename P, typename C>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
[[nodiscard]] inline Work* make_detached_flow(Executor* exec, Work* parent, Gh&& gh, P&& pred, C&& cb);

template <anchor_tag A, typename T, typename R, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
[[nodiscard]] inline Work* make_promised_basic(Executor* exec, Work* parent, T&& t, std::promise<R>&& p, Args&&... args);

template <anchor_tag A, typename T, typename R, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
[[nodiscard]] inline Work* make_promised_runtime(Executor* exec, Work* parent, T&& t, std::promise<R>&& p, Args&&... args);

template <anchor_tag A, typename Gh, typename P, typename C>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
[[nodiscard]] inline Work* make_promised_flow(Executor* exec, Work* parent, Gh&& gh, P&& pred, C&& cb, std::promise<void>&& p);
// ============================================================================
//  前置声明 — 有依赖的异步任务工厂
// ============================================================================

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
[[nodiscard]] inline Work* make_attached_basic(Executor* exec, Work* parent, T&& t, Args&&... args);

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
[[nodiscard]] inline Work* make_attached_runtime(Executor* exec, Work* parent, T&& t, Args&&... args);

template <anchor_tag A, typename Gh, typename P, typename C>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
[[nodiscard]] inline Work* make_attached_flow(Executor* exec, Work* parent, Gh&& gh, P&& pred, C&& cb);

/// @brief 销毁工作节点，直接 delete。
inline void destroy(Work* work) noexcept;
}  // namespace tfl
