/// @file flow.hpp
/// @brief DAG 构建器 - 用户层任务图定义入口
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once
#include <sstream>

#include "traits.hpp"
#include "graph.hpp"
#include "task.hpp"
#include "work.hpp"
#include "work_memory_fwd.hpp"
#include "d2_render.hpp"
namespace tfl {

/// @brief DAG 构建器 —— 用户态任务图的蓝图（Blueprint）。
///
/// @details
/// `Flow` 是框架的 **声明式 DAG 编辑器**：用户用它"画"出任务节点与边，
/// 再把 `Flow` 整体提交给 `Executor` 反复执行。它本质上是一层薄薄的语义壳，
/// 内部托管一个 `Graph`（节点指针数组）和一个 `std::string` 名字。
///
/// 在框架的"静态 vs 动态" 维度上，`Flow` 站在静态侧；`Executor::dependent_async` /
/// `Executor::deferred_async` 与 `Runtime` 站在动态侧，分别表达图外提交和任务体内派生：
///
/// | 维度          | `Flow`              | `Executor` 动态任务        | `Runtime`            |
/// |---------------|--------------------|----------------------------|----------------------|
/// | 何时构造图    | 提交 **前**（静态） | API 调用时（图外动态）      | 任务体 **内**（图内动态） |
/// | 谁拥有节点    | Flow 自己          | Executor 内存池 / Topology | Executor 内存池 / Topology |
/// | 适用场景      | 结构稳定的批处理    | 外部线程的一次性派发        | 数据驱动的自适应分支  |
/// | 重复执行成本  | 极低（图复用）      | 一次性                     | 一次性                |
///
/// ============================================================================
///  所有权与生命周期
/// ============================================================================
/// `Flow` 是 **MoveOnly** —— 同一份图蓝图只能有一个所有者，避免节点指针被多份
/// 句柄重复 erase / clear。提交给 `Executor` 时传引用 / 右值，`Flow` 本身不被移交，
/// 这意味着 **同一 Flow 可以被同一个 Executor 反复提交**（典型用法：流水线常驻图）。
///
/// 节点物理内存归 `Graph::m_works` 持有 —— 通过 `work_memory` 池统一分配 / 释放，
/// `Flow` 析构时连带清理。返回给用户的 `Task` 是 **弱引用**（裸 Work*），不参与
/// 生命周期管理 —— 这点与 `AsyncTask` 的引用计数语义形成关键区分（详见 task.hpp）。
///
/// ============================================================================
///  emplace —— 唯一插入入口，按概念分发到 7 种节点工厂
/// ============================================================================
/// 所有节点插入收敛在 `emplace` 一个名字下，靠 C++20 concepts 在编译期分发：
///
/// | 概念                       | 节点类型             | 触发签名示例                          |
/// |---------------------------|----------------------|-------------------------------------|
/// | `basic_invocable`         | 普通同步任务          | `void()` / `void(Args...)`          |
/// | `branch_invocable`        | 二选一条件分支        | `bool()` → 走 succ[0] 或 succ[1]    |
/// | `multi_branch_invocable`  | N 选一条件分支        | `std::size_t()` → 走 succ[N]        |
/// | `jump_invocable`          | 单跳转                | `Task()` → 强制转到目标             |
/// | `multi_jump_invocable`    | 多跳转                | 返回若干目标 Task                    |
/// | `runtime_invocable`       | 运行时任务            | `void(Runtime&)`                    |
/// | `flow_type`               | 嵌套子流程（Subflow） | 整个 Flow 作为节点                  |
///
/// 每个重载内部通过对应的 `make_xxx` 工厂在 `work_memory` 池里构造具体 Work 子类，
/// 再用 `Graph::_emplace` 接管所有权。这一层"概念 → 工厂"的映射把"用户写什么签名
/// 就得到什么节点类型" 这件事完全推到编译期，运行期零成本。
///
/// ============================================================================
///  批量插入 —— 结构化绑定友好
/// ============================================================================
/// 两个变参重载支持一次性插入多个节点：
/// - `emplace(Ts&&...)`         —— 适用于无参闭包 / 子流程；
/// - `emplace(Packs&&...)`      —— 接收 `tfl::pack{callable, args...}`，
///                                内部 `std::apply` 展开后转发到单任务 `emplace`。
///
/// 都返回 `std::tuple<Task, ...>`，配合结构化绑定写：
/// @code
///   auto [t1, t2, t3] = flow.emplace(
///       tfl::pack{&Svc::work, &svc, 42},
///       tfl::pack{Functor{}, std::ref(state)},
///       []() noexcept { /* ... */ }
///   );
///   t1.precede(t2); t2.precede(t3);
/// @endcode
///
/// ============================================================================
///  图操作语义
/// ============================================================================
/// - `erase(Task)`              ：O(1)，靠 `Graph::_erase` 的 swap-with-last。
///                               注意 erase 不会断开邻边连接 —— 调用方必须先
///                               `clear_predecessors()` / `clear_successors()`。
/// - `clear()`                  ：清空整个图，内部节点全部归还内存池。
/// - `for_each(F)`              ：遍历当前节点，访问者收 `Task` 句柄。
/// - `dump(Direction)`          ：导出 D2 图形描述，可视化到 SVG / PNG。
///
/// ============================================================================
///  线程安全
/// ============================================================================
/// **构建期非线程安全** —— 这是有意为之的设计权衡：
/// - 图通常一次构建后反复执行，单线程构建符合大多数使用场景；
/// - 避免节点插入 / 边管理的内部加锁，让运行期完全无锁。
///
/// 一旦提交给 `Executor`，`Flow` 进入 **只读阶段**，调度器与多个 worker 可并发
/// 访问其 `Graph` —— 节点入度 / 出度由各自 Work 的原子计数器保护，结构本身不变。
/// 因此**正在执行的 Flow 不能被结构性修改**（emplace / erase / clear）。
///
/// ============================================================================
///  哈希与等值
/// ============================================================================
/// `Flow` 的哈希 = `Graph` 对象地址的哈希 —— 用于把 Flow 当作 `unordered_map` 的
/// 键（如缓存图 → 配置映射）。语义上两个 Flow 即便结构相同，地址不同就视作不同
/// 实体。如需结构等值需自行实现深比较。
///
/// ============================================================================
///  使用示例
/// ============================================================================
/// @code
///   tfl::Flow flow;
///   auto [a, b, c] = flow.emplace(
///       []{ load(); },
///       [](Runtime& rt){ rt.silent_async([]{ extra(); }); },   // 自适应分支
///       []{ save(); }
///   );
///   a.precede(b); b.precede(c);
///
///   // 反复执行同一图
///   executor.silent_async(flow, /*num=*/1000);
///
///   // 嵌套：把整张 flow 作为另一图的一个节点
///   tfl::Flow outer;
///   auto inner_task = outer.emplace(flow);   // Subflow 节点
/// @endcode
///
/// @see Task              本类返回的节点句柄
/// @see Graph             实际持有节点指针的容器
/// @see Executor          消费 Flow 的执行引擎
/// @see Runtime   动态侧的对偶 —— 不预先编图、运行期才决定结构
///
class Flow : public MoveOnly<Flow> {
    friend class Work;
    friend class Executor;
    friend class Task;
    friend class Runtime;

public:
    constexpr explicit Flow() = default;

