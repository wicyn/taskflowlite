/// @file work_factory.hpp
/// @brief Work 节点工厂函数族。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <memory>
#include <type_traits>
#include <utility>

#include "work_factory_fwd.hpp"
#include "work_invokers.hpp"

namespace tfl {

// ============================================================================
// Graph 内同步节点
// ============================================================================

/// @brief 创建不执行用户 callable 的静态占位节点.
///
/// 节点参与普通静态图依赖传播和 join 计数，但不执行用户 callable.
///
/// @param graph 节点所属物理 Graph.
/// @return 创建完成的 Work.
[[nodiscard]] inline Work* make_placeholder(const Graph* graph) {
    return create_work(
        std::in_place_type<PlaceholderInvoker>,
        graph
        );
}

/// @brief 创建普通 callable 静态任务节点。
///
/// @tparam F callable 类型。
/// @param graph 节点所属物理 Graph。
/// @param func 用户 callable。
/// @return 创建完成的 Work。
template <typename F>
    requires (basic_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_basic(const Graph* graph, F&& func) {
    using Invoker = BasicInvoker<std::decay_t<F>>;

    return create_work(
        std::in_place_type<Invoker>,
        graph,
        std::forward<F>(func)
        );
}

/// @brief 创建选择单个后继的条件分支节点。
///
/// callable 返回单个后继索引，节点完成后仅激活对应后继。
///
/// @tparam F callable 类型。
/// @param graph 节点所属物理 Graph。
/// @param func 条件 callable。
/// @return 创建完成的 Work。
template <typename F>
    requires (branch_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_branch(const Graph* graph, F&& func) {
    using Invoker = BranchInvoker<std::decay_t<F>>;

    return create_work(
        std::in_place_type<Invoker>,
        graph,
        std::forward<F>(func)
        );
}

/// @brief 创建可选择多个后继的条件分支节点。
///
/// callable 返回多个后继索引，节点完成后激活对应后继集合。
///
/// @tparam F callable 类型。
/// @param graph 节点所属物理 Graph。
/// @param func 条件 callable。
/// @return 创建完成的 Work。
template <typename F>
    requires (multi_branch_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_multi_branch(const Graph* graph, F&& func) {
    using Invoker = MultiBranchInvoker<std::decay_t<F>>;

    return create_work(
        std::in_place_type<Invoker>,
        graph,
        std::forward<F>(func)
        );
}

/// @brief 创建选择单个后继位置的强制跳转节点。
///
/// Jump 不参与普通 JOIN 语义，由 callable 返回值直接选择跳转目标。
///
/// @tparam F callable 类型。
/// @param graph 节点所属物理 Graph。
/// @param func 跳转 callable。
/// @return 创建完成的 Work。
template <typename F>
    requires (jump_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_jump(const Graph* graph, F&& func) {
    using Invoker = JumpInvoker<std::decay_t<F>>;

    return create_work(
        std::in_place_type<Invoker>,
        graph,
        std::forward<F>(func)
        );
}

/// @brief 创建可选择多个后继位置的强制跳转节点。
///
/// MultiJump 不参与普通 JOIN 语义，由 callable 返回多个跳转目标。
///
/// @tparam F callable 类型。
/// @param graph 节点所属物理 Graph。
/// @param func 跳转 callable。
/// @return 创建完成的 Work。
template <typename F>
    requires (multi_jump_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_multi_jump(const Graph* graph, F&& func) {
    using Invoker = MultiJumpInvoker<std::decay_t<F>>;

    return create_work(
        std::in_place_type<Invoker>,
        graph,
        std::forward<F>(func)
        );
}

/// @brief 创建执行时注入 Runtime 上下文的静态任务节点。
///
/// @tparam F callable 类型。
/// @param graph 节点所属物理 Graph。
/// @param func Runtime callable。
/// @return 创建完成的 Work。
template <typename F>
    requires (runtime_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_runtime(const Graph* graph, F&& func) {
    using Invoker = RuntimeInvoker<std::decay_t<F>>;

    return create_work(
        std::in_place_type<Invoker>,
        graph,
        std::forward<F>(func)
        );
}

/// @brief 创建执行时注入 SubFlow 动态构建器的静态任务节点。
///
/// @tparam F callable 类型。
/// @param graph 节点所属物理 Graph。
/// @param func SubFlow callable。
/// @return 创建完成的 Work。
template <typename F>
    requires (subflow_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_subflow(const Graph* graph, F&& func) {
    using Invoker = SubFlowInvoker<std::decay_t<F>>;

    return create_work(
        std::in_place_type<Invoker>,
        graph,
        std::forward<F>(func)
        );
}

/// @brief 创建循环执行指定子图的静态 Module 节点。
///
/// Graph holder 按统一 capture 规则保存，predicate 决定是否终止后续迭代。
///
/// @tparam Gh Graph holder 类型。
/// @tparam P predicate 类型。
/// @param graph 节点所属物理 Graph。
/// @param graph_holder 被执行的子图持有对象。
/// @param pred 迭代终止谓词。
/// @return 创建完成的 Work。
template <graph_holder Gh, predicate P>
    requires capturable<P>
[[nodiscard]] Work* make_module(const Graph* graph, Gh&& graph_holder, P&& pred) {
    using Invoker = ModuleInvoker<detail::captured_t<Gh>, std::decay_t<P>>;

    return create_work(
        std::in_place_type<Invoker>,
        graph,
        detail::capture(std::forward<Gh>(graph_holder)),
        std::forward<P>(pred)
        );
}

// ============================================================================
// SilentAsync 异步节点
// ============================================================================

/// @brief 创建 fire-and-forget 普通异步任务。
///
/// SilentAsync 节点使用隐式归档锚点，不向调用方暴露 ResultSlot。
///
/// @tparam F callable 类型。
/// @param executor 当前任务所属 Executor。
/// @param parent 父 Work；根级异步任务允许为空。
/// @param parent_topology 新任务 Topology 的父 Topology；传 nullptr 表示独立停止域。
/// @param func 用户 callable。
/// @return 创建完成的 Work。
template <typename F>
    requires (basic_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_silent_async_basic(Executor& executor, Work* parent, Topology* parent_topology, F&& func) {
    using Invoker = SilentAsyncBasicInvoker<std::decay_t<F>>;

    return create_work(
        std::in_place_type<Invoker>,
        parent,
        parent_topology,
        std::addressof(executor),
        std::forward<F>(func)
        );
}

/// @brief 创建 fire-and-forget Runtime 异步任务。
///
/// @tparam F callable 类型。
/// @param executor 当前任务所属 Executor。
/// @param parent 父 Work；根级异步任务允许为空。
/// @param parent_topology 新任务 Topology 的父 Topology；传 nullptr 表示独立停止域。
/// @param func Runtime callable。
/// @return 创建完成的 Work。
template <typename F>
    requires (runtime_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_silent_async_runtime(Executor& executor, Work* parent, Topology* parent_topology, F&& func) {
    using Invoker = SilentAsyncRuntimeInvoker<std::decay_t<F>>;

    return create_work(
        std::in_place_type<Invoker>,
        parent,
        parent_topology,
        std::addressof(executor),
        std::forward<F>(func)
        );
}

/// @brief 创建 fire-and-forget SubFlow 异步任务。
///
/// @tparam F callable 类型。
/// @param executor 当前任务所属 Executor。
/// @param parent 父 Work；根级异步任务允许为空。
/// @param parent_topology 新任务 Topology 的父 Topology；传 nullptr 表示独立停止域。
/// @param func SubFlow callable。
/// @return 创建完成的 Work。
template <typename F>
    requires (subflow_invocable<F> && capturable<F>)
[[nodiscard]] Work* make_silent_async_subflow(Executor& executor, Work* parent, Topology* parent_topology, F&& func) {
    using Invoker = SilentAsyncSubFlowInvoker<std::decay_t<F>>;

    return create_work(
        std::in_place_type<Invoker>,
        parent,
        parent_topology,
        std::addressof(executor),
        std::forward<F>(func)
        );
}

/// @brief 创建 fire-and-forget Module 异步任务。
///
/// @tparam Gh Graph holder 类型。
/// @tparam P predicate 类型。
/// @tparam C callback 类型。
/// @param executor 当前任务所属 Executor。
/// @param parent 父 Work；根级异步任务允许为空。
/// @param parent_topology 新任务 Topology 的父 Topology；传 nullptr 表示独立停止域。
/// @param graph_holder 被执行的子图持有对象。
/// @param pred 迭代终止谓词。
/// @param callback 完成回调。
/// @return 创建完成的 Work。
template <graph_holder Gh, predicate P, callback C>
    requires capturable<P, C>
[[nodiscard]] Work* make_silent_async_module(Executor& executor, Work* parent, Topology* parent_topology, Gh&& graph_holder, P&& pred, C&& callback) {
    using Invoker = SilentAsyncModuleInvoker<detail::captured_t<Gh>, std::decay_t<P>, std::decay_t<C>>;

    return create_work(
        std::in_place_type<Invoker>,
        parent,
        parent_topology,
        std::addressof(executor),
        detail::capture(std::forward<Gh>(graph_holder)),
        std::forward<P>(pred),
        std::forward<C>(callback)
        );
}

// ============================================================================
// Async 异步节点
// ============================================================================

/// @brief 创建带结果槽的普通 Async 异步任务。
///
/// Async 节点建立显式归档锚点，并返回 Work 与 ResultSlot。
///
/// @tparam F callable 类型。
/// @param executor 当前任务所属 Executor。
/// @param parent 父 Work；根级异步任务允许为空。
/// @param parent_topology 新任务 Topology 的父 Topology；传 nullptr 表示独立停止域。
/// @param func 用户 callable。
/// @return 创建完成的 Work 和 ResultSlot。
template <typename F>
    requires (basic_invocable<F> && capturable<F>)
[[nodiscard]] std::pair<Work*, ResultSlot<basic_return_t<F>>*> make_async_basic(Executor& executor, Work* parent, Topology* parent_topology, F&& func) {
    using Invoker = AsyncBasicInvoker<std::decay_t<F>>;

    Work* work = create_work(
        std::in_place_type<Invoker>,
        parent,
        parent_topology,
        std::addressof(executor),
        std::forward<F>(func)
        );

    return {work, work->template target<Invoker>().get_result_slot()};
}

/// @brief 创建带结果槽的 Runtime Async 异步任务。
///
/// @tparam F callable 类型。
/// @param executor 当前任务所属 Executor。
/// @param parent 父 Work；根级异步任务允许为空。
/// @param parent_topology 新任务 Topology 的父 Topology；传 nullptr 表示独立停止域。
/// @param func Runtime callable。
/// @return 创建完成的 Work 和 ResultSlot。
template <typename F>
    requires (runtime_invocable<F> && capturable<F>)
[[nodiscard]] std::pair<Work*, ResultSlot<runtime_return_t<F>>*> make_async_runtime(Executor& executor, Work* parent, Topology* parent_topology, F&& func) {
    using Invoker = AsyncRuntimeInvoker<std::decay_t<F>>;

    Work* work = create_work(
        std::in_place_type<Invoker>,
        parent,
        parent_topology,
        std::addressof(executor),
        std::forward<F>(func)
        );

    return {work, work->template target<Invoker>().get_result_slot()};
}

/// @brief 创建带结果槽的 SubFlow Async 异步任务。
///
/// @tparam F callable 类型。
/// @param executor 当前任务所属 Executor。
/// @param parent 父 Work；根级异步任务允许为空。
/// @param parent_topology 新任务 Topology 的父 Topology；传 nullptr 表示独立停止域。
/// @param func SubFlow callable。
/// @return 创建完成的 Work 和 ResultSlot。
template <typename F>
    requires (subflow_invocable<F> && capturable<F>)
[[nodiscard]] std::pair<Work*, ResultSlot<subflow_return_t<F>>*> make_async_subflow(Executor& executor, Work* parent, Topology* parent_topology, F&& func) {
    using Invoker = AsyncSubFlowInvoker<std::decay_t<F>>;

    Work* work = create_work(
        std::in_place_type<Invoker>,
        parent,
        parent_topology,
        std::addressof(executor),
        std::forward<F>(func)
        );

    return {work, work->template target<Invoker>().get_result_slot()};
}

/// @brief 创建带完成结果槽的 Module Async 异步任务。
///
/// Module 没有值结果，使用 `ResultSlot<void>` 表示任务完成状态。
///
/// @tparam Gh Graph holder 类型。
/// @tparam P predicate 类型。
/// @tparam C callback 类型。
/// @param executor 当前任务所属 Executor。
/// @param parent 父 Work；根级异步任务允许为空。
/// @param parent_topology 新任务 Topology 的父 Topology；传 nullptr 表示独立停止域。
/// @param graph_holder 被执行的子图持有对象。
/// @param pred 迭代终止谓词。
/// @param callback 完成回调。
/// @return 创建完成的 Work 和 ResultSlot<void>。
template <graph_holder Gh, predicate P, callback C>
    requires capturable<P, C>
[[nodiscard]] std::pair<Work*, ResultSlot<void>*> make_async_module(Executor& executor, Work* parent, Topology* parent_topology, Gh&& graph_holder, P&& pred, C&& callback) {
    using Invoker = AsyncModuleInvoker<detail::captured_t<Gh>, std::decay_t<P>, std::decay_t<C>>;

    Work* work = create_work(
        std::in_place_type<Invoker>,
        parent,
        parent_topology,
        std::addressof(executor),
        detail::capture(std::forward<Gh>(graph_holder)),
        std::forward<P>(pred),
        std::forward<C>(callback)
        );

    return {work, work->template target<Invoker>().get_result_slot()};
}

// ============================================================================
// AsyncTask 异步节点
// ============================================================================

/// @brief 创建尚未绑定执行作用域的普通 AsyncTask 异步任务。
///
/// AsyncTask 节点在构造阶段只创建独立 Topology 和结果槽；Executor、父 Work 与父
/// Topology 在 `AsyncTask::_start` 成功取得启动权后再绑定。
///
/// @tparam F callable 类型。
/// @param func 用户 callable。
/// @return 创建完成的 Work 和 ResultSlot。
template <typename F>
    requires (basic_invocable<F> && capturable<F>)
[[nodiscard]] std::pair<Work*, ResultSlot<basic_return_t<F>>*> make_async_task_basic(F&& func) {
    using Invoker = AsyncTaskBasicInvoker<std::decay_t<F>>;

    Work* work = create_work(
        std::in_place_type<Invoker>,
        static_cast<Work*>(nullptr),
        static_cast<Topology*>(nullptr),
        static_cast<Executor*>(nullptr),
        std::forward<F>(func)
        );

    return {work, work->template target<Invoker>().get_result_slot()};
}

/// @brief 创建尚未绑定执行作用域的 Runtime AsyncTask 异步任务。
/// @tparam F callable 类型。
/// @param func Runtime callable。
/// @return 创建完成的 Work 和 ResultSlot。
template <typename F>
    requires (runtime_invocable<F> && capturable<F>)
[[nodiscard]] std::pair<Work*, ResultSlot<runtime_return_t<F>>*> make_async_task_runtime(F&& func) {
    using Invoker = AsyncTaskRuntimeInvoker<std::decay_t<F>>;

    Work* work = create_work(
        std::in_place_type<Invoker>,
        static_cast<Work*>(nullptr),
        static_cast<Topology*>(nullptr),
        static_cast<Executor*>(nullptr),
        std::forward<F>(func)
        );

    return {work, work->template target<Invoker>().get_result_slot()};
}

/// @brief 创建尚未绑定执行作用域的 SubFlow AsyncTask 异步任务。
/// @tparam F callable 类型。
/// @param func SubFlow callable。
/// @return 创建完成的 Work 和 ResultSlot。
template <typename F>
    requires (subflow_invocable<F> && capturable<F>)
[[nodiscard]] std::pair<Work*, ResultSlot<subflow_return_t<F>>*> make_async_task_subflow(F&& func) {
    using Invoker = AsyncTaskSubFlowInvoker<std::decay_t<F>>;

    Work* work = create_work(
        std::in_place_type<Invoker>,
        static_cast<Work*>(nullptr),
        static_cast<Topology*>(nullptr),
        static_cast<Executor*>(nullptr),
        std::forward<F>(func)
        );

    return {work, work->template target<Invoker>().get_result_slot()};
}

/// @brief 创建尚未绑定执行作用域的 Module AsyncTask 异步任务。
///
/// Module 没有值结果，使用 `ResultSlot<void>` 表示完成状态。
///
/// @tparam Gh Graph holder 类型。
/// @tparam P predicate 类型。
/// @tparam C callback 类型。
/// @param graph_holder 被执行的子图持有对象。
/// @param pred 迭代终止谓词。
/// @param callback 完成回调。
/// @return 创建完成的 Work 和 ResultSlot<void>。
template <graph_holder Gh, predicate P, callback C>
    requires capturable<P, C>
[[nodiscard]] std::pair<Work*, ResultSlot<void>*> make_async_task_module(Gh&& graph_holder, P&& pred, C&& callback) {
    using Invoker = AsyncTaskModuleInvoker<detail::captured_t<Gh>, std::decay_t<P>, std::decay_t<C>>;

    Work* work = create_work(
        std::in_place_type<Invoker>,
        static_cast<Work*>(nullptr),
        static_cast<Topology*>(nullptr),
        static_cast<Executor*>(nullptr),
        detail::capture(std::forward<Gh>(graph_holder)),
        std::forward<P>(pred),
        std::forward<C>(callback)
        );

    return {work, work->template target<Invoker>().get_result_slot()};
}

}  // namespace tfl
