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

// ── std::unwrap_ref_decay_t<T> 行为速查 ──────────────────────────────
// 框架内存储的 callable 类型均经 unwrap_ref_decay_t 处理:
// 1. Decay: 去引用/cv、数组/函数→指针(等价于值传递)
// 2. Unwrap: 若结果为 reference_wrapper<T> → T&
//
// 速查表:
// | 输入                     | 结果         |
// |--------------------------|-------------|
// | std::ref(x)             | int&        |
// | std::cref(x)            | const int&  |
// | int& / const int&       | int         |
// | int*                    | int*        |
// | int[3]                  | int*        |
// | void()                  | void(*)()   |

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


/// @brief 可安全持久化存储的参数包约束。
///
/// 包中每个 T 满足以下任一条件:
///   1. 已被 std::ref/std::cref 显式包装(引用捕获)
///   2. 是右值且 decay 后可移动构造(接管临时对象所有权)
///   3. 是左值引用且 decay 后可拷贝构造(拷贝活对象)
///
/// 异步代码中,把引用以值方式捕获到闭包里是常见的悬空 bug。
/// 本 concept 强制用户说清意图:用 std::ref 显式包裹、传右值、或传可拷贝左值。
///
/// - `capturable<int>`                  → true  (情形 3)
/// - `capturable<std::string&&>`        → true  (情形 2)
/// - `capturable<std::ref_wrapper<int>>`→ true  (情形 1)
/// - `capturable<std::unique_ptr<int>&>`→ false (不可拷贝构造)
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


/// @brief 检查是否为 Flow 任务图持有者。
///
/// 满足以下任一条件即可：
///   1. 类型本身是 `Graph`，或公开继承自 `Graph`；
///   2. 暴露完整的 graph() 访问接口：
///      - `Graph& graph()`
///      - `const Graph& graph() const`
template <typename Gh>
concept graph_holder = std::derived_from<std::remove_cvref_t<Gh>, Graph> || (
                           requires(std::remove_cvref_t<Gh>& gh) {
                               { gh.graph() } -> std::convertible_to<Graph&>;
                           } &&
                           requires(const std::remove_cvref_t<Gh>& gh) {
                               { gh.graph() } -> std::convertible_to<const Graph&>;
                           }
                           );

namespace detail {

/// @brief 从 graph_holder 中取出底层 `Graph&`。
template <graph_holder Gh>
[[nodiscard]] inline auto to_graph(Gh& gh) noexcept -> Graph& {
    using U = std::remove_cvref_t<Gh>;

    if constexpr (requires(U& x) {{ x.graph() } -> std::convertible_to<Graph&>; }) {
        return static_cast<Graph&>(gh.graph());
    } else {
        return static_cast<Graph&>(gh);
    }
}

/// @brief 从 graph_holder 中取出底层 `const Graph&`。
template <graph_holder Gh>
[[nodiscard]] inline auto to_graph(const Gh& gh) noexcept -> const Graph& {
    using U = std::remove_cvref_t<Gh>;

    if constexpr (requires(const U& x) {{ x.graph() } -> std::convertible_to<const Graph&>; }) {
        return static_cast<const Graph&>(gh.graph());
    } else {
        return static_cast<const Graph&>(gh);
    }
}

}
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

/// @brief `f(args...)` 可调用 —— 普通同步任务签名。
template <typename T, typename... Args>
concept basic_invocable_plain = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&...>;
/// @brief `f(args..., stop_token)` 可调用 —— 普通同步任务（带停止令牌）。
template <typename T, typename... Args>
concept basic_invocable_stoppable = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., std::stop_token>;
/// @brief 普通同步任务：plain 或 stoppable 至少满足其一。
template <typename T, typename... Args>
concept basic_invocable = basic_invocable_plain<T, Args...> || basic_invocable_stoppable<T, Args...>;

