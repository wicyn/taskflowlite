/// @file work_factory.hpp
/// @brief Work 节点工厂函数族 —— inline 实现，与 work_factory_fwd.hpp 配对。
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
#include "works.hpp"
#include "work_factory_fwd.hpp"

namespace tfl {

// ============================================================================
//  内联实现 — Graph 内同步节点工厂
// ============================================================================

[[nodiscard]] inline Work* make_noop(const Graph* graph) {
    return new NoopInvoker(graph);
}

/// @brief 创建普通同步任务节点 —— BasicInvoker。
/// @param graph  所属 Graph，用于依赖计数初始化。
/// @param t      可调用对象（含 stop_token 重载亦可）。
/// @param args   转发给可调用对象的参数。
template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
[[nodiscard]] inline Work* make_basic(const Graph* graph, T&& t, Args&&... args) {
    return new BasicInvoker<std::decay_t<T>, std::decay_t<Args>...>(
        graph, std::forward<T>(t), std::forward<Args>(args)...);
}

/// @brief 创建单目标条件分支节点 —— BranchInvoker。
template <typename T, typename... Args>
    requires (capturable<T, Args...> && branch_invocable<T, Args...>)
[[nodiscard]] inline Work* make_branch(const Graph* graph, T&& t, Args&&... args) {
    return new BranchInvoker<std::decay_t<T>, std::decay_t<Args>...>(
        graph, std::forward<T>(t), std::forward<Args>(args)...);
}

/// @brief 创建多目标广播分支节点 —— MultiBranchInvoker。
template <typename T, typename... Args>
    requires (capturable<T, Args...> && multi_branch_invocable<T, Args...>)
[[nodiscard]] inline Work* make_multi_branch(const Graph* graph, T&& t, Args&&... args) {
    return new MultiBranchInvoker<std::decay_t<T>, std::decay_t<Args>...>(
        graph, std::forward<T>(t), std::forward<Args>(args)...);
}

/// @brief 创建单目标强制跳转节点 —— JumpInvoker。
template <typename T, typename... Args>
    requires (capturable<T, Args...> && jump_invocable<T, Args...>)
[[nodiscard]] inline Work* make_jump(const Graph* graph, T&& t, Args&&... args) {
    return new JumpInvoker<std::decay_t<T>, std::decay_t<Args>...>(
        graph, std::forward<T>(t), std::forward<Args>(args)...);
}

/// @brief 创建多目标广播跳转节点 —— MultiJumpInvoker。
template <typename T, typename... Args>
    requires (capturable<T, Args...> && multi_jump_invocable<T, Args...>)
[[nodiscard]] inline Work* make_multi_jump(const Graph* graph, T&& t, Args&&... args) {
    return new MultiJumpInvoker<std::decay_t<T>, std::decay_t<Args>...>(
        graph, std::forward<T>(t), std::forward<Args>(args)...);
}

/// @brief 创建运行时任务节点 —— RuntimeInvoker，注入 Runtime& 上下文。
template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
[[nodiscard]] inline Work* make_runtime(const Graph* graph, T&& t, Args&&... args) {
    return new RuntimeInvoker<std::decay_t<T>, std::decay_t<Args>...>(
        graph, std::forward<T>(t), std::forward<Args>(args)...);
}

/// @brief 创建循环/条件子图节点 —— SubflowInvoker。
/// @param pred  终止谓词 bool()，返回 true 时停止迭代。
template <typename Gh, typename P>
    requires (capturable<P> && graph_holder<Gh> && predicate<P>)
[[nodiscard]] inline Work* make_subflow(const Graph* graph, Gh&& gh, P&& pred) {
    return new SubflowInvoker<detail::captured_t<Gh>, std::decay_t<P>>(
        graph, detail::capture(std::forward<Gh>(gh)), std::forward<P>(pred));
}

// ============================================================================
//  内联实现 — 独立异步任务工厂（fire-and-forget，无 promise 通道）
// ============================================================================

/// @brief 创建 fire-and-forget 普通异步任务 —— DetachedBasicInvoker。
template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable_plain<T, Args...>)
[[nodiscard]] inline Work* make_detached_basic(Executor* exec, Work* parent, T&& t, Args&&... args) {
    return new DetachedBasicInvoker<A, std::decay_t<T>, std::decay_t<Args>...>(
        exec, parent, std::forward<T>(t), std::forward<Args>(args)...);
}

