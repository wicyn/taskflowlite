/// @file traits.hpp
/// @brief 类型概念与萃取 —— 框架的 C++20 concepts 集中定义。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <concepts>
#include <functional>
#include <type_traits>
#include <version>
#include <coroutine>
#include <string>
#include <memory>
#include <cstddef>
#include <tuple>
#include <utility>

#include "forward.hpp"

namespace tfl {

// ── callable 与捕获存储规则 ──────────────────────────────────────────
// 任务 callable 直接保存自身，执行参数由 callable 自行捕获。
// captured_t 用于需要持有或借用外部对象的内部存储:
// 1. 普通左值 → reference_wrapper<T>
// 2. 普通右值 → decay_t<T>
//
// 速查表:
// | 输入                              | 结果                         |
// |-----------------------------------|------------------------------|
// | std::ref(x)                       | reference_wrapper<T>         |
// | std::cref(x)                      | reference_wrapper<const T>   |
// | T& / const T&                     | reference_wrapper<T>         |
// | T&&                               | T                            |
// | T                                 | T                            |

namespace detail {

/// @brief 将未匹配成完整信号量/计数对的参数序列判定为 false。
/// @tparam Ts 待验证的完整参数序列。
template <typename... Ts>
struct is_sem_count_seq : std::false_type {};

/// @brief 将空序列作为递归终点判定为合法的信号量/计数序列。
template <>
struct is_sem_count_seq<> : std::true_type {};

/// @brief 逐对验证 `Semaphore` 左值引用和可转换为 `size_t` 的计数。
/// @tparam S 当前信号量参数类型。
/// @tparam C 当前计数参数类型。
/// @tparam Rest 尚待递归验证的剩余参数。
template <typename S, typename C, typename... Rest>
struct is_sem_count_seq<S, C, Rest...>
    : std::bool_constant<
          std::is_lvalue_reference_v<S>
          && std::same_as<std::remove_reference_t<S>, Semaphore>
          && std::convertible_to<C, std::size_t>
          && is_sem_count_seq<Rest...>::value
          > {};

// ============================================================================
// reference_wrapper 探测
// ============================================================================

/// @brief 将普通类型判定为非 `std::reference_wrapper` 的内部类型萃取。
/// @tparam T 待检测类型。
template <typename T>
struct is_reference_wrapper : std::false_type {};

/// @brief 识别 `std::reference_wrapper<T>` 并保留其被引用类型参数。
/// @tparam T 包装器引用的对象类型。
template <typename T>
struct is_reference_wrapper<std::reference_wrapper<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_reference_wrapper_v = is_reference_wrapper<std::remove_cvref_t<T>>::value;

// ============================================================================
// 捕获存储
// ============================================================================

/// @brief 根据实参值类别选择非拥有引用包装或衰减值存储。
/// @tparam T 原始实参类型。
template <typename T>
using captured_t = std::conditional_t<is_reference_wrapper_v<T>,
                                      std::decay_t<T>,
                                      std::conditional_t<std::is_lvalue_reference_v<T>,
                                                         std::reference_wrapper<std::remove_reference_t<T>>,
                                                         std::decay_t<T>>>;

static_assert(std::same_as<captured_t<int>, int>);
static_assert(std::same_as<captured_t<int&&>, int>);
static_assert(std::same_as<captured_t<int&>, std::reference_wrapper<int>>);
static_assert(std::same_as<captured_t<const int&>, std::reference_wrapper<const int>>);
static_assert(std::same_as<captured_t<std::reference_wrapper<int>&>, std::reference_wrapper<int>>);
static_assert(std::same_as<captured_t<std::unique_ptr<int>>, std::unique_ptr<int>>);
static_assert(std::same_as<captured_t<std::unique_ptr<int>&>, std::reference_wrapper<std::unique_ptr<int>>>);
static_assert(std::same_as<captured_t<Flow&>, std::reference_wrapper<Flow>>);

/// @brief 将实参转换为可存储表示。
/// @warning 普通左值按非拥有引用保存，调用方必须保证其生命周期。
template <typename T>
    requires std::constructible_from<captured_t<T>, T>
[[nodiscard]] constexpr auto capture(T&& value) noexcept(std::is_nothrow_constructible_v<captured_t<T>, T>) -> captured_t<T> {
    if constexpr (is_reference_wrapper_v<T>) {
        return std::forward<T>(value);
    } else if constexpr (std::is_lvalue_reference_v<T>) {
        return std::ref(value);
    } else {
        return std::forward<T>(value);
    }
}

/// @brief 从捕获存储中借出可调用的左值引用。
template <typename T>
[[nodiscard]] constexpr decltype(auto) borrow(T& value) noexcept {
    if constexpr (is_reference_wrapper_v<T>) {
        return value.get();
    } else {
        return (value);
    }
}

}  // namespace detail
/// @brief 检测单个实参能否按值持久化存储。
///
/// 框架使用 `std::decay_t<T>` 作为存储类型：
///   1. 普通左值需要能够复制到存储对象；
///   2. 普通右值需要能够移动或复制到存储对象；
///   3. std::ref/std::cref 产生的 reference_wrapper 按值保存包装器。
///
/// @note 该约束直接检查下面的构造是否合法：
///       `std::decay_t<T>(std::forward<T>(value))`
template <typename T>
concept capturable_one = std::constructible_from<std::decay_t<T>, T>;

/// @brief 检测参数包中的所有实参能否按值持久化存储。
template <typename... Ts>
concept capturable = (capturable_one<Ts> && ...);

static_assert(capturable<int>);
static_assert(capturable<int&>);
static_assert(capturable<const int&>);
static_assert(capturable<std::string&&>);
static_assert(capturable<std::unique_ptr<int>>);
static_assert(capturable<std::unique_ptr<int>&&>);
static_assert(!capturable<std::unique_ptr<int>&>);
static_assert(capturable<std::reference_wrapper<std::unique_ptr<int>>>);
static_assert(capturable<std::reference_wrapper<int>>);

template <typename P, typename... Args>
concept predicate = std::invocable<std::decay_t<P>&, std::decay_t<Args>&...> &&
                    std::same_as<std::invoke_result_t<std::decay_t<P>&, std::decay_t<Args>&...>, bool>;

/// @brief 约束谓词调用不抛出异常。
template <typename P, typename... Args>
concept noexcept_predicate = predicate<P, Args...> &&
                             std::is_nothrow_invocable_v<std::decay_t<P>&, std::decay_t<Args>&...>;

/// @brief 检查是否为有效的回调类型（返回 void）。
template <typename C, typename... Args>
concept callback = std::invocable<std::decay_t<C>&, std::decay_t<Args>&...> &&
                   std::same_as<std::invoke_result_t<std::decay_t<C>&, std::decay_t<Args>&...>, void>;

/// @brief 表示可默认构造、无状态且调用后不执行任何操作的完成回调。
///
/// 用作可选回调的缺省类型，不保存资源且调用保证不抛异常。
struct noop_callback {
    constexpr void operator()() const noexcept {}
};

static_assert(predicate<decltype([]{ return true; })>);
static_assert(!predicate<decltype([]{})>);

static_assert(callback<decltype([]{})>);
static_assert(!callback<decltype([]{ return true; })>);

/// @brief 约束类型本身为 Graph，或提供 noexcept 的可变与只读 graph() 访问器。
template <typename Gh>
concept graph_holder = std::derived_from<std::remove_cvref_t<Gh>, Graph> || (
                           requires(std::remove_cvref_t<Gh>& gh) {
                               { gh.graph() } noexcept -> std::convertible_to<Graph&>;
                           } &&
                           requires(const std::remove_cvref_t<Gh>& gh) {
                               { gh.graph() } noexcept -> std::convertible_to<const Graph&>;
                           }
                           );

namespace detail {

/// @brief 从 graph_holder 获取可修改 Graph 引用。
template <graph_holder Gh>
[[nodiscard]] inline auto to_graph(Gh& gh) noexcept -> Graph& {
    using U = std::remove_cvref_t<Gh>;
    if constexpr (std::derived_from<U, Graph>) {
        return static_cast<Graph&>(gh);
    } else {
        return static_cast<Graph&>(gh.graph());
    }
}

/// @brief 从 graph_holder 获取只读 Graph 引用。
template <graph_holder Gh>
[[nodiscard]] inline auto to_graph(const Gh& gh) noexcept -> const Graph& {
    using U = std::remove_cvref_t<Gh>;
    if constexpr (std::derived_from<U, Graph>) {
        return static_cast<const Graph&>(gh);
    } else {
        return static_cast<const Graph&>(gh.graph());
    }
}

/// @brief 检测返回类型是否满足协程返回对象协议。
template <typename R>
concept coroutine_returnable = requires {
    typename std::coroutine_traits<std::remove_cvref_t<R>>::promise_type;
};

}

// ============================================================================
// 返回类型推导
// ============================================================================

/// @brief 普通 callable 的返回类型。
template <typename T>
using basic_return_t = std::invoke_result_t<std::decay_t<T>&>;

/// @brief 单目标分支 callable 的返回类型。
template <typename T>
using branch_return_t = std::invoke_result_t<std::decay_t<T>&, Branch&>;

/// @brief 多目标分支 callable 的返回类型。
template <typename T>
using multi_branch_return_t = std::invoke_result_t<std::decay_t<T>&, MultiBranch&>;

/// @brief 单目标跳转 callable 的返回类型。
template <typename T>
using jump_return_t = std::invoke_result_t<std::decay_t<T>&, Jump&>;

/// @brief 多目标跳转 callable 的返回类型。
template <typename T>
using multi_jump_return_t = std::invoke_result_t<std::decay_t<T>&, MultiJump&>;

/// @brief Runtime callable 的返回类型。
template <typename T>
using runtime_return_t = std::invoke_result_t<std::decay_t<T>&, Runtime&>;

/// @brief SubFlow callable 的返回类型。
template <typename T>
using subflow_return_t = std::invoke_result_t<std::decay_t<T>&, SubFlow&>;

// ============================================================================
//  任务 callable 概念
//
//  每类 callable 只接收自身所需的执行上下文。
//  返回协程对象的 callable 不进入普通任务路径。
// ============================================================================

/// @brief `f()` 可调用 —— 普通同步任务签名。
template <typename T>
concept basic_invocable = std::invocable<std::decay_t<T>&>
                          && !detail::coroutine_returnable<basic_return_t<T>>;

/// @brief `f(Branch&)` 可调用 —— 单目标条件分支任务签名。
template <typename T>
concept branch_invocable = std::invocable<std::decay_t<T>&, Branch&>
                           && !detail::coroutine_returnable<branch_return_t<T>>;

/// @brief `f(MultiBranch&)` 可调用 —— 多目标广播分支任务签名。
template <typename T>
concept multi_branch_invocable = std::invocable<std::decay_t<T>&, MultiBranch&>
                                 && !detail::coroutine_returnable<multi_branch_return_t<T>>;

/// @brief `f(Jump&)` 可调用 —— 单目标强制跳转任务签名。
template <typename T>
concept jump_invocable = std::invocable<std::decay_t<T>&, Jump&>
                         && !detail::coroutine_returnable<jump_return_t<T>>;

/// @brief `f(MultiJump&)` 可调用 —— 多目标广播跳转任务签名。
template <typename T>
concept multi_jump_invocable = std::invocable<std::decay_t<T>&, MultiJump&>
                               && !detail::coroutine_returnable<multi_jump_return_t<T>>;

/// @brief `f(Runtime&)` 可调用 —— 运行时动态调度任务签名。
template <typename T>
concept runtime_invocable = std::invocable<std::decay_t<T>&, Runtime&>
                            && !detail::coroutine_returnable<runtime_return_t<T>>;

/// @brief `f(SubFlow&)` 可调用。
template <typename T>
concept subflow_invocable = std::invocable<std::decay_t<T>&, SubFlow&>
                            && !detail::coroutine_returnable<subflow_return_t<T>>;

/// @brief 约束 `Ts...` 为 `{Semaphore, count, Semaphore, count, ...}` 交替序列。
template <typename... Ts>
concept sem_count_sequence = detail::is_sem_count_seq<Ts...>::value;
/// @brief 将一次任务创建所需的 callable 和参数按衰减类型组合保存为元组。
///
/// `pack` 按值拥有内部元素，用于批量 `emplace` 时延后展开；左值也会被复制而非借用。
///
/// @tparam Ts 构造时推导出的原始参数类型。
template <typename... Ts>
struct pack {
    /// @brief 退化后的内部存储类型。
    using tuple_type = std::tuple<std::decay_t<Ts>...>;