    // =====================================================a===================
    //  节点插入接口
    // ========================================================================

    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    [[nodiscard]] Task emplace(T&& task, Args&&... args);

    template <typename T, typename... Args>
        requires (capturable<T, Args...> && branch_invocable<T, Args...>)
    [[nodiscard]] Task emplace(T&& task, Args&&... args);

    template <typename T, typename... Args>
        requires (capturable<T, Args...> && multi_branch_invocable<T, Args...>)
    [[nodiscard]] Task emplace(T&& task, Args&&... args);

    template <typename T, typename... Args>
        requires (capturable<T, Args...> && jump_invocable<T, Args...>)
    [[nodiscard]] Task emplace(T&& task, Args&&... args);

    template <typename T, typename... Args>
        requires (capturable<T, Args...> && multi_jump_invocable<T, Args...>)
    [[nodiscard]] Task emplace(T&& task, Args&&... args);

    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    [[nodiscard]] Task emplace(T&& task, Args&&... args);

    // 子流程
    template <typename F>
        requires flow_type<F>
    [[nodiscard]] Task emplace(F&& subflow);

    template <typename F>
        requires flow_type<F>
    [[nodiscard]] Task emplace(F&& subflow, std::uint64_t num);

    template <typename F, typename P>
        requires (flow_type<F> && capturable<P> && predicate<P>)
    [[nodiscard]] Task emplace(F&& subflow, P&& pred);

