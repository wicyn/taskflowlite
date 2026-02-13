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

private:
    Graph m_graph;  ///< 存储任务和连接的内部图。
};

// /////////////////////////////////////////////////////////////////////////////////////

template <typename T>
    requires (capturable<T> && basic_invocable<T>)
inline Task Flow::emplace(T&& task) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(TaskType::Basic, options, Work::_make_basic(std::forward<T>(task)))};
}

template <typename T>
    requires (capturable<T> && branch_invocable<T>)
inline Task Flow::emplace(T&& task) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(TaskType::Branch, options, Work::_make_branch(std::forward<T>(task)))};
}

template <typename T>
    requires (capturable<T> && multi_branch_invocable<T>)
inline Task Flow::emplace(T&& task) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(TaskType::Branch, options, Work::_make_multi_branch(std::forward<T>(task)))};
}

template <typename T>
    requires (capturable<T> && jump_invocable<T>)
inline Task Flow::emplace(T&& task) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(TaskType::Jump, options, Work::_make_jump(std::forward<T>(task)))};
}

template <typename T>
    requires (capturable<T> && multi_jump_invocable<T>)
inline Task Flow::emplace(T&& task) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(TaskType::Jump, options, Work::_make_multi_jump(std::forward<T>(task)))};
}


template <typename T>
    requires (capturable<T> && runtime_invocable<T>)
inline Task Flow::emplace(T&& task) {
    constexpr auto options = Work::Option::NONE;
    return Task{m_graph._emplace(TaskType::Runtime, options, Work::_make_runtime(std::forward<T>(task)))};
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
    return Task{m_graph._emplace(TaskType::Graph, options, Work::_make_subflow(std::forward<F>(subflow), std::forward<P>(pred)))};
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

} // end of namespace tfl. ---------------------------------------------------


namespace std {

template <>
struct hash<tfl::Flow> {
    inline auto operator() (const tfl::Flow& f) const noexcept {
        return f.hash_value();
    }
};

}  // end of namespace std ----------------------------------------------------

