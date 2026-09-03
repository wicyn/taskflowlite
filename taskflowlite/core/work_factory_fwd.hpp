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

template <typename F>
    requires (basic_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_basic(const Graph* graph, F&& func);

template <typename F>
    requires (branch_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_branch(const Graph* graph, F&& func);

template <typename F>
    requires (multi_branch_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_multi_branch(const Graph* graph, F&& func);

template <typename F>
    requires (jump_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_jump(const Graph* graph, F&& func);

template <typename F>
    requires (multi_jump_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_multi_jump(const Graph* graph, F&& func);

template <typename F>
    requires (runtime_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_runtime(const Graph* graph, F&& func);

template <typename F>
    requires (subflow_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_subflow(const Graph* graph, F&& func);

template <graph_holder Gh, predicate P>
    requires capturable<P>
[[nodiscard]] Work* make_module(const Graph* graph, Gh&& graph_holder, P&& pred);

// ============================================================================
// SilentAsync 异步节点
// ============================================================================

template <typename F>
    requires (basic_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_silent_async_basic(Executor& executor, Work* parent, Topology* parent_topology, F&& func);

template <typename F>
    requires (runtime_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_silent_async_runtime(Executor& executor, Work* parent, Topology* parent_topology, F&& func);

template <typename F>
    requires (subflow_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_silent_async_subflow(Executor& executor, Work* parent, Topology* parent_topology, F&& func);

template <graph_holder Gh, predicate P, callback C>
    requires capturable<P, C>
[[nodiscard]] Work* make_silent_async_module(Executor& executor, Work* parent, Topology* parent_topology, Gh&& graph_holder, P&& pred, C&& callback);

// ============================================================================
// Async 异步节点
// ============================================================================

template <typename F>
    requires (basic_invocable<F> && capturable<F>)
[[nodiscard]] std::pair<Work*, ResultSlot<basic_return_t<F>>*> make_async_basic(Executor& executor, Work* parent, Topology* parent_topology, F&& func);

template <typename F>
    requires (runtime_invocable<F> && capturable<F>)
[[nodiscard]] std::pair<Work*, ResultSlot<runtime_return_t<F>>*> make_async_runtime(Executor& executor, Work* parent, Topology* parent_topology, F&& func);

template <typename F>
    requires (subflow_invocable<F> && capturable<F>)
[[nodiscard]] std::pair<Work*, ResultSlot<subflow_return_t<F>>*> make_async_subflow(Executor& executor, Work* parent, Topology* parent_topology, F&& func);

template <graph_holder Gh, predicate P, callback C>
    requires capturable<P, C>
[[nodiscard]] std::pair<Work*, ResultSlot<void>*> make_async_module(Executor& executor, Work* parent, Topology* parent_topology, Gh&& graph_holder, P&& pred, C&& callback);

// ============================================================================
// AsyncTask 异步节点
// ============================================================================

template <typename F>
    requires (basic_invocable<F> && capturable<F>)
[[nodiscard]] std::pair<Work*, ResultSlot<basic_return_t<F>>*> make_async_task_basic(F&& func);

template <typename F>
    requires (runtime_invocable<F> && capturable<F>)
[[nodiscard]] std::pair<Work*, ResultSlot<runtime_return_t<F>>*> make_async_task_runtime(F&& func);

template <typename F>
    requires (subflow_invocable<F> && capturable<F>)
[[nodiscard]] std::pair<Work*, ResultSlot<subflow_return_t<F>>*> make_async_task_subflow(F&& func);

template <graph_holder Gh, predicate P, callback C>
    requires capturable<P, C>
[[nodiscard]] std::pair<Work*, ResultSlot<void>*> make_async_task_module(Gh&& graph_holder, P&& pred, C&& callback);

}  // namespace tfl
