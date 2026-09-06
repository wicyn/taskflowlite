/// @file flow_builder.hpp
/// @brief DAG 任务图公共构建接口。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-07-27
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <ranges>
#include <tuple>
#include <type_traits>
#include <utility>

#include "graph.hpp"
#include "task.hpp"
#include "traits.hpp"
#include "utility.hpp"
#include "work_factory_fwd.hpp"

namespace tfl {

/// @brief 为 `Flow` 与 `SubFlow` 提供统一的任务节点创建和图结构修改接口。
///
/// 构建器以非拥有引用绑定一个 `Graph`，通过 Work 工厂创建节点，再把节点所有权
/// 交给 Graph。它不单独管理图的生命周期，返回的 `Task` 也只是节点句柄。
/// @warning 图结构修改不得与图执行并发；构建器及其句柄不得超过底层图的生命周期。
class FlowBuilder : public Immovable<FlowBuilder> {
public:
    /// @brief 向当前图插入不执行用户 callable 的占位节点。
    /// @return 指向新节点的非拥有 `Task`；节点由当前图管理。
    /// @throws std::bad_alloc 节点或图存储分配失败。
    [[nodiscard]] Task placeholder();

    /// @brief 向当前图插入普通 callable 节点。
    /// @tparam T 满足 `basic_invocable` 的 callable 类型。
    /// @param task 要保存并在节点执行时调用的 callable。
    /// @return 指向新节点的非拥有 `Task`；节点由当前图管理。
    /// @throws std::bad_alloc 节点或图存储分配失败。
    /// @throws ... callable 的衰减存储构造失败时原样传播。
    /// @note callable 按衰减类型保存：左值构造副本，右值转发到节点存储。
    template <typename T>
        requires (basic_invocable<T> && capturable<T>)
    [[nodiscard]] Task emplace(T&& task);

    /// @brief 向当前图插入可选择一个后继的条件分支节点。
    /// @tparam T 满足 `branch_invocable` 的 callable 类型。
    /// @param task 执行时接收框架注入的 `Branch&`，用于选择一个后继。
    /// @return 指向新节点的非拥有 `Task`；节点由当前图管理。
    /// @throws std::bad_alloc 节点或图存储分配失败。
    /// @throws ... callable 的衰减存储构造失败时原样传播。
    /// @note 注入的 `Branch&` 仅在本次 callable 调用期间有效，不得保存或跨线程使用。
    template <typename T>
        requires (branch_invocable<T> && capturable<T>)
    [[nodiscard]] Task emplace(T&& task);

    /// @brief 向当前图插入可选择多个后继的条件分支节点。
    /// @tparam T 满足 `multi_branch_invocable` 的 callable 类型。
    /// @param task 执行时接收框架注入的 `MultiBranch&`，用于选择多个后继。
    /// @return 指向新节点的非拥有 `Task`；节点由当前图管理。
    /// @throws std::bad_alloc 节点或图存储分配失败。
    /// @throws ... callable 的衰减存储构造失败时原样传播。
    /// @note 注入的 `MultiBranch&` 仅在本次 callable 调用期间有效，不得保存或跨线程使用。
    template <typename T>
        requires (multi_branch_invocable<T> && capturable<T>)
    [[nodiscard]] Task emplace(T&& task);

    /// @brief 向当前图插入可强制激活一个后继的跳转节点。
    /// @tparam T 满足 `jump_invocable` 的 callable 类型。
    /// @param task 执行时接收框架注入的 `Jump&`，用于指定一个跳转目标。
    /// @return 指向新节点的非拥有 `Task`；节点由当前图管理。
    /// @throws std::bad_alloc 节点或图存储分配失败。
    /// @throws ... callable 的衰减存储构造失败时原样传播。
    /// @note 注入的 `Jump&` 仅在本次 callable 调用期间有效，不得保存或跨线程使用。
    template <typename T>
        requires (jump_invocable<T> && capturable<T>)
    [[nodiscard]] Task emplace(T&& task);

