/// @file work_factory_fwd.hpp
/// @brief Work 节点工厂函数族 —— 仅声明，与 work_factory.hpp 配对
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-04-20
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn
///
/// @details
/// 本文件是整个框架 `Work*` 节点的 **唯一分配入口** 的前置声明部分。所有
/// `Work` 子类（BasicInvoker / BranchInvoker / SilentAsync* / AsyncHandle*
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
///
/// 拆分 fwd 与 impl 的目的：上层头文件只需 include fwd 即可获得签名，
/// 避免被 `works.hpp` 的全部 Invoker 模板拖累编译时间。完整实现见
/// `work_factory.hpp`。
///
/// @see work_factory.hpp     全部工厂函数的 inline 实现
/// @see work.hpp            Work 基类与节点状态字段
/// @see works.hpp           各 Invoker 子类（实际构造目标）
///
#pragma once
#include <cmath>
#include <cstring>
#include "work.hpp"
namespace tfl {

namespace detail {

/// @brief 将锚点标签 `A` 映射为对应的 Implicit/Explicit 位掩码对。
///
/// 本函数为 `consteval` —— 所有锚点位均在编译期解析为常量，
/// 运行时零开销。映射规则：
///   - `none_t`     → `{NONE, NONE}`
///   - `implicit_t` → `{ANCHORED, NONE}`
///   - `explicit_t` → `{NONE, ANCHORED}`
///
/// @tparam A  锚点标签类型，必须为 `anchor::none_t`、`anchor::implicit_t`
///            或 `anchor::explicit_t` 之一。
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

// detail 命名空间内
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

/// @brief 销毁工作节点，直接 delete
inline void destroy(Work* work) noexcept;
}  // namespace tfl
