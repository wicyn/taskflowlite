/// @file traits.hpp
/// @brief 类型概念与萃取 —— 框架的 C++20 concepts 集中地
/// @details
/// 本文件汇集所有用户签名约束：
/// - `basic_invocable` / `runtime_invocable` / `branch_invocable` / ... ：
///   按签名形态分类的 invocable 概念，驱动 Flow::emplace 的重载分发；
/// - `graph_holder` / `predicate` / `callback` ：Flow 提交 API 的辅助约束；
/// - `capturable<T...>` ：确保用户传入的所有类型都能被框架安全持久化存储；
/// - `pack<Ts...>`     ：批量插入用的参数打包器（见下）。
///
/// 这些 concepts 让框架的 API 在类型不匹配时给出**清晰的编译错误**，
/// 而不是模板深处的"recursive instantiation 200 deep"。
///
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <stop_token>
#include <concepts>
#include <functional>
#include <type_traits>

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

// 1. 基础模板 (默认匹配失败)
template <typename... Ts>
struct is_sem_count_seq : std::false_type {};
// 2. 递归终止条件 (当参数包拆解为空时，匹配成功)
template <>
struct is_sem_count_seq<> : std::true_type {};

// 3. 递归拆包核心逻辑
template <typename S, typename C, typename... Rest>
struct is_sem_count_seq<S, C, Rest...>
    : std::bool_constant<
          std::is_lvalue_reference_v<S>
          && std::same_as<std::remove_cvref_t<S>, Semaphore>
          && std::convertible_to<C, std::size_t>
          && is_sem_count_seq<Rest...>::value
          > {};

// ============================================================================
//  reference_wrapper 探测
// ============================================================================

/// @brief 检查类型是否为 std::reference_wrapper 特化
template <typename T>
struct is_reference_wrapper : std::false_type {};

template <typename T>
struct is_reference_wrapper<std::reference_wrapper<T>> : std::true_type {};

/// @brief 检查 **原始类型** 是否为 reference_wrapper（不做隐式 decay）
template <typename T>
inline constexpr bool is_reference_wrapper_v = is_reference_wrapper<T>::value;

/// @brief 检查 decay 后是否为 reference_wrapper
/// @note 与 is_reference_wrapper_v 的区别：显式对 T 做 decay，语义更透明。
template <typename T>
inline constexpr bool is_reference_wrapper_after_decay_v = is_reference_wrapper<std::decay_t<T>>::value;

// ============================================================================
//  wrap / unwrap 运行时工具
// ============================================================================

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

/// @brief 获取 wrap 函数的返回类型
template <typename T>
using wrap_t = decltype(wrap(std::declval<T>()));

/// @brief 类型解包函数。
/// @return 如果有 reference_wrapper 包装则返回解包后的左值引用，否则原样转发。
template <typename T>
[[nodiscard]] constexpr decltype(auto) unwrap(T&& t) noexcept {
    if constexpr (is_reference_wrapper_after_decay_v<T>) {
        return t.get();
    } else {
        return std::forward<T>(t);
    }
}

} // namespace detail


