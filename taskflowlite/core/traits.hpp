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
 * std::unwrap_ref_decay_t<T> 行为速查表
 * * 逻辑流程:
 * 1. 先 Decay (退化): 移除引用、cv限定符，数组/函数转指针 (类似于值传递)。
 * 2. 后 Unwrap (解包): 如果结果是 std::reference_wrapper，则还原为原始引用 (T&)。
 * ======================================================================================
 */

// -----------------------------------------------------------
// 场景 1: 针对 std::ref / std::cref (还原为引用)
// -----------------------------------------------------------
// int x;
// 1. std::ref(x)  -> std::reference_wrapper<int>
// 2. Decay        -> std::reference_wrapper<int> (保持不变)
// 3. Unwrap       -> int& (提取引用)
// using A = std::unwrap_ref_decay_t<decltype(std::ref(x))>;  // A == int&

// 1. std::cref(x) -> std::reference_wrapper<const int>
// 2. Decay        -> std::reference_wrapper<const int>
// 3. Unwrap       -> const int&
// using B = std::unwrap_ref_decay_t<decltype(std::cref(x))>; // B == const int&

// -----------------------------------------------------------
// 场景 2: 针对 普通引用 / 指针 / 值 (退化为裸类型)
// -----------------------------------------------------------
// using C = std::unwrap_ref_decay_t<int&>;        // C == int (引用被剥离)
// using D = std::unwrap_ref_decay_t<const int&>;  // D == int (const & 引用都被剥离)
// using E = std::unwrap_ref_decay_t<int>;         // E == int
// using F = std::unwrap_ref_decay_t<int*>;        // F == int* (指针保持不变)

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

详细推导过程
让我们一步步拆解 detail::unwrap_t<Args>& 在传入 std::ref 时的类型变化。

假设我们有一个类型 int，我们传入 std::ref(x)。

1. 类型推导
Args 被推导为 std::reference_wrapper<int>。

2. unwrap_t (即 std::unwrap_ref_decay_t) 的作用
std::unwrap_ref_decay_t 的定义是：如果类型是 std::reference_wrapper<T>，则结果为 T&。

输入： std::reference_wrapper<int>

输出： int& (注意：这里已经是引用了)

3. Concept 中的 & 修饰符
代码中写的是 detail::unwrap_t<Args>&（末尾有一个 &）。

现在我们将第 2 步的结果代入：

代入前： unwrap_t<Args> 是 int&。

代入后： int& + & -> int& &。

4. 引用折叠 (Reference Collapsing)
C++ 有明确的引用折叠规则：

T& & -> T&

T& && -> T&

T&& & -> T&

T&& && -> T&&

结果： int& & 折叠为 int&。

*/

namespace detail {

template <typename T>
struct is_reference_wrapper : std::false_type {};

template <typename T>
struct is_reference_wrapper<std::reference_wrapper<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_reference_wrapper_v = is_reference_wrapper<std::decay_t<T>>::value;

// 单一 concept，支持单参数和变参
template <typename... Ts>
concept capturable_t = ((
                            is_reference_wrapper_v<Ts> ||                                      // std::ref -> OK
                            (!std::is_lvalue_reference_v<Ts> &&
                             std::is_move_constructible_v<std::decay_t<Ts>>) ||                // 右值且可移动 -> OK
                            (std::is_lvalue_reference_v<Ts> &&
                             std::is_copy_constructible_v<std::decay_t<Ts>>)                   // 左值且可拷贝 -> OK
                            ) && ...);
// 简化别名
template <typename T>
using unwrap_t = std::unwrap_ref_decay_t<T>;

// 左值 -> reference_wrapper，右值 -> 移动
template <typename T>
constexpr auto wrap_if_lvalue(T&& t) noexcept {
    if constexpr (std::is_lvalue_reference_v<T>) {
        return std::ref(t);
    } else {
        return std::forward<T>(t);
    }
}

template <typename T>
using wrap_if_lvalue_t = decltype(wrap_if_lvalue(std::declval<T>()));

// 解包 reference_wrapper
template <typename T>
constexpr decltype(auto) unwrap(T&& t) noexcept {
    if constexpr (is_reference_wrapper_v<T>) {
        return t.get();
    } else {
        return std::forward<T>(t);
    }
}

} // namespace detail


template <typename... Ts>
concept capturable = detail::capturable_t<Ts...>;

// ============== 谓词约束（返回 bool 的可调用对象）==============

template <typename P, typename... Args>
concept predicate =
    std::invocable<detail::unwrap_t<P>&, detail::unwrap_t<Args>&...> &&
    std::same_as<std::invoke_result_t<detail::unwrap_t<P>&, detail::unwrap_t<Args>&...>, bool>;

template <typename P, typename... Args>
concept noexcept_predicate =
    predicate<P, Args...> &&
    std::is_nothrow_invocable_v<detail::unwrap_t<P>&, detail::unwrap_t<Args>&...>;

// ============== 异步任务回调 ==============

template <typename C>
concept callback = std::invocable<detail::unwrap_t<C>&>;

// ============== Flow 类型约束 ==============

template <typename F>
concept flow_type = std::same_as<std::remove_cvref_t<F>, Flow>;

// ============== 任务可调用约束 ==============

// 1. 基础调用约束：只接受解包后的 Args...
template <typename T>
concept basic_invocable =
    std::invocable<detail::unwrap_t<T>&>;

// 2. 条件调用约束：接受 Args... + Branch&
template <typename T>
concept branch_invocable =
    std::invocable<detail::unwrap_t<T>&, Branch&>;

// 2. 条件调用约束：接受 Args... + MultiBranch&
template <typename T>
concept multi_branch_invocable =
    std::invocable<detail::unwrap_t<T>&, MultiBranch&>;

// 3. 条件调用约束：接受 Args... + Jump&
template <typename T>
concept jump_invocable =
    std::invocable<detail::unwrap_t<T>&, Jump&>;

// 3. 条件调用约束：接受 Args... + MultiJump&
template <typename T>
concept multi_jump_invocable =
    std::invocable<detail::unwrap_t<T>&, MultiJump&>;


// 4. 运行时调用约束：接受 Args... + Runtime&
template <typename T>
concept runtime_invocable =
    std::invocable<detail::unwrap_t<T>&, Runtime&>;



// 1. 获取 basic_invocable 的返回类型
template <typename T>
using basic_return_t = std::invoke_result_t<detail::unwrap_t<T>&>;

// 2. 获取 branch_invocable 的返回类型
template <typename T>
using branch_return_t = std::invoke_result_t<detail::unwrap_t<T>&, Branch&>;

// 2. 获取 multi_branch_invocable 的返回类型
template <typename T>
using multi_branch_return_t = std::invoke_result_t<detail::unwrap_t<T>&, MultiBranch&>;

// 3. 获取 jump_invocable 的返回类型
template <typename T>
using jump_return_t = std::invoke_result_t<detail::unwrap_t<T>&, Jump&>;

// 3. 获取 multi_jump_invocable 的返回类型
template <typename T>
using multi_jump_return_t = std::invoke_result_t<detail::unwrap_t<T>&, MultiJump&>;

// 4. 获取 runtime_invocable 的返回类型
template <typename T>
using runtime_return_t = std::invoke_result_t<detail::unwrap_t<T>&, Runtime&>;


namespace impl {

template <typename T>
struct EnumMaxImpl;

} // namespace impl

template <typename T>
constexpr std::int32_t EnumMax() noexcept
{
    return impl::EnumMaxImpl<T>::Value;
}


} // namespace tfl