/// @brief `f(args..., Branch&)` 可调用 —— 单目标条件分支任务签名。
template <typename T, typename... Args>
concept branch_invocable_plain = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Branch&>;
/// @brief `f(args..., Branch&, stop_token)` 可调用 —— 条件分支任务（带停止令牌）。
template <typename T, typename... Args>
concept branch_invocable_stoppable = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Branch&, std::stop_token>;
/// @brief 条件分支任务：plain 或 stoppable 至少满足其一。
template <typename T, typename... Args>
concept branch_invocable = branch_invocable_plain<T, Args...> || branch_invocable_stoppable<T, Args...>;

/// @brief `f(args..., MultiBranch&)` 可调用 —— 多目标广播分支任务签名。
template <typename T, typename... Args>
concept multi_branch_invocable_plain = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., MultiBranch&>;
/// @brief `f(args..., MultiBranch&, stop_token)` 可调用 —— 多分支任务（带停止令牌）。
template <typename T, typename... Args>
concept multi_branch_invocable_stoppable = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., MultiBranch&, std::stop_token>;
/// @brief 多目标广播分支任务：plain 或 stoppable 至少满足其一。
template <typename T, typename... Args>
concept multi_branch_invocable = multi_branch_invocable_plain<T, Args...> || multi_branch_invocable_stoppable<T, Args...>;

/// @brief `f(args..., Jump&)` 可调用 —— 单目标强制跳转任务签名。
template <typename T, typename... Args>
concept jump_invocable_plain = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Jump&>;
/// @brief `f(args..., Jump&, stop_token)` 可调用 —— 跳转任务（带停止令牌）。
template <typename T, typename... Args>
concept jump_invocable_stoppable = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Jump&, std::stop_token>;
/// @brief 强制跳转任务：plain 或 stoppable 至少满足其一。
template <typename T, typename... Args>
concept jump_invocable = jump_invocable_plain<T, Args...> || jump_invocable_stoppable<T, Args...>;

/// @brief `f(args..., MultiJump&)` 可调用 —— 多目标广播跳转任务签名。
template <typename T, typename... Args>
concept multi_jump_invocable_plain = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., MultiJump&>;
/// @brief `f(args..., MultiJump&, stop_token)` 可调用 —— 多跳转任务（带停止令牌）。
template <typename T, typename... Args>
concept multi_jump_invocable_stoppable = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., MultiJump&, std::stop_token>;
/// @brief 多目标广播跳转任务：plain 或 stoppable 至少满足其一。
template <typename T, typename... Args>
concept multi_jump_invocable = multi_jump_invocable_plain<T, Args...> || multi_jump_invocable_stoppable<T, Args...>;

/// @brief `f(args..., Runtime&)` 可调用 —— 运行时动态调度任务签名。
template <typename T, typename... Args>
concept runtime_invocable_plain = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Runtime&>;
/// @brief `f(args..., Runtime&, stop_token)` 可调用 —— 运行时任务（带停止令牌）。
template <typename T, typename... Args>
concept runtime_invocable_stoppable = std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Runtime&, std::stop_token>;
/// @brief 运行时动态调度任务：plain 或 stoppable 至少满足其一。
template <typename T, typename... Args>
concept runtime_invocable = runtime_invocable_plain<T, Args...> || runtime_invocable_stoppable<T, Args...>;



// ============================================================================
//  返回类型推导
//
//  统一实现：所有分类（basic/branch/multi_branch/jump/multi_jump/runtime）
//  共用同一套 specialization，只在尾参 Tag... 上差异化。
//
//  Why class specialization (而非 std::conditional_t)：
//      conditional_t 的两条分支都会被立即实例化；
//      若 callable 不接受 stop_token，则 _stoppable 的 invoke_result_t 在
//      替换阶段直接硬错（no type named 'type'），击穿 SFINAE。
//      改用 requires + 偏特化后，未命中的分支根本不会被实例化，行为正确。
//
//  Why _plain 优先：与 invoker 端 `if constexpr (..._stoppable<...>)` 的
//      分派策略保持一致，避免 return_t 与运行时签名错位。
// ============================================================================

