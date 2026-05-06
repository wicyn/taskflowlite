/// @file work_memory_fwd.hpp
/// @brief Work 节点工厂函数族 —— 仅声明，与 work_memory.hpp 配对
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-04-20
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn
///
/// @details
/// 本文件是整个框架 `Work*` 节点的 **唯一分配入口** 的前置声明部分。所有
/// `Work` 子类（BasicInvoker / BranchInvoker / SilentAsync* / DepAsync*
/// 等）都通过这里声明的 `make_*` 工厂函数创建 —— 用户层（Flow / Executor /
/// Runtime）从不直接 `new`，便于将来替换为对象池而无需改动调用点。
///
/// 工厂函数按 **生命周期所有者** 分成三族：
///
///   1. **Graph 内同步节点** —— `make_basic` / `make_branch` /
///      `make_multi_branch` / `make_jump` / `make_multi_jump` /
///      `make_runtime` / `make_subflow`。所有权归属传入的 `Graph*`，
///      由 Flow 析构连带释放。这一族 **不参与 topology 引用计数**。
///
///   2. **独立异步任务** —— `make_silent_async_*` / `make_async_*`。
///      生命周期由 Executor 的 topology 计数管理，无父节点占位（fire-and-
///      forget 或独立 future）。
///
///   3. **依赖型异步任务** —— `make_dep_async_*` / `make_dep_deferred_async_*`。
///      与族 2 的区别在于建立了显式前驱依赖，需要 join_counter 协议参与，
///      `parent` 指针不可为空。
///
/// 拆分 fwd 与 impl 的目的：上层头文件只需 include fwd 即可获得签名，
/// 避免被 `works.hpp` 的全部 Invoker 模板拖累编译时间。完整实现见
/// `work_memory.hpp`。
///
/// @see work_memory.hpp     全部工厂函数的 inline 实现
/// @see work.hpp            Work 基类与节点状态字段
/// @see works.hpp           各 Invoker 子类（实际构造目标）
///
#pragma once
#include <cmath>
#include <cstring>
#include "work.hpp"
namespace tfl {

// ============================================================================
//  内联实现 — Graph 内同步节点工厂
// ============================================================================

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
//  内联实现 — 独立异步任务工厂
// ============================================================================

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable_plain<T, Args...>)
[[nodiscard]] inline Work* make_silent_async_basic(Work* parent, T&& t, Args&&... args);

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable_plain<T, Args...>)
[[nodiscard]] inline Work* make_silent_async_runtime(Work* parent, T&& t, Args&&... args);

template <anchor_tag A, typename Gh, typename P, typename C>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
[[nodiscard]] inline Work* make_silent_async_flow(Work* parent, Gh&& gh, P&& pred, C&& cb);

template <anchor_tag A, typename T, typename R, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
[[nodiscard]] inline Work* make_async_basic(Executor& exec, Work* parent, T&& t, std::promise<R>&& p, Args&&... args);

template <anchor_tag A, typename T, typename R, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
[[nodiscard]] inline Work* make_async_runtime(Executor& exec, Work* parent, T&& t, std::promise<R>&& p, Args&&... args);

template <anchor_tag A, typename Gh, typename P, typename C>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
[[nodiscard]] inline Work* make_async_flow(Executor& exec, Work* parent, Gh&& gh, P&& pred, C&& cb, std::promise<void>&& p);
// ============================================================================
//  内联实现 — 有依赖的异步任务工厂
// ============================================================================

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
[[nodiscard]] inline Work* make_dep_async_basic(Executor& exec, Work* parent, T&& t, Args&&... args);

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
[[nodiscard]] inline Work* make_dep_async_runtime(Executor& exec, Work* parent, T&& t, Args&&... args);

template <anchor_tag A, typename Gh, typename P, typename C>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
[[nodiscard]] inline Work* make_dep_async_flow(Executor& exec, Work* parent, Gh&& gh, P&& pred, C&& cb);

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
[[nodiscard]] inline Work* make_dep_deferred_async_basic(Executor& exec, Work* parent, T&& t, Args&&... args);

template <anchor_tag A, typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
[[nodiscard]] inline Work* make_dep_deferred_async_runtime(Executor& exec, Work* parent, T&& t, Args&&... args);

template <anchor_tag A, typename Gh, typename P, typename C>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
[[nodiscard]] inline Work* make_dep_deferred_async_flow(Executor& exec, Work* parent, Gh&& gh, P&& pred, C&& cb);

/// @brief 销毁工作节点及其关联的拓扑资源
inline void destroy(Work* work) noexcept;
}  // namespace tfl