    /// @brief 向当前图插入可强制激活多个后继的跳转节点。
    /// @tparam T 满足 `multi_jump_invocable` 的 callable 类型。
    /// @param task 执行时接收框架注入的 `MultiJump&`，用于指定多个跳转目标。
    /// @return 指向新节点的非拥有 `Task`；节点由当前图管理。
    /// @throws std::bad_alloc 节点或图存储分配失败。
    /// @throws ... callable 的衰减存储构造失败时原样传播。
    /// @note 注入的 `MultiJump&` 仅在本次 callable 调用期间有效，不得保存或跨线程使用。
    template <typename T>
        requires (multi_jump_invocable<T> && capturable<T>)
    [[nodiscard]] Task emplace(T&& task);

    /// @brief 向当前图插入可在执行期间动态派发任务的运行时节点。
    /// @tparam T 满足 `runtime_invocable` 的 callable 类型。
    /// @param task 执行时接收框架注入的 `Runtime&`。
    /// @return 指向新节点的非拥有 `Task`；节点由当前图管理。
    /// @throws std::bad_alloc 节点或图存储分配失败。
    /// @throws ... callable 的衰减存储构造失败时原样传播。
    /// @note 注入的 `Runtime&` 仅在本次 callable 调用期间有效，不得保存或跨线程使用。
    template <typename T>
        requires (runtime_invocable<T> && capturable<T>)
    [[nodiscard]] Task emplace(T&& task);

    /// @brief 向当前图插入可在执行期间构建动态子图的节点。
    /// @tparam T 满足 `subflow_invocable` 的 callable 类型。
    /// @param task 执行时接收框架注入的 `SubFlow&`。
    /// @return 指向新节点的非拥有 `Task`；节点由当前图管理。
    /// @throws std::bad_alloc 节点或图存储分配失败。
    /// @throws ... callable 的衰减存储构造失败时原样传播。
    /// @note 注入的 `SubFlow&` 仅在本次 callable 调用期间有效，不得保存或跨线程使用。
    template <typename T>
        requires (subflow_invocable<T> && capturable<T>)
    [[nodiscard]] Task emplace(T&& task);

    /// @brief 向当前图插入执行指定子图一次的模块节点。
    /// @tparam Gh 满足 `graph_holder` 的子图持有者类型。
    /// @param gh 要执行的子图；左值按非拥有引用保存，右值按值保存到节点内。
    /// @return 指向新节点的非拥有 `Task`；节点由当前图管理。
    /// @throws std::bad_alloc 节点或图存储分配失败。
    /// @throws ... 子图持有者存储构造失败时原样传播。
    /// @warning 左值子图必须存活到该模块节点完成最后一次执行。
    template <graph_holder Gh>
    [[nodiscard]] Task emplace(Gh&& gh);

    /// @brief 向当前图插入最多执行指定子图 `num` 次的模块节点。
    /// @tparam Gh 满足 `graph_holder` 的子图持有者类型。
    /// @param gh 要执行的子图；左值按非拥有引用保存，右值按值保存到节点内。
    /// @param num 最大迭代次数；0 表示不执行子图。
    /// @return 指向新节点的非拥有 `Task`；节点由当前图管理。
    /// @throws std::bad_alloc 节点或图存储分配失败。
    /// @throws ... 子图持有者存储构造失败时原样传播。
    /// @warning 左值子图必须存活到该模块节点完成最后一次执行。
    /// @note 子图为空、拓扑停止或执行异常时，实际迭代次数可能少于 `num`。
    template <graph_holder Gh>
    [[nodiscard]] Task emplace(Gh&& gh, std::uint64_t num);

    /// @brief 向当前图插入由谓词控制迭代次数的模块节点。
    /// @tparam Gh 满足 `graph_holder` 的子图持有者类型。
    /// @tparam P 满足 `predicate` 的无参布尔 callable 类型。
    /// @param gh 要执行的子图；左值按非拥有引用保存，右值按值保存到节点内。
    /// @param pred 每轮执行前调用的终止谓词；返回 true 时不再启动下一轮。
    /// @return 指向新节点的非拥有 `Task`；节点由当前图管理。
    /// @throws std::bad_alloc 节点或图存储分配失败。
    /// @throws ... 子图持有者或谓词的存储构造失败时原样传播。
    /// @warning 左值子图必须存活到该模块节点完成最后一次执行。
    /// @note 谓词按衰减类型保存；空子图会直接结束迭代。
    template <graph_holder Gh, predicate P>
        requires capturable<P>
    [[nodiscard]] Task emplace(Gh&& gh, P&& pred);