namespace detail {

/// @brief 尾参标签包：编码各分类追加在 Args... 之后的 framework 形参。
///
/// | 分类         | tail_pack          |
/// |--------------|--------------------|
/// | basic        | tail_pack<>        |
/// | branch       | tail_pack<Branch&> |
/// | multi_branch | tail_pack<MultiBranch&> |
/// | jump         | tail_pack<Jump&>   |
/// | multi_jump   | tail_pack<MultiJump&> |
/// | runtime      | tail_pack<Runtime&> |
template <typename... Ts> struct tail_pack {};

template <typename T, typename Pack, typename... Args>
struct invocable_return;

// plain 命中：f(args..., Tail...)
template <typename T, typename... Tail, typename... Args>
    requires std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Tail...>
struct invocable_return<T, tail_pack<Tail...>, Args...> {
    using type = std::invoke_result_t<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Tail...>;
};

// 仅 stoppable 命中：f(args..., Tail..., stop_token)
template <typename T, typename... Tail, typename... Args>
    requires (!std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Tail...>)
            &&  std::invocable<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Tail..., std::stop_token>
struct invocable_return<T, tail_pack<Tail...>, Args...> {
    using type = std::invoke_result_t<std::decay_t<T>&, std::unwrap_ref_decay_t<Args>&..., Tail..., std::stop_token>;
};

} // namespace detail

/// @brief `basic_invocable<T, Args...>` 的返回类型 —— `f(args...)` 或 `f(args..., stop_token)`。
template <typename T, typename... Args>
using basic_return_t        = typename detail::invocable_return<T, detail::tail_pack<>,            Args...>::type;
/// @brief `branch_invocable<T, Args...>` 的返回类型。
template <typename T, typename... Args>
using branch_return_t       = typename detail::invocable_return<T, detail::tail_pack<Branch&>,     Args...>::type;
/// @brief `multi_branch_invocable<T, Args...>` 的返回类型。
template <typename T, typename... Args>
using multi_branch_return_t = typename detail::invocable_return<T, detail::tail_pack<MultiBranch&>,Args...>::type;
/// @brief `jump_invocable<T, Args...>` 的返回类型。
template <typename T, typename... Args>
using jump_return_t         = typename detail::invocable_return<T, detail::tail_pack<Jump&>,       Args...>::type;
/// @brief `multi_jump_invocable<T, Args...>` 的返回类型。
template <typename T, typename... Args>
using multi_jump_return_t   = typename detail::invocable_return<T, detail::tail_pack<MultiJump&>,  Args...>::type;
/// @brief `runtime_invocable<T, Args...>` 的返回类型。
template <typename T, typename... Args>
using runtime_return_t      = typename detail::invocable_return<T, detail::tail_pack<Runtime&>,    Args...>::type;

/// @brief 约束 `Ts...` 为 `{Semaphore, count, Semaphore, count, ...}` 交替序列。
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
///   与等价的 `std::tuple<std::decay_t<Ts>...>` 完全相同，零额外开销。
///
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
///        与 AsyncHandle* / corun 同档,适合需要"把 child 异常截在我这里"的场景。
struct explicit_t  { explicit constexpr explicit_t() = default; };

} // namespace anchor

/// @brief 约束 `T` 为 `anchor::none_t`、`anchor::implicit_t` 或 `anchor::explicit_t` 之一。
template <typename T>
concept anchor_tag = std::same_as<T, anchor::none_t> || std::same_as<T, anchor::implicit_t> || std::same_as<T, anchor::explicit_t>;


// ============================================================
// AsyncTask —— mode tag 与概念家族
// ============================================================

/// @namespace task_mode
/// @brief AsyncTask 的执行模式标签 —— 描述任务在状态机上的允许行为。
///
/// AsyncTask 通过模板参数 `Mode` 在编译期区分"单次执行"和"可重复执行"两种语义。
/// 模式标签作为类型 tag 参与重载解析与 concept 约束,确保依赖图的拓扑安全性
/// (例如:可重复任务在编译期就被禁止作为其它任务的依赖)。
namespace task_mode {

/// @brief 不可重复任务模式 —— 状态机单调走完:Idle → Running → Finished。
///        启动一次后即耗尽,不允许 resubmit。
///        典型用途:作为他人依赖、组成静态 DAG 的叶节点。
struct nonrepeat_t { explicit constexpr nonrepeat_t() = default; };

/// @brief 可重复任务模式 —— 完成后可经 `Executor::resubmit` 反复启动,
///        每轮独立携带异常 / 信号量配额 / join_counter。
///        不允许作为其它任务的依赖,避免依赖图被多轮回放污染。
struct repeat_t    { explicit constexpr repeat_t()    = default; };

}  // namespace task_mode


