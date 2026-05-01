/// @file work_memory.hpp
/// @brief Work 节点工厂函数族 —— inline 实现，与 work_memory_fwd.hpp 配对
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-04-20
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn
///
/// @details
/// 本文件提供 `make_*` 系列工厂的内联实现。所有函数都是 **薄包装**：
/// 校验 concept 约束、`std::decay` 模板参数、`new` 出对应的 Invoker
/// 子类。这层薄包装的存在意义有三：
///
///   - **类型擦除**：调用点拿到的是 `Work*`，与具体 Invoker 子类解耦，
///     未来加新节点类型不会污染上层模板签名；
///   - **统一入口**：所有 `Work` 实例化集中在此处，便于将来替换分配器
///     （例如改为对象池或 arena）—— 当前的 `new` / `delete` 只是占位实现；
///   - **decay 标准化**：模板参数在用户传入时通常带引用 / cv 限定，本层
///     统一用 `std::decay_t` 拍平，避免 Invoker 内部需要重复处理这种类型杂项。
///
/// 函数族结构与 work_memory_fwd.hpp 保持完全一致 —— 修改时务必同步两份。
/// 节点销毁集中在文件末尾的 `destroy(Work*)`，目前是直接 `delete`，
/// 将来切换分配器时这是唯一需要改动的释放点。
///
/// @see work_memory_fwd.hpp  对应的前置声明（仅签名，不含实现）
/// @see works.hpp            被本文件 new 出的各 Invoker 子类定义


#pragma once
#include <cmath>
#include <cstring>
#include "work.hpp"
#include "works.hpp"
#include "work_memory_fwd.hpp"