    /// @brief 按参数顺序批量插入任务，支持 callable、子图与 `tfl::pack` 混合传入。
    /// @tparam Ts 至少两个参数，每个类型满足 `callback`、`graph_holder` 或 `task_pack`。
    /// @param tasks 无参 void callable、子图持有者或任务参数包。
    /// @return 与参数顺序一致的结果元组；单节点插入返回的 `Task` 为非拥有句柄。
    /// @throws std::bad_alloc 任一节点或图存储分配失败。
    /// @throws ... 任一节点参数的存储构造失败时原样传播。
    /// @note 普通参数直接转发给 emplace；参数包展开其 data 后转发给 emplace。
    /// @note 参数包展开后的参数必须匹配有效的 emplace 重载；返回结果不做元组扁平化。
    /// @note 每个元素的保存方式和生命周期要求与对应的 emplace 重载相同。
    /// @warning 批量插入不提供事务回滚；后续元素创建失败时，先前节点仍保留在图中。
    template <typename... Ts>
        requires (sizeof...(Ts) > 1) &&
                ((callback<Ts> || graph_holder<Ts> || task_pack<Ts>) && ...)
    [[nodiscard]] auto emplace(Ts&&... tasks);

    /// @brief 从当前图断开并销毁指定节点。
    /// @param task 待删除节点的非拥有句柄；空句柄或其他图的节点会被忽略。
    /// @post 成功删除后，节点及其全部连接关系失效；传入句柄本身不会被置空。
    /// @warning 成功删除后，所有指向该节点的 `Task` 和 `TaskView` 都会悬空。
    void erase(Task task) noexcept;

    /// @brief 按参数顺序从当前图断开并销毁多个节点。
    /// @tparam Ts 一个或多个 `Task` 句柄类型。
    /// @param tasks 待删除节点的非拥有句柄；空句柄或其他图的节点会被忽略。
    /// @pre 任意两个有效句柄不得指向同一个底层节点。
    /// @post 每个成功删除的节点及其连接关系失效；传入句柄不会被置空。
    /// @warning 成功删除后，所有指向对应节点的 `Task` 和 `TaskView` 都会悬空。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    void erase(Ts&&... tasks) noexcept;

    /// @brief 返回当前构建器所绑定图对象的身份哈希。
    /// @return 根据 `Graph` 对象地址计算的哈希值；图内容变化不会改变该值。
    [[nodiscard]] std::size_t hash_value() const noexcept;

    /// @brief 断开并销毁当前图中的全部节点。
    /// @post `empty()` 返回 true，原有节点及其连接关系全部失效。
    /// @warning 所有指向原有节点的 `Task` 和 `TaskView` 都会悬空。
    void clear() noexcept;

    /// @brief 判断当前图是否不包含节点。
    /// @return 图中没有节点时返回 true。
    [[nodiscard]] bool empty() const noexcept;

    /// @brief 返回当前图拥有的节点数量。
    /// @return 调用时图中 `Work` 节点的数量。
    [[nodiscard]] std::size_t size() const noexcept;

    /// @brief 按当前图的物理存储顺序访问每个节点。
    /// @tparam F 可接收非拥有 `Task` 的 visitor 类型。
    /// @param visitor 对每个节点调用一次的 callable。
    /// @throws ... visitor 抛出的异常在其调用不是 noexcept 时原样传播。
    /// @pre visitor 不得在遍历期间插入、删除或清空当前图。
    /// @note 传给 visitor 的 `Task` 仅借用节点，不延长节点生命周期。
    template <typename F>
        requires std::invocable<F&, Task>
    void for_each(F&& visitor) noexcept(std::is_nothrow_invocable_v<F&, Task>);

    /// @brief 按范围的遍历顺序将任务串联为线性依赖链。
    ///
    /// 对范围中的每一对相邻任务建立前驱关系：
    /// `tasks[0] -> tasks[1] -> ... -> tasks[n - 1]`。
    ///
    /// 支持所有元素类型为 Task 的 forward_range，例如：
    /// std::vector、std::array、std::list、std::span、std::set 以及 ranges view。
    ///
    /// @tparam R 满足 forward_range，且迭代元素去除 cvref 后必须为 Task。
    /// @param tasks 待串联的任务范围。
    ///
    /// @note 空范围或仅包含一个任务时不执行任何操作。
    /// @note 任务串联顺序完全取决于范围的遍历顺序。
    /// @note 本函数只修改 Work 节点之间的依赖边，不修改范围本身。
    /// @note 应当仅在任务图构建阶段调用，不得与图执行并发进行。
    template <std::ranges::forward_range R>
        requires std::same_as<std::remove_cvref_t<std::ranges::range_reference_t<R>>, Task>
    void linearize(R&& tasks);