// ============================================================
// mode tag 约束 concept
// ============================================================

/// @brief 约束 `T` 为合法的任务模式标签 —— 必须是 `task_mode::nonrepeat_t`
///        或 `task_mode::repeat_t` 之一。
///        用于 AsyncTask 的模板参数 SFINAE 守卫(配合类内 static_assert)。
template <typename T>
concept task_mode_tag = std::same_as<T, task_mode::nonrepeat_t> || std::same_as<T, task_mode::repeat_t>;


// ============================================================
// AsyncTask 类型别名
// ============================================================

/// @brief 不可重复异步任务句柄 —— `AsyncTask<task_mode::nonrepeat_t>` 简称。
///        默认句柄类型;可作为 submit 依赖。
using NonrepeatAsyncTask = AsyncTask<task_mode::nonrepeat_t>;

/// @brief 可重复异步任务句柄 —— `AsyncTask<task_mode::repeat_t>` 简称。
///        支持 resubmit;不可作为其它任务的依赖。
using RepeatAsyncTask    = AsyncTask<task_mode::repeat_t>;


// ============================================================
// AsyncTask 能力探测 concept(用于 Executor / Runtime 接口守卫)
// ============================================================

/// @brief 检测 `T` 是否为不可重复异步任务句柄(忽略 cv / 引用)。
///        Executor::submit 的依赖参数靠它约束:只有不可重复任务可作依赖。
template <typename T>
concept nonrepeat_async_task = std::same_as<std::remove_cvref_t<T>, NonrepeatAsyncTask>;

/// @brief 检测 `T` 是否为可重复异步任务句柄(忽略 cv / 引用)。
///        Executor::resubmit 靠它约束:只有可重复任务能被重启。
template <typename T>
concept repeat_async_task = std::same_as<std::remove_cvref_t<T>, RepeatAsyncTask>;

/// @brief 检测 `T` 是否为任意一种异步任务句柄(忽略 cv / 引用)。
///        Executor::submit 的首参靠它约束:两种 mode 都允许首次启动。
template <typename T>
concept any_async_task = nonrepeat_async_task<T> || repeat_async_task<T>;


// ─── 主 concept ────────────────────────────────────────────────────────
//
// 合法形态(H 是模板参数推导得到的类型,保留值类别):
//
//   ┌────────────────────────────────┬───────────────────┬──────────┐
//   │ 用户写法                       │ H 推导结果        │ 语义     │
//   ├────────────────────────────────┼───────────────────┼──────────┤
//   │ Executor e(h)         (lvalue) │ MyHandler&        │ 借用     │
//   │ Executor e(ch)  (const lvalue) │ const MyHandler&  │ 借用     │
//   │ Executor e(std::move(h))       │ MyHandler         │ 拥有(move) │
//   │ Executor e(MyHandler{...})     │ MyHandler         │ 拥有(move) │
//   └────────────────────────────────┴───────────────────┴──────────┘
//
// 约束:
//   1. remove_cvref_t<H> 必须派生自 WorkerHandler
//   2. WorkerHandler 是抽象类,无法构造,自动防御切片
//   3. lvalue 形态总是合法(借用,无需构造);
//      rvalue 形态必须能从 H 构造(拷贝/移动到堆)
template <class H>
concept worker_handle = std::derived_from<std::remove_cvref_t<H>, WorkerHandler> &&
                        (std::is_lvalue_reference_v<H> || std::constructible_from<std::remove_cvref_t<H>, H>);

} // namespace tfl