    // ========================================================================
    //  批量插入接口
    // ========================================================================

    // 1. 批量插入多个节点 (无参数或已通过 lambda 捕获的闭包)
    // 返回 tuple 以支持 auto [...] 结构化绑定
    // 批量插入（无参闭包 / Flow）
    template <typename... Ts>
        requires (sizeof...(Ts) > 1) && ((callback<Ts> || flow_type<Ts>) && ...)
    [[nodiscard]] auto emplace(Ts&&... tasks);

    // 2. 批量插入多个带参数的节点 (通过 std::tuple / std::pair 打包)
    // 返回 tuple 以支持 auto [...] 结构化绑定
    template <typename... Packs>
        requires (sizeof...(Packs) > 1) && (task_pack<Packs> && ...)
    [[nodiscard]] auto emplace(Packs&&... task_packs);

    // ========================================================================
    //  图操作接口
    // ========================================================================
    void erase(Task t) noexcept;
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    void erase(Ts&&... tasks) noexcept;

    [[nodiscard]] std::size_t hash_value() const noexcept;
    void clear() noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    template <typename F>
        requires std::invocable<F, Task>
    void for_each(F&& visitor) noexcept(std::is_nothrow_invocable_v<F, Task>);

    [[nodiscard]] std::string dump(Direction dir = Direction::Default) const;
    void dump(std::ostream& os, Direction dir = Direction::Default) const;

    template <typename S>
        requires std::constructible_from<std::string, S>
    Flow& name(S&& name);

    [[nodiscard]] std::string_view name() const noexcept;