    /// @brief 按参数排列顺序将多个任务串联为线性依赖链。
    ///
    /// 对每一对相邻任务建立前驱关系：
    /// `tasks[0] -> tasks[1] -> ... -> tasks[n - 1]`。
    ///
    /// 示例：
    /// @code
    /// flow.linearize(task1, task2, task3);
    /// // 等价于：task1.precede(task2); task2.precede(task3);
    /// @endcode
    ///
    /// @tparam Ts Task 参数类型包。所有参数去除 cvref 后必须为 Task。
    /// @param tasks 待串联的任务句柄，至少需要两个。
    ///
    /// @note 参数顺序决定任务的依赖顺序。
    /// @note 本函数只修改底层 Work 节点之间的依赖边，不修改 Task 句柄。
    /// @note 应当仅在任务图构建阶段调用，不得与任务图执行并发进行。
    template <typename... Ts>
        requires (sizeof...(Ts) > 1) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    void linearize(Ts&&... tasks);

    /// @brief 按初始化列表顺序将任务串联为线性依赖链。
    ///
    /// 提供该重载是为了支持直接使用花括号初始化列表：
    /// `linearize({task1, task2, task3})`。
    ///
    /// @param tasks 待串联的任务初始化列表。
    ///
    /// @note 本重载仅负责适配初始化列表，实际逻辑转发到 ranges 主实现。
    void linearize(std::initializer_list<Task> tasks);

    /// @brief 返回当前构建器所绑定图的可修改引用。
    /// @return 非拥有 `Graph&`；不得超过当前构建器及底层图的生命周期。
    /// @warning 通过该引用修改图结构时，不得与图执行或其他结构修改并发。
    [[nodiscard]] Graph& graph() noexcept;

    /// @brief 返回当前构建器所绑定图的只读引用。
    /// @return 非拥有 `const Graph&`；不得超过当前构建器及底层图的生命周期。
    [[nodiscard]] const Graph& graph() const noexcept;

protected:
    /// @brief 将构建器绑定到现有图对象。
    /// @param graph 由派生对象或外部上下文拥有的图，必须比本构建器存活更久。
    explicit FlowBuilder(Graph& graph) noexcept;