/// @brief 核心捕获约束：确保给定的所有类型均能被框架安全地持久化存储。
///
/// @details
/// **满足以下任一条件即视为可捕获：**
///
/// 1. 已被 std::ref 或 std::cref 显式包装。
/// 2. 是右值且具备移动构造能力（接管临时对象的所有权）。
/// 3. 是左值引用且具备拷贝构造能力。
template <typename... Ts>
concept capturable = ((
                          detail::is_reference_wrapper_after_decay_v<Ts> ||
                          (!std::is_lvalue_reference_v<Ts> && std::is_move_constructible_v<std::decay_t<Ts>>) ||
                          ( std::is_lvalue_reference_v<Ts> && std::is_copy_constructible_v<std::decay_t<Ts>>)
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
///
///   1. 必须暴露 `Graph& graph()` 与 `const Graph& graph() const` 接口
template <typename Gh>
concept graph_holder = requires(std::remove_cvref_t<Gh>& g, const std::remove_cvref_t<Gh>& cg) {
        { g.graph()  } -> std::same_as<Graph&>;
        { cg.graph() } -> std::same_as<const Graph&>;
    };


// ============================================================================
//  Concepts — 任务节点类型约束
//
//  Args 经 std::unwrap_ref_decay_t 处理，与框架实际存储/传递 callable 参数
//  的类型一致。
//
//  每个 concept 拆为两个独立实现：
//    _plain     : f(args..., [Tail&])
//    _stoppable : f(args..., [Tail&], std::stop_token)
//  顶层 concept = _plain || _stoppable，便于 invoker 用
//  `if constexpr (xxx_stoppable<...>)` 精确分派。
//
//  Why: stop_token 是 shared_ptr 语义；用值接收 + std::move 进 invoke
//       省一次 atomic ref-count，与 std::jthread 惯例一致。
// ============================================================================

// ── basic ──────────────────────────────────────────────────────────────────
template <typename T, typename... Args>
concept basic_invocable_plain = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&...>;
template <typename T, typename... Args>
concept basic_invocable_stoppable = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., std::stop_token>;
template <typename T, typename... Args>
concept basic_invocable = basic_invocable_plain<T, Args...> || basic_invocable_stoppable<T, Args...>;

// ── branch ─────────────────────────────────────────────────────────────────
template <typename T, typename... Args>
concept branch_invocable_plain = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Branch&>;
template <typename T, typename... Args>
concept branch_invocable_stoppable = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Branch&, std::stop_token>;
template <typename T, typename... Args>
concept branch_invocable = branch_invocable_plain<T, Args...> || branch_invocable_stoppable<T, Args...>;

// ── multi_branch ───────────────────────────────────────────────────────────
template <typename T, typename... Args>
concept multi_branch_invocable_plain = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., MultiBranch&>;
template <typename T, typename... Args>
concept multi_branch_invocable_stoppable = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., MultiBranch&, std::stop_token>;
template <typename T, typename... Args>
concept multi_branch_invocable = multi_branch_invocable_plain<T, Args...> || multi_branch_invocable_stoppable<T, Args...>;

// ── jump ───────────────────────────────────────────────────────────────────
template <typename T, typename... Args>
concept jump_invocable_plain = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Jump&>;
template <typename T, typename... Args>
concept jump_invocable_stoppable = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Jump&, std::stop_token>;
template <typename T, typename... Args>
concept jump_invocable = jump_invocable_plain<T, Args...> || jump_invocable_stoppable<T, Args...>;

// ── multi_jump ─────────────────────────────────────────────────────────────
template <typename T, typename... Args>
concept multi_jump_invocable_plain = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., MultiJump&>;
template <typename T, typename... Args>
concept multi_jump_invocable_stoppable = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., MultiJump&, std::stop_token>;
template <typename T, typename... Args>
concept multi_jump_invocable = multi_jump_invocable_plain<T, Args...> || multi_jump_invocable_stoppable<T, Args...>;

// ── runtime ────────────────────────────────────────────────────────────────
template <typename T, typename... Args>
concept runtime_invocable_plain = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Runtime&>;
template <typename T, typename... Args>
concept runtime_invocable_stoppable = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Runtime&, std::stop_token>;
template <typename T, typename... Args>
concept runtime_invocable = runtime_invocable_plain<T, Args...> || runtime_invocable_stoppable<T, Args...>;


// ============================================================================
//  返回类型推导
//
//  xxx_return_t 选取实际命中的签名：
//    _stoppable 优先（只有 _stoppable 满足才走带 stop_token 的签名），
//    否则回退到 _plain。
//
//  Why: 与 invoker 端 `if constexpr (xxx_stoppable<...>)` 优先的分派策略
//       保持一致，避免 return_t 与运行时实际调用的签名不一致导致
//       Future<T> / Promise<T> 的 T 类型错位。
//  Why: 用 std::conditional_t 而非偏特化，是因为顶层的两个条件互斥地
//       挑选 invoke_result_t 实参列表，写一行更直接，编译期成本更低。
// ============================================================================

// ── basic ──────────────────────────────────────────────────────────────────
template <typename T, typename... Args>
using basic_return_plain_t = std::invoke_result_t<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&...>;
template <typename T, typename... Args>
using basic_return_stoppable_t = std::invoke_result_t<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., std::stop_token>;
template <typename T, typename... Args>
using basic_return_t = std::conditional_t<basic_invocable_plain<T, Args...>, basic_return_plain_t<T, Args...>, basic_return_stoppable_t<T, Args...>>;

// ── branch ─────────────────────────────────────────────────────────────────
template <typename T, typename... Args>
using branch_return_plain_t = std::invoke_result_t<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Branch&>;
template <typename T, typename... Args>
using branch_return_stoppable_t = std::invoke_result_t<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Branch&, std::stop_token>;
template <typename T, typename... Args>
using branch_return_t = std::conditional_t<branch_invocable_plain<T, Args...>, branch_return_plain_t<T, Args...>, branch_return_stoppable_t<T, Args...>>;

// ── multi_branch ───────────────────────────────────────────────────────────
template <typename T, typename... Args>
using multi_branch_return_plain_t = std::invoke_result_t<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., MultiBranch&>;
template <typename T, typename... Args>
using multi_branch_return_stoppable_t = std::invoke_result_t<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., MultiBranch&, std::stop_token>;
template <typename T, typename... Args>
using multi_branch_return_t = std::conditional_t<multi_branch_invocable_plain<T, Args...>, multi_branch_return_plain_t<T, Args...>, multi_branch_return_stoppable_t<T, Args...>>;

// ── jump ───────────────────────────────────────────────────────────────────
template <typename T, typename... Args>
using jump_return_plain_t = std::invoke_result_t<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Jump&>;
template <typename T, typename... Args>
using jump_return_stoppable_t = std::invoke_result_t<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Jump&, std::stop_token>;
template <typename T, typename... Args>
using jump_return_t = std::conditional_t<jump_invocable_plain<T, Args...>, jump_return_plain_t<T, Args...>, jump_return_stoppable_t<T, Args...>>;

// ── multi_jump ─────────────────────────────────────────────────────────────
template <typename T, typename... Args>
using multi_jump_return_plain_t = std::invoke_result_t<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., MultiJump&>;
template <typename T, typename... Args>
using multi_jump_return_stoppable_t = std::invoke_result_t<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., MultiJump&, std::stop_token>;
template <typename T, typename... Args>
using multi_jump_return_t = std::conditional_t<multi_jump_invocable_plain<T, Args...>, multi_jump_return_plain_t<T, Args...>, multi_jump_return_stoppable_t<T, Args...>>;

// ── runtime ────────────────────────────────────────────────────────────────
template <typename T, typename... Args>
using runtime_return_plain_t = std::invoke_result_t<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Runtime&>;
template <typename T, typename... Args>
using runtime_return_stoppable_t = std::invoke_result_t<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Runtime&, std::stop_token>;
template <typename T, typename... Args>
using runtime_return_t = std::conditional_t<runtime_invocable_plain<T, Args...>, runtime_return_plain_t<T, Args...>, runtime_return_stoppable_t<T, Args...>>;

template <typename... Ts>
concept sem_count_sequence = detail::is_sem_count_seq<Ts...>::value;


/// @brief 带自动 decay 的任务参数包。
///
/// @details 用于 `Flow::emplace(packs...)` 批量插入重载。
///   内部存储 `std::tuple<std::decay_t<Ts>...>`，构造时自动退化
///   函数类型和数组类型，避免 `std::tuple` CTAD 的 by-ref guide 陷阱。
///
/// @tparam Ts 原始参数类型（由 CTAD 推导，未退化）。
///
/// @par 内存布局
///   与等价的 `std::tuple<std::decay_t<Ts>...>` 完全相同，零额外开销。
///
/// @par 退化规则
///   | 原始类型          | decay 后            | 说明                  |
///   |-------------------|---------------------|-----------------------|
///   | `void(int&)`      | `void(*)(int&)`     | 函数 → 函数指针       |
///   | `int[5]`          | `int*`              | 数组 → 指针           |
///   | `const int&`      | `int`               | 去 cv + 去引用        |
///   | `ref_wrapper<T>`  | `ref_wrapper<T>`    | 不变（已是对象类型）  |
template <typename... Ts>
struct pack {
    /// @brief 退化后的内部存储类型。
    using tuple_type = std::tuple<std::decay_t<Ts>...>;

    tuple_type data;

    /// @brief 构造并自动 decay 所有参数。
    ///
    /// @param args 任务的可调用对象和参数，第一个通常是 callable，
    ///             后续是传给 callable 的参数（支持 std::ref）。
    constexpr pack(Ts&&... args)
        : data(std::forward<Ts>(args)...) {}
};

/// @brief CTAD guide：`tfl::pack{a, b, c}` → `pack<decltype(a), decltype(b), decltype(c)>`。
///
/// @details 注意 Ts... 推导的是**原始类型**（可能包含函数类型、数组类型），
///   退化发生在 pack 的成员类型 `std::tuple<std::decay_t<Ts>...>` 中，
///   而非 CTAD guide 本身——这是刻意设计：如果在 guide 中就 decay，
///   构造函数的参数类型会和 guide 推导的类型不匹配，导致编译错误。
template <typename... Ts>
pack(Ts&&...) -> pack<Ts...>;

// ============================================================================
//  pack 探测
// ============================================================================
namespace detail {

/// @brief pack 类型萃取（主模板：不是 pack）。
template <typename T>
struct is_pack_impl : std::false_type {};

/// @brief pack 类型萃取（特化：是 pack）。
template <typename... Args>
struct is_pack_impl<pack<Args...>> : std::true_type {};

}  // namespace detail

/// @brief 约束类型为 tfl::pack，拒绝裸 std::tuple。
///
/// @details 用于 `Flow::emplace(Packs&&...)` 的 requires 子句，
///   确保用户必须使用 `tfl::pack{...}` 构造参数包，
///   从而强制走 decay 路径，杜绝 CTAD 陷阱。
template <typename T>
concept task_pack = detail::is_pack_impl<std::remove_cvref_t<T>>::value;



namespace anchor {

/// @brief 不设锚 —— 异常沿 m_parent 链向上传播，由更上游的锚点归档。
///        语义最轻；适合"我就是个叶子，出错丢给爹处理"的场景。
struct none_t      { explicit constexpr none_t()     = default; };

/// @brief 隐式锚 —— 构造期置 Implicit::ANCHORED（非原子，运行期只读）。
///        语义：本节点天生是个异常汇聚点；与 Subflow / Runtime body 同档。
struct implicit_t  { explicit constexpr implicit_t() = default; };

/// @brief 显式锚 —— 构造期置 Explicit::ANCHORED（原子位）。
///        语义：本节点是运行期可被 AnchorGuard / corun 动态翻转的会话锚。
///        与 DepAsync* / corun 同档,适合需要"把 child 异常截在我这里"的场景。
struct explicit_t  { explicit constexpr explicit_t() = default; };

} // namespace anchor

// inline constexpr anchor::none_t     none_v{};
// inline constexpr anchor::implicit_t implicit_v{};
// inline constexpr anchor::explicit_t explicit_v{};   // ← 'explicit_v' 不是关键字

template <typename T>
concept anchor_tag = std::same_as<T, anchor::none_t>
                     || std::same_as<T, anchor::implicit_t>
                     || std::same_as<T, anchor::explicit_t>;

} // namespace tfl