    [[nodiscard]] Graph& graph() noexcept;
    [[nodiscard]] const Graph& graph() const noexcept;
protected:
    Graph m_graph;
    std::string m_name;
};

// ============================================================================
//  节点插入实现
// ============================================================================

template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
inline Task Flow::emplace(T&& task, Args&&... args) {
    return Task{m_graph._emplace(make_basic(std::addressof(m_graph), std::forward<T>(task), std::forward<Args>(args)...))};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && branch_invocable<T, Args...>)
inline Task Flow::emplace(T&& task, Args&&... args) {
    return Task{m_graph._emplace(make_branch(std::addressof(m_graph), std::forward<T>(task), std::forward<Args>(args)...))};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && multi_branch_invocable<T, Args...>)
inline Task Flow::emplace(T&& task, Args&&... args) {
    return Task{m_graph._emplace(make_multi_branch(std::addressof(m_graph), std::forward<T>(task), std::forward<Args>(args)...))};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && jump_invocable<T, Args...>)
inline Task Flow::emplace(T&& task, Args&&... args) {
    return Task{m_graph._emplace(make_jump(std::addressof(m_graph), std::forward<T>(task), std::forward<Args>(args)...))};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && multi_jump_invocable<T, Args...>)
inline Task Flow::emplace(T&& task, Args&&... args) {
    return Task{m_graph._emplace(make_multi_jump(std::addressof(m_graph), std::forward<T>(task), std::forward<Args>(args)...))};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
inline Task Flow::emplace(T&& task, Args&&... args) {
    return Task{m_graph._emplace(make_runtime(std::addressof(m_graph), std::forward<T>(task), std::forward<Args>(args)...))};
}

// ============================================================================
//  子流程插入实现
// ============================================================================

template <typename F>
    requires flow_type<F>
inline Task Flow::emplace(F&& subflow) {
    return emplace(std::forward<F>(subflow), 1ULL);
}

template <typename F>
    requires flow_type<F>
inline Task Flow::emplace(F&& subflow, std::uint64_t num) {
    auto counter = [remaining = num, reset = num]() mutable noexcept -> bool {
        if (remaining-- == 0) {
            remaining = reset;
            return true;
        }
        return false;
    };
    return emplace(std::forward<F>(subflow), std::move(counter));
}

template <typename F, typename P>
    requires (flow_type<F> && capturable<P> && predicate<P>)
inline Task Flow::emplace(F&& subflow, P&& pred) {
    return Task{m_graph._emplace(make_subflow(std::addressof(m_graph), std::forward<F>(subflow), std::forward<P>(pred)))};
}

// ============================================================================
//  批量插入实现
// ============================================================================

// 1. 批量插入多个无参数节点
template <typename... Ts>
    requires (sizeof...(Ts) > 1) && ((callback<Ts> || flow_type<Ts>) && ...)
inline auto Flow::emplace(Ts&&... tasks) {
    // 直接对每个 task 调用单任务的 emplace，并打包返回
    return std::make_tuple(this->emplace(std::forward<Ts>(tasks))...);
}

/// @brief 批量插入多个带参数的任务（tfl::pack 重载）。
///
/// @details 每个 tfl::pack 内部持有 decay 后的 tuple，
///   通过 std::apply 展开后转发到单任务 emplace 重载。
///
/// @code
///   auto [t1, t2, t3] = flow.emplace(
///       tfl::pack{&MyService::process, &service, 999},
///       tfl::pack{MyFunctor{10}, std::ref(counter)},
///       tfl::pack{free_func_ref, std::ref(counter)}
///   );
///   t1.precede(t2);
///   t2.precede(t3);
/// @endcode
///
/// @tparam Packs 参数包类型，每个必须是 tfl::pack。
/// @param task_packs 由 tfl::pack{callable, args...} 构造的任务描述。
/// @return std::tuple<Task, Task, ...>，每个 Task 对应一个插入的节点。
template <typename... Packs>
    requires (sizeof...(Packs) > 1) && (task_pack<Packs> && ...)
inline auto Flow::emplace(Packs&&... task_packs) {
    return std::make_tuple(
        std::apply(
            [this]<typename... Args>(Args&&... args) {
                return this->emplace(std::forward<Args>(args)...);
            },
            std::forward<Packs>(task_packs).data  // ← 取 .data（decay 后的 tuple）
            )...
        );
}

// ============================================================================
//  图操作实现
// ============================================================================

inline void Flow::erase(Task t) noexcept {
    m_graph._erase(t.m_work);
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
inline void Flow::erase(Ts&&... tasks) noexcept {
    (m_graph._erase(tasks.m_work), ...);
}

inline std::size_t Flow::hash_value() const noexcept {
    return std::hash<const Graph*>{}(std::addressof(m_graph));
}

inline void Flow::clear() noexcept {
    m_graph._clear();
}

inline bool Flow::empty() const noexcept {
    return m_graph.empty();
}

inline std::size_t Flow::size() const noexcept {
    return m_graph.size();
}

template <typename F>
    requires std::invocable<F, Task>
inline void Flow::for_each(F&& visitor) noexcept(std::is_nothrow_invocable_v<F, Task>) {
    for (auto* w : m_graph.m_works) {
        std::invoke(visitor, Task{w});
    }
}

// ============================================================================
//  dump 实现
// ============================================================================
inline std::string Flow::dump(Direction dir) const {
    std::ostringstream oss;
    dump(oss, dir);
    return std::move(oss).str();
}

inline void Flow::dump(std::ostream& os, Direction dir) const {
    os << "direction: " << to_string(dir) << "\n\n";

    char id[24];
    std::snprintf(id, sizeof(id), "p%zx",
                  reinterpret_cast<std::uintptr_t>(std::addressof(m_graph)));
    const std::string& display_name = m_name.empty() ? std::string(id) : m_name;

    os << "root: |md\n  <center>";
    D2Renderer::write_html_escaped(os, display_name);
    os << "<br/><span style=\"color: #6b7280;\">[ "
       << to_string(TaskType::Graph)
       << " ]</span></center>\n| {\n"
          "  shape: rectangle\n"
          "  label.near: top-center\n"
          "  style.fill: \"#e8f5e9\"\n"
          "  style.stroke: \"#10b981\"\n"
          "  style.stroke-width: 2\n"
          "  style.border-radius: 14\n\n";
    m_graph.dump(os);
    os << "}\n";
}
// ============================================================================
//  name 实现
// ============================================================================
template <typename S>
    requires std::constructible_from<std::string, S>
inline Flow& Flow::name(S&& name) {
    m_name = std::forward<S>(name);
    return *this;
}

inline std::string_view Flow::name() const noexcept {
    return m_name;
}

// ========================================================================
//  图访问接口
// ========================================================================

[[nodiscard]] Graph& Flow::graph() noexcept {
    return m_graph;
}

[[nodiscard]] const Graph& Flow::graph() const noexcept {
    return m_graph;
}

inline std::ostream& operator << (std::ostream& os, const Flow& task) {
    task.dump(os);
    return os;
}

} // namespace tfl

namespace std {
template <>
struct hash<tfl::Flow> {
    inline auto operator() (const tfl::Flow& f) const noexcept {
        return f.hash_value();
    }
};
} // namespace std
