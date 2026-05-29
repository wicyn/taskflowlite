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
#include "work_factory_fwd.hpp"
#include "d2_render.hpp"
namespace tfl {

/// @brief DAG 构建器 —— 声明式任务图编辑器。
///
/// 内部持有 Graph，提供 emplace/precede/succeed 等 fluent API 构建 DAG。
/// MoveOnly，同一 Flow 可被 Executor 反复提交。节点内存由 Graph 统一管理，
/// 返回的 Task 是弱引用（裸 Work*），不参与生命周期管理。
///
/// emplace 按 C++20 concept 自动分发到 7 种节点工厂（basic/branch/jump/runtime/graph 等），
/// 编译期零开销。构建期非线程安全，提交后进入只读阶段供多 worker 并发访问。
/// 哈希基于 Graph 对象地址，可直接用作 unordered_map 键。
///
class Flow : public MoveOnly<Flow> {
    friend class Work;
    friend class Executor;
    friend class Task;
    friend class Runtime;

public:
    /// @brief 默认构造空 DAG，不含任何节点，name 为空字符串；与 `Flow{}` 等价。
    Flow() = default;


    /// @brief 构造带名字的任务图。
    /// @tparam S 任何可用于构造 std::string 的类型(const char*、string_view、string&& 等)。
    /// @param name 图的名字,用于调试和可视化输出。
    template <typename S>
        requires std::constructible_from<std::string, S>
    explicit Flow(S&& name);
    // ========================================================================
    //  节点插入接口
    // ========================================================================

    /// @brief 插入占位任务节点。
    ///
    /// @details 占位节点存在于图中,正常参与依赖计数与边遍历,但执行时不调用
    /// 任何用户逻辑、不触发观察者、不参与信号量与异常归档。常用于:
    ///   - 图构建期先声明占位、后续 `emplace` 重赋(尚未实现);
    ///   - 显式插入的同步汇聚点(命名清晰的 join point);
    ///   - 测试与基准里需要"空节点"占位拓扑结构。
    ///
    /// @return 指向占位节点的 Task 句柄。
    [[nodiscard]] Task noop();

    /// @brief 插入无 Runtime 参数的同步任务节点。
    /// @tparam T 可调用对象类型，满足 basic_invocable concept。
    /// @tparam Args 可捕获的附加参数类型。
    /// @param task 可调用对象（函数指针、lambda、std::function 等）。
    /// @param args 转发给 task 的参数（按值或 std::ref 捕获）。
    /// @return 指向新节点的 Task 句柄（弱引用），可用于 precede/succeed 构建依赖。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    [[nodiscard]] Task emplace(T&& task, Args&&... args);

    /// @brief 插入单目标分支任务节点，callable 接收 `Branch&` 选择恰好一个后继执行。
    /// @param task 满足 branch_invocable concept 的可调用对象。
    /// @param args 转发给 task 的附加参数。
    /// @return 指向新分支节点的 Task 句柄。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && branch_invocable<T, Args...>)
    [[nodiscard]] Task emplace(T&& task, Args&&... args);

    /// @brief 插入多目标分支任务节点，callable 接收 `MultiBranch&` 选择任意数量后继执行。
    /// @param task 满足 multi_branch_invocable concept 的可调用对象。
    /// @param args 转发给 task 的附加参数。
    /// @return 指向新多目标分支节点的 Task 句柄。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && multi_branch_invocable<T, Args...>)
    [[nodiscard]] Task emplace(T&& task, Args&&... args);

    /// @brief 插入单目标跳转任务节点，callable 接收 `Jump&` 将执行流转移到另一节点。
    /// @param task 满足 jump_invocable concept 的可调用对象。
    /// @param args 转发给 task 的附加参数。
    /// @return 指向新跳转节点的 Task 句柄。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && jump_invocable<T, Args...>)
    [[nodiscard]] Task emplace(T&& task, Args&&... args);

    /// @brief 插入多目标跳转任务节点，callable 接收 `MultiJump&` 将执行流转移到多个目标节点。
    /// @param task 满足 multi_jump_invocable concept 的可调用对象。
    /// @param args 转发给 task 的附加参数。
    /// @return 指向新多目标跳转节点的 Task 句柄。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && multi_jump_invocable<T, Args...>)
    [[nodiscard]] Task emplace(T&& task, Args&&... args);

    /// @brief 插入运行时任务节点，callable 接收 `Runtime&` 可在执行期动态派生/调度子任务。
    /// @param task 满足 runtime_invocable concept 的可调用对象。
    /// @param args 转发给 task 的附加参数。
    /// @return 指向新运行时节点的 Task 句柄。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    [[nodiscard]] Task emplace(T&& task, Args&&... args);

    /// @brief 插入子流程节点，执行时运行一整张 Flow（相当于调用一次）。
    /// @tparam Gh 满足 graph_holder concept 的类型。
    /// @param gh 要嵌入的子图。
    /// @return 指向新子流程节点的 Task 句柄。
    /// @note 等价于 `emplace(gh, 1ULL)`。
    template <typename Gh>
        requires graph_holder<Gh>
    [[nodiscard]] Task emplace(Gh&& gh);

