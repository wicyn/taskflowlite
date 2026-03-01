#pragma once
#include "traits.hpp"
#include "graph.hpp"
#include "task.hpp"

namespace tfl {


/**
 * @class Flow
 * @brief 用于构建有向无环图 (DAG) 的任务构建器类。
 *        允许放置任务（静态或运行时，可选参数和循环次数）、
 *        组合子流程，并管理占位符。从 MoveOnly 继承，确保仅支持移动语义，防止拷贝。
 *        主要用于定义可由 Executor 类执行的执行流程。
 *
 * @note 该类设计用于编译时和运行时任务集成，支持变参放置以进行批量操作。
 */
class Flow {
    friend class Work;
    friend class Executor;
    friend class Task;
    friend class Runtime;

    // ---- 子类友元 ----
    TFL_WORK_SUBCLASS_FRIENDS;

public:
    /**
     * @brief 默认构造函数。初始化一个空图。
     */
    constexpr explicit Flow() = default;

    Flow(const Flow&) = delete;
    Flow& operator=(const Flow&) = delete;
    Flow(Flow&& other) noexcept = default;
    Flow& operator=(Flow&& other) noexcept = default;

    template <typename T>
        requires (capturable<T> && basic_invocable<T>)
    Task emplace(T&& task);

    template <typename T>
        requires (capturable<T> && branch_invocable<T>)
    Task emplace(T&& task);

    template <typename T>
        requires (capturable<T> && multi_branch_invocable<T>)
    Task emplace(T&& task);

    template <typename T>
        requires (capturable<T> && jump_invocable<T>)
    Task emplace(T&& task);

    template <typename T>
        requires (capturable<T> && multi_jump_invocable<T>)
    Task emplace(T&& task);

    template <typename T>
        requires (capturable<T> && runtime_invocable<T>)
    Task emplace(T&& task);

    template <typename F>
        requires flow_type<F>
    Task emplace(F&& subflow);

    template <typename F>
        requires flow_type<F>
    Task emplace(F&& subflow, std::uint64_t num);

    template <typename F, typename P>
        requires (flow_type<F> && capturable<P> && predicate<P>)
    Task emplace(F&& subflow, P&& pred);

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

    std::string dump(Direction dir = Direction::Default) const;

    void dump(std::ostream& ostream, Direction dir = Direction::Default) const;

    Flow& name(const std::string& n);
    [[nodiscard]] std::string_view name() const noexcept;
private:
    Graph m_graph;  ///< 存储任务和连接的内部图。
    std::string m_name;
};

// /////////////////////////////////////////////////////////////////////////////////////
template <typename T>
    requires (capturable<T> && basic_invocable<T>)
inline Task Flow::emplace(T&& task) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(Work::make_basic(&m_graph, options, std::forward<T>(task)))};
}

template <typename T>
    requires (capturable<T> && branch_invocable<T>)
inline Task Flow::emplace(T&& task) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(Work::make_branch(&m_graph, options, std::forward<T>(task)))};
}

template <typename T>
    requires (capturable<T> && multi_branch_invocable<T>)
inline Task Flow::emplace(T&& task) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(Work::make_multi_branch(&m_graph, options, std::forward<T>(task)))};
}

template <typename T>
    requires (capturable<T> && jump_invocable<T>)
inline Task Flow::emplace(T&& task) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(Work::make_jump(&m_graph, options, std::forward<T>(task)))};
}

template <typename T>
    requires (capturable<T> && multi_jump_invocable<T>)
inline Task Flow::emplace(T&& task) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(Work::make_multi_jump(&m_graph, options, std::forward<T>(task)))};
}

template <typename T>
    requires (capturable<T> && runtime_invocable<T>)
inline Task Flow::emplace(T&& task) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(Work::make_runtime(&m_graph, options, std::forward<T>(task)))};
}

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
    return Task{m_graph._emplace(Work::make_subflow(&m_graph, options, std::forward<F>(subflow), std::forward<P>(pred)))};
}

// 单个删除
inline void Flow::erase(Task t) noexcept {
    m_graph._erase(t.m_work);
}

// 多个删除 (变长参数模板)
template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
inline void Flow::erase(Ts&&... tasks) noexcept {
    // 展开调用：针对每一个 task 及其内部的 m_work 执行删除
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

Flow& Flow::name(const std::string& n) {
    m_name = n;
    return *this;
}

[[nodiscard]] std::string_view Flow::name() const noexcept {
    return m_name;
}

} // end of namespace tfl. ---------------------------------------------------


namespace std {

template <>
struct hash<tfl::Flow> {
    inline auto operator() (const tfl::Flow& f) const noexcept {
        return f.hash_value();
    }
};

}  // end of namespace std ----------------------------------------------------