    tuple_type data;

    /// @brief 按衰减规则保存全部参数。
    /// @param args 要保存的 callable 及其参数。
    constexpr pack(Ts&&... args)
        : data(std::forward<Ts>(args)...) {}
};

/// @brief 为 `pack{args...}` 推导原始参数类型。
/// @note 内部 tuple 负责对推导结果执行衰减。
template <typename... Ts>
pack(Ts&&...) -> pack<Ts...>;

// ============================================================================
// pack 探测
// ============================================================================
namespace detail {

/// @brief 将任意普通类型判定为非 `tfl::pack` 的内部类型萃取。
/// @tparam T 待检测类型。
template <typename T>
struct is_pack_impl : std::false_type {};

/// @brief 识别任意参数列表的 `tfl::pack` 特化。
/// @tparam Args `pack` 保存的参数类型。
template <typename... Args>
struct is_pack_impl<pack<Args...>> : std::true_type {};

}  // namespace detail

/// @brief 约束类型为 `tfl::pack`。
template <typename T>
concept task_pack = detail::is_pack_impl<std::remove_cvref_t<T>>::value;

// ============================================================================
// AsyncFuture —— 类型识别
// ============================================================================

template <typename T>
concept async_future = requires {typename std::remove_cvref_t<T>::result_type;} &&
                       std::derived_from<std::remove_cvref_t<T>, AsyncFuture<typename std::remove_cvref_t<T>::result_type>>;

// ============================================================================
// AsyncTask —— 类型识别
// ============================================================================

template <typename T>
struct is_async_task : std::false_type {};

template <typename R>
struct is_async_task<AsyncTask<R>> : std::true_type {};

template <typename T>
inline constexpr bool is_async_task_v = is_async_task<std::remove_cvref_t<T>>::value;

template <typename T>
concept async_task = is_async_task_v<T>;


// ============================================================================
// 转发返回类型
// ============================================================================

template <typename T>
using forward_return_t = std::conditional_t<std::is_lvalue_reference_v<T>, T, std::remove_cvref_t<T>>;

} // namespace tfl