    /// @brief 插入固定次数循环执行的子流程节点，子图连续运行 @p num 次。
    /// @tparam Gh 满足 graph_holder concept 的类型。
    /// @param gh 要嵌入的子图。
    /// @param num 循环次数。
    /// @return 指向新子流程节点的 Task 句柄。
    template <typename Gh>
        requires graph_holder<Gh>
    [[nodiscard]] Task emplace(Gh&& gh, std::uint64_t num);

    /// @brief 插入条件循环执行的子流程节点，每次迭代前调用 @p pred()，返回 true 时停止。
    /// @tparam Gh 满足 graph_holder concept 的类型。
    /// @tparam P 满足 predicate concept 的可调用对象。
    /// @param gh 要嵌入的子图。
    /// @param pred 循环终止谓词，返回 true 时退出循环。
    /// @return 指向新子流程节点的 Task 句柄。
    template <typename Gh, typename P>
        requires (graph_holder<Gh> && capturable<P> && predicate<P>)
    [[nodiscard]] Task emplace(Gh&& gh, P&& pred);

    // ========================================================================
    //  批量插入接口
    // ========================================================================

    /// @brief 批量插入多个无参闭包或子流程节点。
    /// @return Task 元组，支持结构化绑定。
    template <typename... Ts>
        requires (sizeof...(Ts) > 1) && ((callback<Ts> || graph_holder<Ts>) && ...)
    [[nodiscard]] auto emplace(Ts&&... tasks);

    /// @brief 批量插入多个已打包参数的任务节点。
    /// @return Task 元组，支持结构化绑定。
    template <typename... Packs>
        requires (sizeof...(Packs) > 1) && (task_pack<Packs> && ...)
    [[nodiscard]] auto emplace(Packs&&... task_packs);

    // ========================================================================
    //  图操作接口
    // ========================================================================

    /// @brief 从图中移除一个任务节点，同时清理其所有进出边并从相邻节点解链。
    /// @param t 要移除的任务句柄，移除后该句柄悬空。
    void erase(Task t) noexcept;

    /// @brief 从图中批量移除多个任务节点。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    void erase(Ts&&... tasks) noexcept;

    /// @brief 获取当前 Flow 的哈希值，基于内部 Graph 地址。
    [[nodiscard]] std::size_t hash_value() const noexcept;

    /// @brief 清空当前任务图中的所有节点。
    void clear() noexcept;

    /// @brief 判断图内是否无任何任务节点。
    /// @return 节点数为 0 时返回 true。
    [[nodiscard]] bool empty() const noexcept;

    /// @brief 返回图中任务节点总数，不含隐式辅助或临时节点。
    /// @return 节点数量。
    [[nodiscard]] std::size_t size() const noexcept;

    /// @brief 遍历当前任务图中的所有节点。
    template <typename F>
        requires std::invocable<F, Task>
    void for_each(F&& visitor) noexcept(std::is_nothrow_invocable_v<F, Task>);

    /// @brief 将当前任务图导出为 D2 描述字符串。
    [[nodiscard]] std::string dump(Direction dir = Direction::Default) const;

    /// @brief 将当前任务图的 D2 描述写入输出流。
    void dump(std::ostream& os, Direction dir = Direction::Default) const;

    /// @brief 设置当前任务图名称，用于调试和可视化。
    template <typename S>
        requires std::constructible_from<std::string, S>
    Flow& name(S&& name);

    /// @brief 返回图名称（调试/可视化用），直接读取 Flow::m_name。
    /// @return 图的名称 view；若未设置则为空字符串。
    [[nodiscard]] std::string_view name() const noexcept;

    /// @brief 获取内部 Graph 的可变引用，允许直接操作底层节点存储与边表。
    [[nodiscard]] Graph& graph() noexcept;

    /// @brief 获取内部 Graph 的只读引用，仅允许遍历/查询。
    [[nodiscard]] const Graph& graph() const noexcept;
protected:
    Graph m_graph;
    std::string m_name;
};

template <typename S>
    requires std::constructible_from<std::string, S>
inline Flow::Flow(S&& name) : m_name(std::forward<S>(name)) {}

// ============================================================================
//  节点插入实现
// ============================================================================

inline Task Flow::noop() {
    return Task{m_graph._emplace(make_noop(std::addressof(m_graph)))};
}

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

template <typename Gh>
    requires graph_holder<Gh>
inline Task Flow::emplace(Gh&& gh) {
    return emplace(std::forward<Gh>(gh), 1ULL);
}

template <typename Gh>
    requires graph_holder<Gh>
inline Task Flow::emplace(Gh&& gh, std::uint64_t num) {
    auto counter = [num, remaining = num]() mutable noexcept -> bool {
        if (remaining-- == 0) [[unlikely]] {
            remaining = num;
            return true;
        }
        return false;
    };
    return emplace(std::forward<Gh>(gh), std::move(counter));
}

template <typename Gh, typename P>
    requires (graph_holder<Gh> && capturable<P> && predicate<P>)
inline Task Flow::emplace(Gh&& gh, P&& pred) {
    return Task{m_graph._emplace(make_subflow(std::addressof(m_graph), std::forward<Gh>(gh), std::forward<P>(pred)))};
}

// ============================================================================
//  批量插入实现
// ============================================================================

// 1. 批量插入多个无参数节点
template <typename... Ts>
    requires (sizeof...(Ts) > 1) && ((callback<Ts> || graph_holder<Ts>) && ...)
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

inline Graph& Flow::graph() noexcept {
    return m_graph;
}

inline const Graph& Flow::graph() const noexcept {
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