namespace tfl {

// ============================================================================
//  内联实现 — Graph 内同步节点工厂
// ============================================================================

template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
[[nodiscard]] inline Work* make_basic(const Graph* graph, T&& f, Args&&... args) {
    return new BasicInvoker<std::decay_t<T>, std::decay_t<Args>...>(
        graph, std::forward<T>(f), std::forward<Args>(args)...);
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && branch_invocable<T, Args...>)
[[nodiscard]] inline Work* make_branch(const Graph* graph, T&& f, Args&&... args) {
    return new BranchInvoker<std::decay_t<T>, std::decay_t<Args>...>(
        graph, std::forward<T>(f), std::forward<Args>(args)...);
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && multi_branch_invocable<T, Args...>)
[[nodiscard]] inline Work* make_multi_branch(const Graph* graph, T&& f, Args&&... args) {
    return new MultiBranchInvoker<std::decay_t<T>, std::decay_t<Args>...>(
        graph, std::forward<T>(f), std::forward<Args>(args)...);
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && jump_invocable<T, Args...>)
[[nodiscard]] inline Work* make_jump(const Graph* graph, T&& f, Args&&... args) {
    return new JumpInvoker<std::decay_t<T>, std::decay_t<Args>...>(
        graph, std::forward<T>(f), std::forward<Args>(args)...);
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && multi_jump_invocable<T, Args...>)
[[nodiscard]] inline Work* make_multi_jump(const Graph* graph, T&& f, Args&&... args) {
    return new MultiJumpInvoker<std::decay_t<T>, std::decay_t<Args>...>(
        graph, std::forward<T>(f), std::forward<Args>(args)...);
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
[[nodiscard]] inline Work* make_runtime(const Graph* graph, T&& f, Args&&... args) {
    return new RuntimeInvoker<std::decay_t<T>, std::decay_t<Args>...>(
        graph, std::forward<T>(f), std::forward<Args>(args)...);
}

template <typename F, typename P>
    requires (capturable<P> && flow_type<F> && predicate<P>)
[[nodiscard]] inline Work* make_subflow(const Graph* graph, F&& flow, P&& pred) {
    return new SubflowInvoker<detail::wrap_t<F>, std::decay_t<P>>(
        graph, detail::wrap(std::forward<F>(flow)), std::forward<P>(pred));
}

// ============================================================================
//  内联实现 — 独立异步任务工厂
// ============================================================================

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
[[nodiscard]] inline Work* make_silent_async_basic(Executor& exec, Work* parent, T&& f, Args&&... args) {
    return new SilentAsyncBasicInvoker<A, std::decay_t<T>, std::decay_t<Args>...>(
        exec, parent, std::forward<T>(f), std::forward<Args>(args)...);
}

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
[[nodiscard]] inline Work* make_silent_async_runtime(Executor& exec, Work* parent, T&& f, Args&&... args) {
    return new SilentAsyncRuntimeInvoker<A, std::decay_t<T>, std::decay_t<Args>...>(
        exec, parent, std::forward<T>(f), std::forward<Args>(args)...);
}

template <anchor_tag A, typename F, typename P, typename C>
    requires (capturable<P, C> && flow_type<F> && predicate<P> && callback<C>)
[[nodiscard]] inline Work* make_silent_async_flow(Executor& exec, Work* parent, F&& flow, P&& pred, C&& cb) {
    return new SilentAsyncFlowInvoker<A, detail::wrap_t<F>, std::decay_t<P>, std::decay_t<C>>(
        exec, parent, detail::wrap(std::forward<F>(flow)),
        std::forward<P>(pred),
        std::forward<C>(cb));
}

template <anchor_tag A, typename T, typename R, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
[[nodiscard]] inline Work* make_async_basic(Executor& exec, Work* parent, T&& f, std::promise<R>&& p, Args&&... args) {
    return new AsyncBasicInvoker<A, std::decay_t<T>, R, std::decay_t<Args>...>(
        exec, parent, std::forward<T>(f), std::move(p), std::forward<Args>(args)...);
}

template <anchor_tag A, typename T, typename R, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
[[nodiscard]] inline Work* make_async_runtime(Executor& exec, Work* parent, T&& f, std::promise<R>&& p, Args&&... args) {
    return new AsyncRuntimeInvoker<A, std::decay_t<T>, R, std::decay_t<Args>...>(
        exec, parent, std::forward<T>(f), std::move(p), std::forward<Args>(args)...);
}

// ============================================================================
//  内联实现 — 有依赖的异步任务工厂
// ============================================================================

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
[[nodiscard]] inline Work* make_dep_async_basic(Executor& exec, Work* parent, T&& f, Args&&... args) {
    return new DepAsyncBasicInvoker<A, std::decay_t<T>, std::decay_t<Args>...>(
        exec, parent, std::forward<T>(f), std::forward<Args>(args)...);
}

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
[[nodiscard]] inline Work* make_dep_async_runtime(Executor& exec, Work* parent, T&& f, Args&&... args) {
    return new DepAsyncRuntimeInvoker<A, std::decay_t<T>, std::decay_t<Args>...>(
        exec, parent, std::forward<T>(f), std::forward<Args>(args)...);
}

template <anchor_tag A, typename F, typename P, typename C>
    requires (capturable<P, C> && flow_type<F> && predicate<P> && callback<C>)
[[nodiscard]] inline Work* make_dep_async_flow(Executor& exec, Work* parent, F&& flow, P&& pred, C&& cb) {
    return new DepAsyncFlowInvoker<A, detail::wrap_t<F>, std::decay_t<P>, std::decay_t<C>>(
        exec, parent, detail::wrap(std::forward<F>(flow)),
        std::forward<P>(pred),
        std::forward<C>(cb));
}

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
[[nodiscard]] inline Work* make_dep_deferred_async_basic(Executor& exec, Work* parent, T&& f, Args&&... args) {
    return new DepDeferredAsyncBasicInvoker<A, std::decay_t<T>, std::decay_t<Args>...>(
        exec, parent, std::forward<T>(f), std::forward<Args>(args)...);
}

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
[[nodiscard]] inline Work* make_dep_deferred_async_runtime(Executor& exec, Work* parent, T&& f, Args&&... args) {
    return new DepDeferredAsyncRuntimeInvoker<A, std::decay_t<T>, std::decay_t<Args>...>(
        exec, parent, std::forward<T>(f), std::forward<Args>(args)...);
}

template <anchor_tag A, typename F, typename P, typename C>
    requires (capturable<P, C> && flow_type<F> && predicate<P> && callback<C>)
[[nodiscard]] inline Work* make_dep_deferred_async_flow(Executor& exec, Work* parent, F&& flow, P&& pred, C&& cb) {
    return new DepDeferredAsyncFlowInvoker<A, detail::wrap_t<F>, std::decay_t<P>, std::decay_t<C>>(
        exec, parent, detail::wrap(std::forward<F>(flow)),
        std::forward<P>(pred),
        std::forward<C>(cb));
}

/// @brief 销毁工作节点及其关联的拓扑资源
inline void destroy(Work* work) noexcept {
    delete work;
}

}  // namespace tfl