    /// @brief 销毁构建接口本身，不清空或销毁所绑定的图。
    ~FlowBuilder() = default;
private:
    Graph& m_graph;
};

inline FlowBuilder::FlowBuilder(Graph& graph) noexcept
    : m_graph{graph} {
}

inline Graph& FlowBuilder::graph() noexcept {
    return m_graph;
}

inline const Graph& FlowBuilder::graph() const noexcept {
    return m_graph;
}


// ============================================================================
// 节点创建
// ============================================================================

inline Task FlowBuilder::placeholder() {
    return Task{m_graph.emplace(make_placeholder(std::addressof(m_graph)))};
}

template <typename T>
    requires (basic_invocable<T> && capturable<T>)
inline Task FlowBuilder::emplace(T&& task) {
    return Task{m_graph.emplace(make_basic(std::addressof(m_graph), std::forward<T>(task)))};
}

template <typename T>
    requires (branch_invocable<T> && capturable<T>)
inline Task FlowBuilder::emplace(T&& task) {
    return Task{m_graph.emplace(make_branch(std::addressof(m_graph), std::forward<T>(task)))};
}

template <typename T>
    requires (multi_branch_invocable<T> && capturable<T>)
inline Task FlowBuilder::emplace(T&& task) {
    return Task{m_graph.emplace(make_multi_branch(std::addressof(m_graph), std::forward<T>(task)))};
}

template <typename T>
    requires (jump_invocable<T> && capturable<T>)
inline Task FlowBuilder::emplace(T&& task) {
    return Task{m_graph.emplace(make_jump(std::addressof(m_graph), std::forward<T>(task)))};
}

template <typename T>
    requires (multi_jump_invocable<T> && capturable<T>)
inline Task FlowBuilder::emplace(T&& task) {
    return Task{m_graph.emplace(make_multi_jump(std::addressof(m_graph), std::forward<T>(task)))};
}

template <typename T>
    requires (runtime_invocable<T> && capturable<T>)
inline Task FlowBuilder::emplace(T&& task) {
    return Task{m_graph.emplace(make_runtime(std::addressof(m_graph), std::forward<T>(task)))};
}

template <typename T>
    requires (subflow_invocable<T> && capturable<T>)
inline Task FlowBuilder::emplace(T&& task) {
    return Task{m_graph.emplace(make_subflow(std::addressof(m_graph), std::forward<T>(task)))};
}

template <graph_holder Gh>
inline Task FlowBuilder::emplace(Gh&& gh) {
    return emplace(std::forward<Gh>(gh), std::uint64_t{1});
}

template <graph_holder Gh>
inline Task FlowBuilder::emplace(Gh&& gh, std::uint64_t num) {
    auto counter = [num, remaining = num]() mutable noexcept -> bool {
        if (remaining-- == 0) [[unlikely]] {
            remaining = num;
            return true;
        }

        return false;
    };

    return emplace(std::forward<Gh>(gh), std::move(counter));
}

template <graph_holder Gh, predicate P>
    requires capturable<P>
inline Task FlowBuilder::emplace(Gh&& gh, P&& pred) {
    return Task{m_graph.emplace(make_module(std::addressof(m_graph), std::forward<Gh>(gh), std::forward<P>(pred)))};
}

template <typename... Ts>
    requires (sizeof...(Ts) > 1) &&
            ((callback<Ts> || graph_holder<Ts> || task_pack<Ts>) && ...)
inline auto FlowBuilder::emplace(Ts&&... tasks) {
    // 按参数类型选择直接插入或展开参数包。
    auto emplace_one = [this]<typename T>(T&& task) {
        if constexpr (task_pack<T>) {
            return std::apply(
                [this](auto&&... args) {
                    return this->emplace(
                        std::forward<decltype(args)>(args)...
                        );
                },
                std::forward<T>(task).data
                );
        } else {
            return this->emplace(std::forward<T>(task));
        }
    };

    // 花括号初始化保证各次插入按参数顺序执行。
    return std::tuple{
        emplace_one(std::forward<Ts>(tasks))...
    };
}

inline void FlowBuilder::erase(Task task) noexcept {
    m_graph.erase(task.m_work);
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
inline void FlowBuilder::erase(Ts&&... tasks) noexcept {
    (m_graph.erase(tasks.m_work), ...);
}

inline std::size_t FlowBuilder::hash_value() const noexcept {
    return std::hash<const Graph*>{}(std::addressof(m_graph));
}

inline void FlowBuilder::clear() noexcept {
    m_graph.clear();
}

inline bool FlowBuilder::empty() const noexcept {
    return m_graph.empty();
}

inline std::size_t FlowBuilder::size() const noexcept {
    return m_graph.size();
}

template <typename F>
    requires std::invocable<F&, Task>
inline void FlowBuilder::for_each(F&& visitor) noexcept(std::is_nothrow_invocable_v<F&, Task>) {
    for (Work* work : m_graph.m_works) {
        std::invoke(visitor, Task{work});
    }
}

template <std::ranges::forward_range R>
    requires std::same_as< std::remove_cvref_t<std::ranges::range_reference_t<R>>, Task>
inline void FlowBuilder::linearize(R&& tasks) {
    auto current = std::ranges::begin(tasks);
    const auto last = std::ranges::end(tasks);

    // 空范围不包含任何可串联任务。
    if (current == last) {
        return;
    }

    // current 指向当前任务，next 始终指向它的下一个任务。
    auto next = current;

    for (++next; next != last; ++current, ++next) {
        (*current).m_work->_precede((*next).m_work);
    }
}

template <typename... Ts>
    requires (sizeof...(Ts) > 1) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
inline void FlowBuilder::linearize(Ts&&... tasks) {
    const std::array<Work*, sizeof...(Ts)> works{tasks.m_work...};

    for (std::size_t i = 1; i < works.size(); ++i) {
        works[i - 1]->_precede(works[i]);
    }
}

inline void FlowBuilder::linearize(std::initializer_list<Task> tasks) {
    this->template linearize<std::initializer_list<Task>&>(tasks);
}

}  // namespace tfl
