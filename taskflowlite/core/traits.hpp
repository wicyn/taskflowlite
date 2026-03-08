/// @file traits.hpp
/// @brief 提供框架核心类型约束概念（Concepts）与类型萃取工具，用于编译期类型检查与推导。
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include <concepts>
#include <functional>
#include <type_traits>
#include <span>
#include <string>

#include "forward.hpp"

namespace tfl {
/*
 * ======================================================================================
 *
 * std::unwrap_ref_decay_t<T> 行为速查表
 *
 * 这是理解本文件所有 concept 约束的基础。框架中存储的 callable 类型都是
 * unwrap_ref_decay_t<T>，即经过 decay + unwrap 后的最终存储类型。
 *
 * 逻辑流程 / Logic flow:
 * 1. 先 Decay (退化): 移除引用、cv限定符，数组/函数转指针 (类似于值传递)。
 * 2. 后 Unwrap (解包): 如果结果是 std::reference_wrapper，则还原为原始引用 (T&)。
 *
 * ======================================================================================
 */

// -----------------------------------------------------------
// 场景 1: 针对 std::ref / std::cref (还原为引用)
// -----------------------------------------------------------
// int x;
// 1. std::ref(x)  -> std::reference_wrapper<int>
// 2. Decay        -> std::reference_wrapper<int> (保持不变 / unchanged)
// 3. Unwrap       -> int& (提取引用 / extract reference)
// using A = std::unwrap_ref_decay_t<decltype(std::ref(x))>;  // A == int&

// 1. std::cref(x) -> std::reference_wrapper<const int>
// 2. Decay        -> std::reference_wrapper<const int>
// 3. Unwrap       -> const int&
// using B = std::unwrap_ref_decay_t<decltype(std::cref(x))>; // B == const int&

// -----------------------------------------------------------
// 场景 2: 针对 普通引用 / 指针 / 值 (退化为裸类型)
// -----------------------------------------------------------
// using C = std::unwrap_ref_decay_t<int&>;        // C == int (引用被剥离 / ref stripped)
// using D = std::unwrap_ref_decay_t<const int&>;  // D == int (const & 都被剥离 / both stripped)
// using E = std::unwrap_ref_decay_t<int>;         // E == int
// using F = std::unwrap_ref_decay_t<int*>;        // F == int* (指针不变 / pointer unchanged)

// -----------------------------------------------------------
// 场景 3: 针对 数组 / 函数 (退化为指针)
// -----------------------------------------------------------
// using G = std::unwrap_ref_decay_t<int[3]>;      // G == int*
// using H = std::unwrap_ref_decay_t<void()>;      // H == void(*)()

// -----------------------------------------------------------
// 典型应用场景示例: 自定义 Invoke
// -----------------------------------------------------------
// 目的: 允许参数通过 std::ref 传递以避免拷贝，但在内部处理时还原为引用使用
/*
template<class F, class... Args>
void invoke_like(F&& f, Args&&... args) {
    std::invoke(
        std::forward<F>(f),
        // 如果 args 是 std::ref(obj)，这里会被还原为 obj& 传递给函数
        // 如果 args 是 int&，这里会被退化为 int (值拷贝) 传递 (取决于 forward 的行为)
        std::forward<std::unwrap_ref_decay_t<Args>>(args)...
    );
}

# 如果传入的是 std::ref，detail::unwrap_t<Args>& 最终得到的是 左值引用 (T&)。
这里发生了 引用折叠 (Reference Collapsing)。

详细推导过程 / Detailed derivation:
让我们一步步拆解 detail::unwrap_t<Args>& 在传入 std::ref 时的类型变化。

假设我们有一个类型 int，我们传入 std::ref(x)。

1. 类型推导 / Type deduction:
   Args 被推导为 std::reference_wrapper<int>。

2. unwrap_t (即 std::unwrap_ref_decay_t) 的作用:
   std::unwrap_ref_decay_t 的定义是：如果类型是 std::reference_wrapper<T>，则结果为 T&。

   输入 / Input:  std::reference_wrapper<int>
   输出 / Output: int& (注意：这里已经是引用了 / note: already a reference)

3. Concept 中的 & 修饰符 / The & qualifier in concepts:
   代码中写的是 detail::unwrap_t<Args>&（末尾有一个 &）。

   代入后 / After substitution: int& + & -> int& &

4. 引用折叠 (Reference Collapsing):
   C++ 有明确的引用折叠规则:

   T& &   -> T&
   T& &&  -> T&
   T&& &  -> T&
   T&& && -> T&&

   结果 / Result: int& & 折叠为 int&。

*/

namespace detail {

/// @brief 检查类型是否为 std::reference_wrapper 包装
template <typename T>
struct is_reference_wrapper : std::false_type {};

template <typename T>
struct is_reference_wrapper<std::reference_wrapper<T>> : std::true_type {};

/// @brief 检查类型是否被 reference_wrapper 包装
template <typename T>
inline constexpr bool is_reference_wrapper_v = is_reference_wrapper<std::decay_t<T>>::value;

/// @brief 左值引用包装函数。
/// @return 左值返回 std::ref 包装，右值直接转发。
template <typename T>
[[nodiscard]] constexpr auto wrap(T&& t) noexcept {
    if constexpr (std::is_lvalue_reference_v<T>) {
        return std::ref(t);
    } else {
        return std::forward<T>(t);
    }
}

/// @brief 获取包装函数的返回类型。
template <typename T>
using wrap_t = decltype(wrap(std::declval<T>()));

/// @brief 类型解包函数。
/// @return 如果有 reference_wrapper 包装则返回解包后的左值引用，否则原样转发。
template <typename T>
[[nodiscard]] constexpr decltype(auto) unwrap(T&& t) noexcept {
    if constexpr (is_reference_wrapper_v<T>) {
        return t.get();
    } else {
        return std::forward<T>(t);
    }
}

/// @brief 获取解包函数的返回类型。
template <typename T>
using unwrap_t = decltype(unwrap(std::declval<T>()));

} // namespace detail

// ============================================================================
//  Concepts - 类型约束
// ============================================================================

/// @brief 核心捕获约束：确保给定的所有类型均能被框架安全地持久化存储。
///
/// 满足以下任一条件即视为可捕获：
/// 1. 已被 std::ref 或 std::cref 显式包装。
/// 2. 是右值且具备移动构造能力（接管临时对象的所有权）。
/// 3. 是左值引用且具备拷贝构造能力。
template <typename... Ts>
concept capturable = ((
                            detail::is_reference_wrapper_v<Ts> ||                                     // std::ref -> OK
                            (!std::is_lvalue_reference_v<Ts> &&
                             std::is_move_constructible_v<std::decay_t<Ts>>) ||                // 右值且可移动 -> OK
                            (std::is_lvalue_reference_v<Ts> &&
                             std::is_copy_constructible_v<std::decay_t<Ts>>)                   // 左值且可拷贝 -> OK
                            ) && ...);

/// @brief 检查是否为有效的谓词类型
template <typename P, typename... Args>
concept predicate =
    std::invocable<std::decay_t<P>&, std::decay_t<Args>&...> &&
    std::same_as<std::invoke_result_t<std::decay_t<P>&, std::decay_t<Args>&...>, bool>;

/// @brief 检查是否为 noexcept 谓词
template <typename P, typename... Args>
concept noexcept_predicate =
    predicate<P, Args...> &&
    std::is_nothrow_invocable_v<std::decay_t<P>&, std::decay_t<Args>&...>;

/// @brief 检查是否为有效的回调类型
template <typename C>
concept callback = std::invocable<std::decay_t<C>&>;

/// @brief 检查是否为 Flow 任务图类型
template <typename F>
concept flow_type = std::same_as<std::remove_cvref_t<F>, Flow>;

// ============================================================================
//  任务节点类型约束
// ============================================================================

template <typename T, typename... Args>
concept basic_invocable =
    std::invocable<std::decay_t<T>&, detail::unwrap_t<Args>&...>;

template <typename T, typename... Args>
concept branch_invocable =
    std::invocable<std::decay_t<T>&, detail::unwrap_t<Args>&..., Branch&>;

template <typename T, typename... Args>
concept multi_branch_invocable =
    std::invocable<std::decay_t<T>&, detail::unwrap_t<Args>&..., MultiBranch&>;

template <typename T, typename... Args>
concept jump_invocable =
    std::invocable<std::decay_t<T>&, detail::unwrap_t<Args>&..., Jump&>;

template <typename T, typename... Args>
concept multi_jump_invocable =
    std::invocable<std::decay_t<T>&, detail::unwrap_t<Args>&..., MultiJump&>;

template <typename T, typename... Args>
concept runtime_invocable =
    std::invocable<std::decay_t<T>&, detail::unwrap_t<Args>&..., Runtime&>;

// ============================================================================
//  返回类型推导
// ============================================================================

template <typename T, typename... Args>
using basic_return_t = std::invoke_result_t<std::decay_t<T>&, detail::unwrap_t<Args>&...>;

template <typename T, typename... Args>
using branch_return_t = std::invoke_result_t<std::decay_t<T>&, detail::unwrap_t<Args>&..., Branch&>;

template <typename T, typename... Args>
using multi_branch_return_t = std::invoke_result_t<std::decay_t<T>&, detail::unwrap_t<Args>&..., MultiBranch&>;

template <typename T, typename... Args>
using jump_return_t = std::invoke_result_t<std::decay_t<T>&, detail::unwrap_t<Args>&..., Jump&>;

template <typename T, typename... Args>
using multi_jump_return_t = std::invoke_result_t<std::decay_t<T>&, detail::unwrap_t<Args>&..., MultiJump&>;

template <typename T, typename... Args>
using runtime_return_t = std::invoke_result_t<std::decay_t<T>&, detail::unwrap_t<Args>&..., Runtime&>;

// ============================================================================
//  任务节点综合类型约束
// ============================================================================

template <typename T, typename... Args>
concept valid_task_arg =
    (capturable<T, Args...> && basic_invocable<T, Args...>) ||
    (capturable<T, Args...> && branch_invocable<T, Args...>) ||
    (capturable<T, Args...> && multi_branch_invocable<T, Args...>) ||
    (capturable<T, Args...> && jump_invocable<T, Args...>) ||
    (capturable<T, Args...> && multi_jump_invocable<T, Args...>) ||
    (capturable<T, Args...> && runtime_invocable<T, Args...>) ||
    flow_type<T>;

// ============================================================================
//  枚举工具
// ============================================================================

namespace impl {
template <typename T>
struct EnumMaxImpl;
} // namespace impl

template <typename T>
constexpr std::int32_t EnumMax() noexcept {
    return impl::EnumMaxImpl<T>::Value;
}

} // namespace tfl
