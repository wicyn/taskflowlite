/// @file work_factory_fwd.hpp
/// @brief Work 节点工厂函数前置声明。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-08-16
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <utility>

#include "traits.hpp"
#include "result_slot.hpp"

namespace tfl {

// ============================================================================
// Graph 内同步节点
// ============================================================================

[[nodiscard]] Work* make_placeholder(const Graph* graph);

template <typename F, typename... Args>
    requires (basic_invocable<F, Args...> && capturable<F, Args...>)
[[nodiscard]] Work* make_basic(const Graph* graph, F&& func, Args&&... args);

template <typename F, typename... Args>
    requires (branch_invocable<F, Args...> && capturable<F, Args...>)
[[nodiscard]] Work* make_branch(const Graph* graph, F&& func, Args&&... args);

template <typename F, typename... Args>
    requires (multi_branch_invocable<F, Args...> && capturable<F, Args...>)
[[nodiscard]] Work* make_multi_branch(const Graph* graph, F&& func, Args&&... args);

template <typename F, typename... Args>
    requires (jump_invocable<F, Args...> && capturable<F, Args...>)
[[nodiscard]] Work* make_jump(const Graph* graph, F&& func, Args&&... args);

template <typename F, typename... Args>
    requires (multi_jump_invocable<F, Args...> && capturable<F, Args...>)
[[nodiscard]] Work* make_multi_jump(const Graph* graph, F&& func, Args&&... args);

template <typename F, typename... Args>
    requires (runtime_invocable<F, Args...> && capturable<F, Args...>)
[[nodiscard]] Work* make_runtime(const Graph* graph, F&& func, Args&&... args);

template <typename F, typename... Args>
    requires (subflow_invocable<F, Args...> && capturable<F, Args...>)
[[nodiscard]] Work* make_subflow(const Graph* graph, F&& func, Args&&... args);

template <graph_holder Gh, predicate P>
    requires capturable<P>
[[nodiscard]] Work* make_module(const Graph* graph, Gh&& graph_holder, P&& pred);

// ============================================================================
// Detached 异步节点
// ============================================================================

template <typename F, typename... Args>
    requires (basic_invocable<F, Args...> && capturable<F, Args...>)
[[nodiscard]] Work* make_detached_basic(Executor& executor, Work* parent, Topology* parent_topology, F&& func, Args&&... args);

template <typename F, typename... Args>
    requires (runtime_invocable<F, Args...> && capturable<F, Args...>)
[[nodiscard]] Work* make_detached_runtime(Executor& executor, Work* parent, Topology* parent_topology, F&& func, Args&&... args);

template <typename F, typename... Args>
    requires (subflow_invocable<F, Args...> && capturable<F, Args...>)
[[nodiscard]] Work* make_detached_subflow(Executor& executor, Work* parent, Topology* parent_topology, F&& func, Args&&... args);

template <graph_holder Gh, predicate P, callback C>
    requires capturable<P, C>
[[nodiscard]] Work* make_detached_module(Executor& executor, Work* parent, Topology* parent_topology, Gh&& graph_holder, P&& pred, C&& callback);

// ============================================================================
// Joinable 异步节点
// ============================================================================

template <typename F, typename... Args>
    requires (basic_invocable<F, Args...> && capturable<F, Args...>)
[[nodiscard]] std::pair<Work*, ResultSlot<basic_return_t<F, Args...>>*> make_joinable_basic(Executor& executor, Work* parent, Topology* parent_topology, F&& func, Args&&... args);

template <typename F, typename... Args>
    requires (runtime_invocable<F, Args...> && capturable<F, Args...>)
[[nodiscard]] std::pair<Work*, ResultSlot<runtime_return_t<F, Args...>>*> make_joinable_runtime(Executor& executor, Work* parent, Topology* parent_topology, F&& func, Args&&... args);

template <typename F, typename... Args>
    requires (subflow_invocable<F, Args...> && capturable<F, Args...>)
[[nodiscard]] std::pair<Work*, ResultSlot<subflow_return_t<F, Args...>>*> make_joinable_subflow(Executor& executor, Work* parent, Topology* parent_topology, F&& func, Args&&... args);

template <graph_holder Gh, predicate P, callback C>
    requires capturable<P, C>
[[nodiscard]] std::pair<Work*, ResultSlot<void>*> make_joinable_module(Executor& executor, Work* parent, Topology* parent_topology, Gh&& graph_holder, P&& pred, C&& callback);

// ============================================================================
// Attached 异步节点
// ============================================================================

template <typename F, typename... Args>
    requires (basic_invocable<F, Args...> && capturable<F, Args...>)
[[nodiscard]] std::pair<Work*, ResultSlot<basic_return_t<F, Args...>>*> make_attached_basic(F&& func, Args&&... args);

template <typename F, typename... Args>
    requires (runtime_invocable<F, Args...> && capturable<F, Args...>)
[[nodiscard]] std::pair<Work*, ResultSlot<runtime_return_t<F, Args...>>*> make_attached_runtime(F&& func, Args&&... args);

template <typename F, typename... Args>
    requires (subflow_invocable<F, Args...> && capturable<F, Args...>)
[[nodiscard]] std::pair<Work*, ResultSlot<subflow_return_t<F, Args...>>*> make_attached_subflow(F&& func, Args&&... args);

template <graph_holder Gh, predicate P, callback C>
    requires capturable<P, C>
[[nodiscard]] std::pair<Work*, ResultSlot<void>*> make_attached_module(Gh&& graph_holder, P&& pred, C&& callback);

}  // namespace tfl
