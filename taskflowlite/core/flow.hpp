/// @file flow.hpp
/// @brief DAG 构建器 - 用户层任务图定义入口
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include "traits.hpp"
#include "graph.hpp"
#include "task.hpp"

namespace tfl {

/// @brief DAG 构建器 - 用户层任务图定义入口
///
/// @details
/// 负责管理任务节点的物理存储与生命周期。支持多种节点类型：
/// - 静态任务（Basic）
/// - 条件分支（Branch）
/// - 运行时动态任务（Runtime）
/// - 嵌套子流程（Subflow）
///
/// @note 所有权：Move-only，禁止拷贝，确保节点所有权的唯一性
class Flow {
    friend class Work;
    friend class Executor;
    friend class Task;
    friend class Runtime;

    TFL_WORK_SUBCLASS_FRIENDS;

public:
    constexpr explicit Flow() = default;

    Flow(const Flow&) = delete;
    Flow& operator=(const Flow&) = delete;
    Flow(Flow&& other) noexcept = default;
    Flow& operator=(Flow&& other) noexcept = default;

    // ========================================================================
    //  节点插入接口
    // ========================================================================

    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    Task emplace(T&& task, Args&&... args);

    template <typename T, typename... Args>
        requires (capturable<T, Args...> && branch_invocable<T, Args...>)
    Task emplace(T&& task, Args&&... args);

    template <typename T, typename... Args>
        requires (capturable<T, Args...> && multi_branch_invocable<T, Args...>)
    Task emplace(T&& task, Args&&... args);

    template <typename T, typename... Args>
        requires (capturable<T, Args...> && jump_invocable<T, Args...>)
    Task emplace(T&& task, Args&&... args);

    template <typename T, typename... Args>
        requires (capturable<T, Args...> && multi_jump_invocable<T, Args...>)
    Task emplace(T&& task, Args&&... args);

    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    Task emplace(T&& task, Args&&... args);

    // 子流程
    template <typename F>
        requires flow_type<F>
    Task emplace(F&& subflow);

    template <typename F>
        requires flow_type<F>
    Task emplace(F&& subflow, std::uint64_t num);

    template <typename F, typename P>
        requires (flow_type<F> && capturable<P> && predicate<P>)
    Task emplace(F&& subflow, P&& pred);

    // 批量插入多个节点，返回 tuple 以支持 auto [...] 结构化绑定
    template <typename... Ts>
        requires (sizeof...(Ts) > 1) && (valid_task_arg<Ts> && ...)
    auto emplace(Ts&&... tasks);

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
        requires (std::invocable<F, Task>)
    void for_each(F&& visitor) noexcept(std::is_nothrow_invocable_v<F, Task>);

    [[nodiscard]] std::string dump(Direction dir = Direction::Default) const;
    void dump(std::ostream& os, Direction dir = Direction::Default) const;

    Flow& name(const std::string& n);
    [[nodiscard]] std::string_view name() const noexcept;

private:
    Graph m_graph;
    std::string m_name;
};

// ============================================================================
//  节点插入实现
// ============================================================================

template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
inline Task Flow::emplace(T&& task, Args&&... args) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(
        Work::make_basic(&m_graph, options,
                         std::forward<T>(task), std::forward<Args>(args)...))};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && branch_invocable<T, Args...>)
inline Task Flow::emplace(T&& task, Args&&... args) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(
        Work::make_branch(&m_graph, options,
                          std::forward<T>(task), std::forward<Args>(args)...))};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && multi_branch_invocable<T, Args...>)
inline Task Flow::emplace(T&& task, Args&&... args) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(
        Work::make_multi_branch(&m_graph, options,
                                std::forward<T>(task), std::forward<Args>(args)...))};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && jump_invocable<T, Args...>)
inline Task Flow::emplace(T&& task, Args&&... args) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(
        Work::make_jump(&m_graph, options,
                        std::forward<T>(task), std::forward<Args>(args)...))};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && multi_jump_invocable<T, Args...>)
inline Task Flow::emplace(T&& task, Args&&... args) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(
        Work::make_multi_jump(&m_graph, options,
                              std::forward<T>(task), std::forward<Args>(args)...))};
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
inline Task Flow::emplace(T&& task, Args&&... args) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(
        Work::make_runtime(&m_graph, options,
                           std::forward<T>(task), std::forward<Args>(args)...))};
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
    constexpr auto options = Work::Option::PREEMPTED;
    return Task{m_graph._emplace(
        Work::make_subflow(&m_graph, options,
                           std::forward<F>(subflow), std::forward<P>(pred)))};
}

// ============================================================================
//  批量插入实现
// ============================================================================

template <typename... Ts>
    requires (sizeof...(Ts) > 1) && (valid_task_arg<Ts> && ...)
inline auto Flow::emplace(Ts&&... tasks) {
    return std::make_tuple(this->emplace(std::forward<Ts>(tasks))...);
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
    return std::hash<const Graph*>{}(&m_graph);
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
    requires (std::invocable<F, Task>)
inline void Flow::for_each(F&& visitor) noexcept(std::is_nothrow_invocable_v<F, Task>) {
    for (auto* w : m_graph.m_works) {
        std::invoke(visitor, Task{w});
    }
}

// ============================================================================
//  dump 实现
// ============================================================================

inline std::string Flow::dump(Direction dir) const {
    std::string out;
    out.reserve(m_graph.size() * 120 + 256);

    out += "direction: ";
    out += to_string(dir);
    out += "\n\n";

    if (!m_name.empty()) {
        out += "root: |md\n  <center>";
        out += m_name;
        out += "<br/><span style=\"color: #6b7280;\">[ ";
        out += to_string(TaskType::Graph);
        out += " ]</span></center>\n| {\n";
        out += "  shape: rectangle\n";
        out += "  label.near: top-center\n";
        out += "  style.fill: \"#e8f5e9\"\n";
        out += "  style.stroke: \"#10b981\"\n";
        out += "  style.stroke-width: 2\n";
        out += "  style.border-radius: 14\n\n";
        out += m_graph._dump();
        out += "}\n";
    } else {
        out += m_graph._dump();
    }

    return out;
}

inline void Flow::dump(std::ostream& os, Direction dir) const {
    os << "direction: " << to_string(dir) << "\n\n";

    if (!m_name.empty()) {
        os << "root: |md\n  <center>"
           << m_name
           << "<br/><span style=\"color: #6b7280;\">[ "
           << to_string(TaskType::Graph)
           << " ]</span></center>\n| {\n";
        os << "  shape: rectangle\n";
        os << "  label.near: top-center\n";
        os << "  style.fill: \"#e8f5e9\"\n";
        os << "  style.stroke: \"#10b981\"\n";
        os << "  style.stroke-width: 2\n";
        os << "  style.border-radius: 14\n\n";
        m_graph._dump(os);
        os << "}\n";
    } else {
        m_graph._dump(os);
    }
}

// ============================================================================
//  name 实现
// ============================================================================

inline Flow& Flow::name(const std::string& n) {
    m_name = n;
    return *this;
}

inline std::string_view Flow::name() const noexcept {
    return m_name;
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