/// @brief 创建 fire-and-forget 运行时异步任务 —— DetachedRuntimeInvoker。
template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable_plain<T, Args...>)
[[nodiscard]] inline Work* make_detached_runtime(Executor* exec, Work* parent, T&& t, Args&&... args) {
    return new DetachedRuntimeInvoker<A, std::decay_t<T>, std::decay_t<Args>...>(
        exec, parent, std::forward<T>(t), std::forward<Args>(args)...);
}

/// @brief 创建 fire-and-forget 异步子图任务 —— DetachedFlowInvoker。
template <anchor_tag A, typename Gh, typename P, typename C>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
[[nodiscard]] inline Work* make_detached_flow(Executor* exec, Work* parent, Gh&& gh, P&& pred, C&& cb) {
    return new DetachedFlowInvoker<A, detail::captured_t<Gh>, std::decay_t<P>, std::decay_t<C>>(
        exec, parent, detail::capture(std::forward<Gh>(gh)), std::forward<P>(pred), std::forward<C>(cb));
}

/// @brief 创建带 std::promise<R> 的普通异步任务 —— PromisedBasicInvoker。
template <anchor_tag A, typename T, typename R, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
[[nodiscard]] inline Work* make_promised_basic(Executor* exec, Work* parent, T&& t, std::promise<R>&& p, Args&&... args) {
    return new PromisedBasicInvoker<A, std::decay_t<T>, R, std::decay_t<Args>...>(
        exec, parent, std::forward<T>(t), std::move(p), std::forward<Args>(args)...);
}

/// @brief 创建带 std::promise<R> 的运行时异步任务 —— PromisedRuntimeInvoker。
template <anchor_tag A, typename T, typename R, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
[[nodiscard]] inline Work* make_promised_runtime(Executor* exec, Work* parent, T&& t, std::promise<R>&& p, Args&&... args) {
    return new PromisedRuntimeInvoker<A, std::decay_t<T>, R, std::decay_t<Args>...>(
        exec, parent, std::forward<T>(t), std::move(p), std::forward<Args>(args)...);
}

/// @brief 创建带 std::promise<void> 的异步子图任务 —— PromisedFlowInvoker。
template <anchor_tag A, typename Gh, typename P, typename C>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
[[nodiscard]] inline Work* make_promised_flow(Executor* exec, Work* parent, Gh&& gh, P&& pred, C&& cb, std::promise<void>&& p) {
    return new PromisedFlowInvoker<A, detail::captured_t<Gh>, std::decay_t<P>, std::decay_t<C>>(
        exec, parent, detail::capture(std::forward<Gh>(gh)), std::forward<P>(pred), std::forward<C>(cb), std::move(p));
}
// ============================================================================
//  内联实现 — 有依赖的异步任务工厂（挂载到外部 Topology）
// ============================================================================

/// @brief 创建依赖型延迟普通异步任务 —— AttachedBasicInvoker。
template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
[[nodiscard]] inline Work* make_attached_basic(Executor* exec, Work* parent, T&& t, Args&&... args) {
    return new AttachedBasicInvoker<A, std::decay_t<T>, std::decay_t<Args>...>(
        exec, parent, std::forward<T>(t), std::forward<Args>(args)...);
}

/// @brief 创建依赖型延迟运行时异步任务 —— AttachedRuntimeInvoker。
template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
[[nodiscard]] inline Work* make_attached_runtime(Executor* exec, Work* parent, T&& t, Args&&... args) {
    return new AttachedRuntimeInvoker<A, std::decay_t<T>, std::decay_t<Args>...>(
        exec, parent, std::forward<T>(t), std::forward<Args>(args)...);
}

/// @brief 创建依赖型延迟异步子图任务 —— AttachedFlowInvoker。
template <anchor_tag A, typename Gh, typename P, typename C>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
[[nodiscard]] inline Work* make_attached_flow(Executor* exec, Work* parent, Gh&& gh, P&& pred, C&& cb) {
    return new AttachedFlowInvoker<A, detail::captured_t<Gh>, std::decay_t<P>, std::decay_t<C>>(
        exec, parent, detail::capture(std::forward<Gh>(gh)),
        std::forward<P>(pred),
        std::forward<C>(cb));
}

/// @brief 销毁工作节点，直接 delete。
inline void destroy(Work* work) noexcept {
    delete work;
}

}  // namespace tfl
