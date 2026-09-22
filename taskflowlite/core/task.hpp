/// @file task.hpp
/// @brief 任务句柄 Task 与只读视图 TaskView。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "work.hpp"
#include "semaphore.hpp"
#include "enums.hpp"
#include "traits.hpp"

namespace tfl {


// ============================================================================
// TaskView 只读视图
// ============================================================================
/// @brief 提供对一个既有任务节点的非空、只读、非拥有视图。
///
/// 视图只保存底层 `Work` 引用，可查询节点元数据和邻接关系，但不能修改图；
/// 复制视图不会复制节点或延长其生命周期。
///
/// @warning 底层节点必须在全部视图使用期间存活，且相关查询不得与图结构修改并发。
class TaskView {
    friend class Executor;
    friend class Branch;
    friend class MultiBranch;
    friend class Jump;
    friend class MultiJump;
    friend class Task;

public:
    /// @brief 判断两个只读视图是否引用同一个底层任务节点。
    [[nodiscard]] bool operator==(const TaskView& rhs) const noexcept;

    /// @brief 判断两个只读视图是否引用不同的底层任务节点。
    [[nodiscard]] bool operator!=(const TaskView& rhs) const noexcept;

    // ============================================================================
    // 状态查询
    // ============================================================================

    /// @brief 返回视图的哈希值，基于底层 Work 地址。
    /// @return std::hash<const Work*> 结果。
    [[nodiscard]] std::size_t hash_value() const noexcept;

    /// @brief 获取任务名称（调试/可视化用），直接读取底层 Work::m_name。
    /// @return 任务名称视图；底层名称被修改或节点销毁后失效。
    [[nodiscard]] std::string_view name() const noexcept;

    /// @brief 获取当前任务的后继节点数量，直接读取 Work::m_num_successors。
    /// @return 后继任务数量。
    [[nodiscard]] std::size_t num_successors() const noexcept;

    /// @brief 获取当前任务的前驱节点数量，通过 Work::_num_predecessors() 计算。
    /// @return 前驱任务数量。
    [[nodiscard]] std::size_t num_predecessors() const noexcept;

    /// @brief 获取执行前需要获取的信号量约束数量。
    /// @return 信号量获取项数量。
    [[nodiscard]] std::size_t num_acquires() const noexcept;

    /// @brief 获取执行后需要释放的信号量约束数量。
    /// @return 信号量释放项数量。
    [[nodiscard]] std::size_t num_releases() const noexcept;

    /// @brief 获取已注册在任务上的观察者数量。
    /// @return 观察者数量，0 表示无观察者或观察者数据尚未分配。
    [[nodiscard]] std::size_t num_observers() const noexcept;

    /// @brief 获取底层任务节点类型，由 Work 当前 Payload 的 Invoker 类型决定。
    /// @return 当前 Payload 对应的 TaskType。
    [[nodiscard]] TaskType type() const noexcept;

    /// @brief 检测任务是否已经保存异常指针。
    ///
    /// 通过检查底层 Work 的 `m_exception_ptr` 是否非空判断当前任务是否已经归档异常。
    ///
    /// @return 已经保存异常指针时返回 true。
    [[nodiscard]] bool has_exception_ptr() const noexcept;

    /// @brief 获取任务执行期间归档的异常指针。
    ///
    /// 直接返回底层 Work 保存的 `m_exception_ptr`；未归档异常时返回空
    /// `std::exception_ptr`。
    ///
    /// @return 当前任务归档的异常指针，可能为空。
    [[nodiscard]] std::exception_ptr exception_ptr() const noexcept;

    /// @brief 将当前任务节点导出为 D2 描述字符串。
    [[nodiscard]] std::string dump(Direction dir = Direction::Default) const;

    /// @brief 将当前任务节点的 D2 描述写入输出流。
    void dump(std::ostream& ostream, Direction dir = Direction::Default) const;

    // ============================================================================
    // 迭代访问
    // ============================================================================

    /// @brief 遍历当前任务的所有前驱只读节点，对每个调用 visitor(TaskView{*predecessor})。
    /// @tparam F 可调用对象，接收 TaskView 只读视图。
    /// @param visitor 访问器，只允许查询前驱节点。
    template <typename F>
        requires std::invocable<F&, TaskView>
    void for_each_predecessor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F&, TaskView>);

    /// @brief 遍历当前任务的所有后继只读节点，对每个调用 visitor(TaskView{*successor})。
    /// @tparam F 可调用对象，接收 TaskView 只读视图。
    /// @param visitor 访问器，只允许查询后继节点。
    template <typename F>
        requires std::invocable<F&, TaskView>
    void for_each_successor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F&, TaskView>);

    /// @brief 遍历任务执行前的信号量获取约束，对每个调用 visitor(const Semaphore&, std::size_t) 或 visitor(const Semaphore&)。
    /// @tparam F 可调用对象，可接收 (const Semaphore&, std::size_t) 或仅 (const Semaphore&)。
    /// @param visitor 访问器，信号量与配额均为只读。
    template <typename F>
        requires std::invocable<F&, const Semaphore&, std::size_t>
                 || std::invocable<F&, const Semaphore&>
    void for_each_acquire(F&& visitor) const noexcept(
        std::invocable<F&, const Semaphore&, std::size_t>
            ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
            : std::is_nothrow_invocable_v<F&, const Semaphore&>);

    /// @brief 遍历任务执行后的信号量释放约束，对每个调用 visitor(const Semaphore&, std::size_t) 或 visitor(const Semaphore&)。
    /// @tparam F 可调用对象，可接收 (const Semaphore&, std::size_t) 或仅 (const Semaphore&)。
    /// @param visitor 访问器，信号量与配额均为只读。
    template <typename F>
        requires std::invocable<F&, const Semaphore&, std::size_t>
                 || std::invocable<F&, const Semaphore&>
    void for_each_release(F&& visitor) const noexcept(
        std::invocable<F&, const Semaphore&, std::size_t>
            ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
            : std::is_nothrow_invocable_v<F&, const Semaphore&>);

private:
    /// @brief 从底层 Work 引用构造只读任务视图。
    explicit TaskView(const Work& work) noexcept;

    const Work& m_work;  ///< 底层 Work 节点只读引用，非拥有。
};


// ============================================================================
// TaskView 实现
// ============================================================================

inline TaskView::TaskView(const Work& work) noexcept : m_work{work} {}

inline bool TaskView::operator==(const TaskView& rhs) const noexcept {
    return std::addressof(m_work) == std::addressof(rhs.m_work);
}

inline bool TaskView::operator!=(const TaskView& rhs) const noexcept {
    return std::addressof(m_work) != std::addressof(rhs.m_work);
}

inline std::size_t TaskView::hash_value() const noexcept {
    return std::hash<const Work*>{}(std::addressof(m_work));
}

inline std::string_view TaskView::name() const noexcept {
    return m_work._name();
}

inline std::size_t TaskView::num_successors() const noexcept {
    return m_work.m_num_successors;
}

inline std::size_t TaskView::num_predecessors() const noexcept {
    return m_work._num_predecessors();
}

inline std::size_t TaskView::num_acquires() const noexcept {
    return m_work._num_acquires();
}

inline std::size_t TaskView::num_releases() const noexcept {
    return m_work._num_releases();
}

inline std::size_t TaskView::num_observers() const noexcept {
    return m_work._num_observers();
}

inline TaskType TaskView::type() const noexcept {
    return m_work.type();
}

inline bool TaskView::has_exception_ptr() const noexcept {
    return m_work.m_exception_ptr != nullptr;
}

inline std::exception_ptr TaskView::exception_ptr() const noexcept {
    return m_work.m_exception_ptr;
}

inline std::string TaskView::dump(Direction dir) const {
    std::ostringstream oss;
    dump(oss, dir);
    return std::move(oss).str();
}

inline void TaskView::dump(std::ostream& os, Direction dir) const {
    os << "direction: " << to_string(dir) << "\n\n";
    m_work.dump(os);
    os << "\n";
}

template <typename F>
    requires std::invocable<F&, TaskView>
inline void TaskView::for_each_predecessor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F&, TaskView>) {
    for (const Work* predecessor : m_work._predecessors()) {
        std::invoke(visitor, TaskView{*predecessor});
    }
}

template <typename F>
    requires std::invocable<F&, TaskView>
inline void TaskView::for_each_successor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F&, TaskView>) {
    for (const Work* successor : m_work._successors()) {
        std::invoke(visitor, TaskView{*successor});
    }
}

template <typename F>
    requires std::invocable<F&, const Semaphore&, std::size_t>
             || std::invocable<F&, const Semaphore&>
inline void TaskView::for_each_acquire(F&& visitor) const noexcept(
    std::invocable<F&, const Semaphore&, std::size_t>
        ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
        : std::is_nothrow_invocable_v<F&, const Semaphore&>) {
    for (const auto& req : m_work._acquires()) {
        if constexpr (std::invocable<F&, const Semaphore&, std::size_t>) {
            std::invoke(visitor, *req.sem, req.count);
        } else {
            std::invoke(visitor, *req.sem);
        }
    }
}

template <typename F>
    requires std::invocable<F&, const Semaphore&, std::size_t>
             || std::invocable<F&, const Semaphore&>
inline void TaskView::for_each_release(F&& visitor) const noexcept(
    std::invocable<F&, const Semaphore&, std::size_t>
        ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
        : std::is_nothrow_invocable_v<F&, const Semaphore&>) {
    for (const auto& req : m_work._releases()) {
        if constexpr (std::invocable<F&, const Semaphore&, std::size_t>) {
            std::invoke(visitor, *req.sem, req.count);
        } else {
            std::invoke(visitor, *req.sem);
        }
    }
}



/// @brief 提供创建依赖、配置节点属性和查询状态的可空非拥有任务句柄。
///
/// 句柄只保存 `Work*`；复制会产生别名，移动会清空源句柄，任何操作都不会复制
/// 或延长节点生命周期。节点由所属 `Flow`、动态子图或异步任务状态管理。
///
/// @warning `valid()` 只能检查非空，不能识别悬空句柄；图修改和句柄操作不得与节点执行并发。
class Task {
    friend class FlowBuilder;
    friend class Runtime;
    friend class SubFlow;
    friend class Executor;

public:
    /// @brief 默认构造空任务句柄，m_work 为 nullptr，valid() 返回 false。
    Task() = default;

    /// @brief 显式构造空任务句柄，等价于默认构造。
    explicit Task(std::nullptr_t) noexcept;

    /// @brief 拷贝构造，浅复制底层 Work 指针；两个句柄指向同一节点。
    Task(const Task& rhs) noexcept;

    /// @brief 拷贝赋值，浅复制底层 Work 指针；两个句柄指向同一节点。
    Task& operator=(const Task& rhs) noexcept;

    /// @brief 移动构造，接管 rhs 的底层 Work 指针并将 rhs 置空。
    Task(Task&& rhs) noexcept;

    /// @brief 移动赋值，接管 rhs 的底层 Work 指针并将 rhs 置空。
    Task& operator=(Task&& rhs) noexcept;

    /// @brief 将当前句柄置空（m_work = nullptr），等价于 `reset()`。
    Task& operator=(std::nullptr_t) noexcept;

    /// @brief 判断两个任务句柄是否指向同一个底层节点。
    [[nodiscard]] bool operator==(const Task& rhs) const noexcept;

    /// @brief 判断两个任务句柄是否指向不同底层节点。
    [[nodiscard]] bool operator!=(const Task& rhs) const noexcept;

    /// @brief 将当前句柄置空（m_work = nullptr），等价于 `operator=(nullptr)`。
    void reset() noexcept;

    // ============================================================================
    // 状态查询
    // ============================================================================

    /// @brief 返回句柄的哈希值，基于底层 Work 指针地址，支持 unordered_map 直接使用。
    /// @return std::hash<const Work*> 结果。
    [[nodiscard]] std::size_t hash_value() const noexcept;

    /// @brief 获取任务名称（调试/可视化用），直接读取底层 Work::m_name。
    /// @return 非空 Task 返回名称视图；空 Task 返回空视图。名称被修改或节点销毁后失效。
    [[nodiscard]] std::string_view name() const noexcept;

    /// @brief 判断当前句柄是否绑定了有效任务节点（m_work != nullptr）。
    /// @return 仅当内部指针非空时返回 true；悬空状态无法检测，仍可能返回 true。
    [[nodiscard]] bool valid() const noexcept;

    /// @brief 获取当前任务的后继节点数量，直接读取 Work::m_num_successors。
    /// @return 后继任务数量。
    [[nodiscard]] std::size_t num_successors() const noexcept;

    /// @brief 获取当前任务的前驱节点数量，通过 Work::_num_predecessors() 计算。
    /// @return 前驱任务数量。
    [[nodiscard]] std::size_t num_predecessors() const noexcept;

    /// @brief 获取执行前需要获取的信号量约束数量。
    /// @return 信号量获取项数量。
    [[nodiscard]] std::size_t num_acquires() const noexcept;

    /// @brief 获取执行后需要释放的信号量约束数量。
    /// @return 信号量释放项数量。
    [[nodiscard]] std::size_t num_releases() const noexcept;

    /// @brief 获取已注册在任务上的观察者数量。
    /// @return 观察者数量，0 表示无观察者或观察者数据尚未分配。
    [[nodiscard]] std::size_t num_observers() const noexcept;

    /// @brief 检测当前句柄是否非空（m_work != nullptr），与 valid() 语义等价，支持 `if(task)`。
    [[nodiscard]] explicit operator bool() const noexcept;

    /// @brief 获取底层任务节点类型，由 Work 当前 Payload 的 Invoker 类型决定。
    /// @return TaskType 枚举值（Basic, Branch, Runtime, Graph 等）。
    [[nodiscard]] TaskType type() const noexcept;

    /// @brief 检测任务是否已经保存异常指针。
    ///
    /// 通过检查底层 Work 的 `m_exception_ptr` 是否非空判断当前任务是否已经归档异常。
    ///
    /// @return 已经保存异常指针时返回 true；空 Task 返回 false。
    [[nodiscard]] bool has_exception_ptr() const noexcept;

    /// @brief 获取任务执行期间归档的异常指针。
    ///
    /// 直接返回底层 Work 保存的 `m_exception_ptr`；空 Task 或未归档异常时返回空
    /// `std::exception_ptr`。
    ///
    /// @return 当前任务归档的异常指针，可能为空。
    [[nodiscard]] std::exception_ptr exception_ptr() const noexcept;

    /// @brief 将当前任务节点导出为 D2 描述字符串。
    [[nodiscard]] std::string dump(Direction dir = Direction::Default) const;

    /// @brief 将当前任务节点的 D2 描述写入输出流。
    void dump(std::ostream& ostream, Direction dir = Direction::Default) const;

    // ============================================================================
    // 执行体重绑定
    // ============================================================================

    /// @brief 将当前静态图节点的执行体替换为普通 callable。
    ///
    /// Work 节点身份、名称、边关系、信号量和观察者保持不变，仅替换内部 Payload。
    /// 典型用途是先通过 `FlowBuilder::placeholder()` 建立图结构，再补充实际执行体。
    ///
    /// @warning 不得与当前 Graph 的执行并发调用。
    template <typename T>
        requires (basic_invocable<T> && capturable<T>)
    Task& work(T&& task);

    /// @brief 将当前静态图节点的执行体替换为单目标条件分支 callable。
    template <typename T>
        requires (branch_invocable<T> && capturable<T>)
    Task& work(T&& task);

    /// @brief 将当前静态图节点的执行体替换为多目标条件分支 callable。
    template <typename T>
        requires (multi_branch_invocable<T> && capturable<T>)
    Task& work(T&& task);

    /// @brief 将当前静态图节点的执行体替换为单目标 Jump callable。
    template <typename T>
        requires (jump_invocable<T> && capturable<T>)
    Task& work(T&& task);

    /// @brief 将当前静态图节点的执行体替换为多目标 MultiJump callable。
    template <typename T>
        requires (multi_jump_invocable<T> && capturable<T>)
    Task& work(T&& task);

    /// @brief 将当前静态图节点的执行体替换为 Runtime callable。
    template <typename T>
        requires (runtime_invocable<T> && capturable<T>)
    Task& work(T&& task);

    /// @brief 将当前静态图节点的执行体替换为 SubFlow callable。
    template <typename T>
        requires (subflow_invocable<T> && capturable<T>)
    Task& work(T&& task);

    /// @brief 将当前静态图节点替换为单次执行的 Module 节点。
    template <graph_holder Gh>
    Task& work(Gh&& gh);

    /// @brief 将当前静态图节点替换为最多执行 num 次的 Module 节点。
    template <graph_holder Gh>
    Task& work(Gh&& gh, std::uint64_t num);

    /// @brief 将当前静态图节点替换为由终止谓词控制的 Module 节点。
    template <graph_holder Gh, predicate P>
        requires capturable<P>
    Task& work(Gh&& gh, P&& pred);

    // ============================================================================
    // 执行对象原地重绑定
    // ============================================================================

    /// @brief 原地构造普通 callable，并返回内部对象引用。
    template <typename T, typename... Args>
        requires (std::same_as<T, std::decay_t<T>> && basic_invocable<T> && std::constructible_from<T, Args&&...>)
    [[nodiscard]] T& work_object(Args&&... args);

    /// @brief 原地构造单目标条件分支 callable，并返回内部对象引用。
    template <typename T, typename... Args>
        requires (std::same_as<T, std::decay_t<T>> && branch_invocable<T> && std::constructible_from<T, Args&&...>)
    [[nodiscard]] T& work_object(Args&&... args);

    /// @brief 原地构造多目标条件分支 callable，并返回内部对象引用。
    template <typename T, typename... Args>
        requires (std::same_as<T, std::decay_t<T>> && multi_branch_invocable<T> && std::constructible_from<T, Args&&...>)
    [[nodiscard]] T& work_object(Args&&... args);

    /// @brief 原地构造单目标 Jump callable，并返回内部对象引用。
    template <typename T, typename... Args>
        requires (std::same_as<T, std::decay_t<T>> && jump_invocable<T> && std::constructible_from<T, Args&&...>)
    [[nodiscard]] T& work_object(Args&&... args);

    /// @brief 原地构造 MultiJump callable，并返回内部对象引用。
    template <typename T, typename... Args>
        requires (std::same_as<T, std::decay_t<T>> && multi_jump_invocable<T> && std::constructible_from<T, Args&&...>)
    [[nodiscard]] T& work_object(Args&&... args);

    /// @brief 原地构造 Runtime callable，并返回内部对象引用。
    template <typename T, typename... Args>
        requires (std::same_as<T, std::decay_t<T>> && runtime_invocable<T> && std::constructible_from<T, Args&&...>)
    [[nodiscard]] T& work_object(Args&&... args);

    /// @brief 原地构造 SubFlow callable，并返回内部对象引用。
    template <typename T, typename... Args>
        requires (std::same_as<T, std::decay_t<T>> && subflow_invocable<T> && std::constructible_from<T, Args&&...>)
    [[nodiscard]] T& work_object(Args&&... args);

    /// @brief 原地默认构造子图持有者，并绑定为单次 Module。
    template <graph_holder Gh>
        requires (std::same_as<Gh, std::decay_t<Gh>> && std::constructible_from<Gh>)
    [[nodiscard]] Gh& work_object();

    /// @brief 原地默认构造子图持有者，并绑定为最多执行 num 次的 Module。
    template <graph_holder Gh>
        requires (std::same_as<Gh, std::decay_t<Gh>> && std::constructible_from<Gh>)
    [[nodiscard]] Gh& work_object(std::uint64_t num);

    /// @brief 原地默认构造子图持有者，并绑定终止谓词。
    template <graph_holder Gh, predicate P>
        requires (std::same_as<Gh, std::decay_t<Gh>> && capturable<P> && std::constructible_from<Gh>)
    [[nodiscard]] Gh& work_object(P&& pred);

    /// @brief 使用 tuple 参数原地构造子图持有者，并绑定为单次 Module。
    template <graph_holder Gh, typename Tuple>
        requires (std::same_as<Gh, std::decay_t<Gh>> && tuple_constructible_from<Gh, Tuple&&>)
    [[nodiscard]] Gh& work_object(Tuple&& args);

    /// @brief 使用 tuple 参数原地构造子图持有者，并绑定为最多执行 num 次的 Module。
    template <graph_holder Gh, typename Tuple>
        requires (std::same_as<Gh, std::decay_t<Gh>> && tuple_constructible_from<Gh, Tuple&&>)
    [[nodiscard]] Gh& work_object(Tuple&& args, std::uint64_t num);

    /// @brief 使用 tuple 参数原地构造子图持有者，并绑定终止谓词。
    template <graph_holder Gh, typename Tuple, predicate P>
        requires (std::same_as<Gh, std::decay_t<Gh>> && capturable<P> && tuple_constructible_from<Gh, Tuple&&>)
    [[nodiscard]] Gh& work_object(Tuple&& args, P&& pred);

    // ============================================================================
    // 拓扑构建
    // ============================================================================

    /// @brief 设置任务名称，直接写入 Work::m_name，用于调试和可视化。
    /// @tparam S 任何可用于构造 std::string 的类型。
    /// @param name 新的任务名称。
    /// @return *this。
    template <typename S>
        requires std::constructible_from<std::string, S>
    Task& name(S&& name);

    /// @brief 将当前任务设为 @p ts 中每个任务的前驱，建立 this -> each(t) 依赖。
    /// @tparam Check 默认检查拓扑；precede<false>() 跳过建边校验。
    /// @tparam Ts 每个参数必须为 Task 或其公有派生类型。
    /// @pre Check 为 false 时，调用方保证句柄有效、同属一图、边不重复且无非法闭环。
    /// @note 跳过检查仍可能分配内存并抛出异常。参数包逐边插入，失败不撤销此前成功的边。
    /// @param ts 一个或多个后继任务。
    /// @return *this。
    template <bool Check = true, typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::derived_from<std::remove_cvref_t<Ts>, Task> && ...)
    Task& precede(Ts&&... ts);

    /// @brief 将当前任务设为 @p ts 中每个任务的后继，建立 each(t) -> this 依赖。
    /// @tparam Check 默认检查拓扑；succeed<false>() 跳过建边校验。
    /// @tparam Ts 每个参数必须为 Task 或其公有派生类型。
    /// @pre Check 为 false 时，调用方保证句柄有效、同属一图、边不重复且无非法闭环。
    /// @note 跳过检查仍可能分配内存并抛出异常。参数包逐边插入，失败不撤销此前成功的边。
    /// @param ts 一个或多个前驱任务。
    /// @return *this。
    template <bool Check = true, typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::derived_from<std::remove_cvref_t<Ts>, Task> && ...)
    Task& succeed(Ts&&... ts);

    /// @brief 移除当前任务的一个或多个前驱关系，解除 each(task) -> this 依赖。
    /// @tparam Ts 每个参数必须为 Task 类型。
    /// @param tasks 需要解除的前驱任务。
    /// @return *this。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    Task& remove_predecessor(Ts&&... tasks) noexcept;

    /// @brief 移除当前任务的一个或多个后继关系，解除 this -> each(task) 依赖。
    /// @tparam Ts 每个参数必须为 Task 类型。
    /// @param tasks 需要解除的后继任务。
    /// @return *this。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    Task& remove_successor(Ts&&... tasks) noexcept;

    /// @brief 清空所有前驱关系，解除所有节点到当前任务的依赖。
    /// @return *this。
    Task& clear_predecessors() noexcept;

    /// @brief 清空所有后继关系，解除当前任务到所有节点的依赖。
    /// @return *this。
    Task& clear_successors() noexcept;

    // ============================================================================
    // 信号量管理
    // ============================================================================

    /// @brief 声明任务执行前需要获取的一个或多个信号量，每个默认获取 1 个配额。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
    Task& acquire(Ts&... semaphores);

    /// @brief 声明任务执行前需要从指定信号量获取的配额。
    /// @param semaphore 非拥有引用；必须存活到任务执行完毕。
    /// @param count 配额数；0 表示不添加约束。
    /// @throws Exception 同一信号量已存在于 acquire 列表时抛出。
    Task& acquire(Semaphore& semaphore, std::size_t count);

    /// @brief 声明任务执行后需要释放的一个或多个信号量，每个默认释放 1 个配额。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
    Task& release(Ts&... semaphores);

    /// @brief 声明任务执行后需要向指定信号量释放的配额。
    /// @param semaphore 非拥有引用；必须存活到任务执行完毕。
    /// @param count 配额数；0 表示不添加约束。
    /// @throws Exception 同一信号量已存在于 release 列表时抛出。
    Task& release(Semaphore& semaphore, std::size_t count);

    /// @brief 移除一个或多个执行前信号量获取约束。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
    Task& remove_acquire(Ts&... semaphores) noexcept;

    /// @brief 移除一个或多个执行后信号量释放约束。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
    Task& remove_release(Ts&... semaphores) noexcept;

    /// @brief 清空所有执行前信号量获取约束。
    /// @return *this。
    Task& clear_acquires() noexcept;

    /// @brief 清空所有执行后信号量释放约束。
    /// @return *this。
    Task& clear_releases() noexcept;

    // ============================================================================
    // 迭代访问
    // ============================================================================

    /// @brief 遍历当前任务的所有前驱任务，对每个调用 visitor(Task{predecessor})。
    /// @tparam F 可调用对象，接收 Task 句柄。
    /// @param visitor 访问器，非 const 重载允许修改前驱节点的属性。
    template <typename F>
        requires std::invocable<F&, Task>
    void for_each_predecessor(F&& visitor) noexcept(std::is_nothrow_invocable_v<F&, Task>);

    /// @brief const 重载 —— 遍历时提供 `TaskView` 只读句柄，禁止修改前驱节点。
    template <typename F>
        requires std::invocable<F&, TaskView>
    void for_each_predecessor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F&, TaskView>);

    /// @brief 遍历当前任务的所有后继任务，对每个调用 visitor(Task{successor})。
    /// @tparam F 可调用对象，接收 Task 句柄。
    /// @param visitor 访问器，非 const 重载允许修改后继节点的属性。
    template <typename F>
        requires std::invocable<F&, Task>
    void for_each_successor(F&& visitor) noexcept(std::is_nothrow_invocable_v<F&, Task>);

    /// @brief const 重载 —— 遍历时提供 `TaskView` 只读句柄，禁止修改后继节点。
    template <typename F>
        requires std::invocable<F&, TaskView>
    void for_each_successor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F&, TaskView>);

    /// @brief 遍历任务执行前的信号量获取约束，对每个调用 visitor(Semaphore&, std::size_t&) 或 visitor(Semaphore&)。
    /// @tparam F 可调用对象，可接收 (Semaphore&, std::size_t&) 或仅 (Semaphore&)。
    /// @param visitor 访问器，可变重载允许修改信号量及配额。
    template <typename F>
        requires std::invocable<F&, Semaphore&, std::size_t&>
                 || std::invocable<F&, Semaphore&>
    void for_each_acquire(F&& visitor) noexcept(
        std::invocable<F&, Semaphore&, std::size_t&>
            ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
            : std::is_nothrow_invocable_v<F&, Semaphore&>);

    /// @brief const 重载 —— 遍历时提供只读信号量与不可变配额。
    template <typename F>
        requires std::invocable<F&, const Semaphore&, std::size_t>
                 || std::invocable<F&, const Semaphore&>
    void for_each_acquire(F&& visitor) const noexcept(
        std::invocable<F&, const Semaphore&, std::size_t>
            ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
            : std::is_nothrow_invocable_v<F&, const Semaphore&>);

    /// @brief 遍历任务执行后的信号量释放约束，对每个调用 visitor(Semaphore&, std::size_t&) 或 visitor(Semaphore&)。
    /// @tparam F 可调用对象，可接收 (Semaphore&, std::size_t&) 或仅 (Semaphore&)。
    /// @param visitor 访问器，可变重载允许修改信号量及配额。
    template <typename F>
        requires std::invocable<F&, Semaphore&, std::size_t&>
                 || std::invocable<F&, Semaphore&>
    void for_each_release(F&& visitor) noexcept(
        std::invocable<F&, Semaphore&, std::size_t&>
            ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
            : std::is_nothrow_invocable_v<F&, Semaphore&>);

    /// @brief const 重载 —— 遍历时提供只读信号量与不可变配额。
    template <typename F>
        requires std::invocable<F&, const Semaphore&, std::size_t>
                 || std::invocable<F&, const Semaphore&>
    void for_each_release(F&& visitor) const noexcept(
        std::invocable<F&, const Semaphore&, std::size_t>
            ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
            : std::is_nothrow_invocable_v<F&, const Semaphore&>);


    // ============================================================================
    // 观察者管理
    // ============================================================================

    /// @brief 注册任务观察者，在任务执行前后接收回调。
    /// @tparam Observer TaskObserver 的派生类型。
    /// @param args 构造 Observer 所需参数。
    /// @return 已注册观察者的 shared_ptr，可用于后续注销。
    template <std::derived_from<TaskObserver> Observer, typename... Args>
        requires std::constructible_from<Observer, Args&&...>
    [[nodiscard]] std::shared_ptr<Observer> register_observer(Args&&... args);

    /// @brief 注销指定任务观察者。
    /// @param observer `register_observer` 返回的观察者指针。
    template <std::derived_from<TaskObserver> Observer>
    void unregister_observer(const std::shared_ptr<Observer>& observer) noexcept;
private:
    Work* m_work{nullptr};  ///< 底层 Work 节点指针，非拥有引用。

    /// @brief 从底层 Work 指针构造任务句柄。
    explicit Task(Work* work) noexcept;

    template <typename Invoker, typename... Args>
    Invoker& _replace_work(Args&&... args);
};

// ============================================================================
// Task 实现
// ============================================================================
// ============================================================================
// 执行体重绑定
// ============================================================================

template <typename Invoker, typename... Args>
inline Invoker& Task::_replace_work(Args&&... args) {
    TFL_ASSERT(m_work);
    TFL_ASSERT(m_work->m_graph && "Task::work only supports static Graph nodes");
    return m_work->template emplace<Invoker>(std::forward<Args>(args)...);
}

// ============================================================================
// Basic
// ============================================================================

template <typename T>
    requires (basic_invocable<T> && capturable<T>)
inline Task& Task::work(T&& task) {
    using Invoker = BasicInvoker<std::decay_t<T>>;
    _replace_work<Invoker>(std::forward<T>(task));
    return *this;
}


// ============================================================================
// Branch
// ============================================================================

template <typename T>
    requires (branch_invocable<T> && capturable<T>)
inline Task& Task::work(T&& task) {
    using Invoker = BranchInvoker<std::decay_t<T>>;
    _replace_work<Invoker>(std::forward<T>(task));
    return *this;
}


// ============================================================================
// MultiBranch
// ============================================================================

template <typename T>
    requires (multi_branch_invocable<T> && capturable<T>)
inline Task& Task::work(T&& task) {
    using Invoker = MultiBranchInvoker<std::decay_t<T>>;
    _replace_work<Invoker>(std::forward<T>(task));
    return *this;
}


// ============================================================================
// Jump
// ============================================================================

template <typename T>
    requires (jump_invocable<T> && capturable<T>)
inline Task& Task::work(T&& task) {
    using Invoker = JumpInvoker<std::decay_t<T>>;
    _replace_work<Invoker>(std::forward<T>(task));
    return *this;
}


// ============================================================================
// MultiJump
// ============================================================================

template <typename T>
    requires (multi_jump_invocable<T> && capturable<T>)
inline Task& Task::work(T&& task) {
    using Invoker = MultiJumpInvoker<std::decay_t<T>>;
    _replace_work<Invoker>(std::forward<T>(task));
    return *this;
}


// ============================================================================
// Runtime
// ============================================================================

template <typename T>
    requires (runtime_invocable<T> && capturable<T>)
inline Task& Task::work(T&& task) {
    using Invoker = RuntimeInvoker<std::decay_t<T>>;
    _replace_work<Invoker>(std::forward<T>(task));
    return *this;
}


// ============================================================================
// SubFlow
// ============================================================================

template <typename T>
    requires (subflow_invocable<T> && capturable<T>)
inline Task& Task::work(T&& task) {
    using Invoker = SubFlowInvoker<std::decay_t<T>>;
    _replace_work<Invoker>(std::forward<T>(task));
    return *this;
}


// ============================================================================
// Module
// ============================================================================

template <graph_holder Gh>
inline Task& Task::work(Gh&& gh) {
    return work(std::forward<Gh>(gh), std::uint64_t{1});
}

template <graph_holder Gh>
inline Task& Task::work(Gh&& gh, std::uint64_t num) {
    auto pred = [num, remaining = num]() mutable noexcept -> bool {
        if (remaining == 0) {
            remaining = num;
            return true;
        }

        --remaining;
        return false;
    };

    using Invoker = ModuleInvoker<detail::captured_t<Gh>, decltype(pred)>;
    _replace_work<Invoker>(detail::capture(std::forward<Gh>(gh)), std::move(pred));
    return *this;
}

template <graph_holder Gh, predicate P>
    requires capturable<P>
inline Task& Task::work(Gh&& gh, P&& pred) {
    using Invoker = ModuleInvoker<detail::captured_t<Gh>, std::decay_t<P>>;
    _replace_work<Invoker>(detail::capture(std::forward<Gh>(gh)), std::forward<P>(pred));
    return *this;
}

// ============================================================================
// Basic Object
// ============================================================================

template <typename T, typename... Args>
    requires (std::same_as<T, std::decay_t<T>> && basic_invocable<T> && std::constructible_from<T, Args&&...>)
inline T& Task::work_object(Args&&... args) {
    using Invoker = BasicInvoker<T>;
    return _replace_work<Invoker>(std::in_place, std::forward<Args>(args)...).object();
}


// ============================================================================
// Branch Object
// ============================================================================

template <typename T, typename... Args>
    requires (std::same_as<T, std::decay_t<T>> && branch_invocable<T> && std::constructible_from<T, Args&&...>)
inline T& Task::work_object(Args&&... args) {
    using Invoker = BranchInvoker<T>;
    return _replace_work<Invoker>(std::in_place, std::forward<Args>(args)...).object();
}


// ============================================================================
// MultiBranch Object
// ============================================================================

template <typename T, typename... Args>
    requires (std::same_as<T, std::decay_t<T>> && multi_branch_invocable<T> && std::constructible_from<T, Args&&...>)
inline T& Task::work_object(Args&&... args) {
    using Invoker = MultiBranchInvoker<T>;
    return _replace_work<Invoker>(std::in_place, std::forward<Args>(args)...).object();
}


// ============================================================================
// Jump Object
// ============================================================================

template <typename T, typename... Args>
    requires (std::same_as<T, std::decay_t<T>> && jump_invocable<T> && std::constructible_from<T, Args&&...>)
inline T& Task::work_object(Args&&... args) {
    using Invoker = JumpInvoker<T>;
    return _replace_work<Invoker>(std::in_place, std::forward<Args>(args)...).object();
}


// ============================================================================
// MultiJump Object
// ============================================================================

template <typename T, typename... Args>
    requires (std::same_as<T, std::decay_t<T>> && multi_jump_invocable<T> && std::constructible_from<T, Args&&...>)
inline T& Task::work_object(Args&&... args) {
    using Invoker = MultiJumpInvoker<T>;
    return _replace_work<Invoker>(std::in_place, std::forward<Args>(args)...).object();
}


// ============================================================================
// Runtime Object
// ============================================================================

template <typename T, typename... Args>
    requires (std::same_as<T, std::decay_t<T>> && runtime_invocable<T> && std::constructible_from<T, Args&&...>)
inline T& Task::work_object(Args&&... args) {
    using Invoker = RuntimeInvoker<T>;
    return _replace_work<Invoker>(std::in_place, std::forward<Args>(args)...).object();
}


// ============================================================================
// SubFlow Object
// ============================================================================

template <typename T, typename... Args>
    requires (std::same_as<T, std::decay_t<T>> && subflow_invocable<T> && std::constructible_from<T, Args&&...>)
inline T& Task::work_object(Args&&... args) {
    using Invoker = SubFlowInvoker<T>;
    return _replace_work<Invoker>(std::in_place, std::forward<Args>(args)...).object();
}


// ============================================================================
// Module Object
// ============================================================================

template <graph_holder Gh>
    requires (std::same_as<Gh, std::decay_t<Gh>> && std::constructible_from<Gh>)
inline Gh& Task::work_object() {
    return work_object<Gh>(std::tuple{});
}

template <graph_holder Gh>
    requires (std::same_as<Gh, std::decay_t<Gh>> && std::constructible_from<Gh>)
inline Gh& Task::work_object(std::uint64_t num) {
    return work_object<Gh>(std::tuple{}, num);
}

template <graph_holder Gh, predicate P>
    requires (std::same_as<Gh, std::decay_t<Gh>> && capturable<P> && std::constructible_from<Gh>)
inline Gh& Task::work_object(P&& pred) {
    return work_object<Gh>(std::tuple{}, std::forward<P>(pred));
}

template <graph_holder Gh, typename Tuple>
    requires (std::same_as<Gh, std::decay_t<Gh>> && tuple_constructible_from<Gh, Tuple&&>)
inline Gh& Task::work_object(Tuple&& args) {
    return work_object<Gh>(std::forward<Tuple>(args), std::uint64_t{1});
}

template <graph_holder Gh, typename Tuple>
    requires (std::same_as<Gh, std::decay_t<Gh>> && tuple_constructible_from<Gh, Tuple&&>)
inline Gh& Task::work_object(Tuple&& args, std::uint64_t num) {
    auto pred = [num, remaining = num]() mutable noexcept -> bool {
        if (remaining == 0) {
            remaining = num;
            return true;
        }

        --remaining;
        return false;
    };

    return work_object<Gh>(std::forward<Tuple>(args), std::move(pred));
}

template <graph_holder Gh, typename Tuple, predicate P>
    requires (std::same_as<Gh, std::decay_t<Gh>> && capturable<P> && tuple_constructible_from<Gh, Tuple&&>)
inline Gh& Task::work_object(Tuple&& args, P&& pred) {
    return std::apply([this, &pred](auto&&... values) -> Gh& {
        using Invoker = ModuleInvoker<Gh, std::decay_t<P>>;
        return _replace_work<Invoker>(
                   std::in_place,
                   std::forward<P>(pred),
                   std::forward<decltype(values)>(values)...
                   ).object();
    }, std::forward<Tuple>(args));
}

inline Task::Task(Work* work) noexcept : m_work{work} {}
inline Task::Task(std::nullptr_t) noexcept : m_work{nullptr} {}
inline Task::Task(const Task& rhs) noexcept : m_work{rhs.m_work} {}

inline Task& Task::operator=(const Task& rhs) noexcept {
    m_work = rhs.m_work;
    return *this;
}

inline Task::Task(Task&& rhs) noexcept : m_work{std::exchange(rhs.m_work, nullptr)} {}

inline Task& Task::operator=(Task&& rhs) noexcept {
    if (this != &rhs) {
        m_work = std::exchange(rhs.m_work, nullptr);
    }
    return *this;
}

inline Task& Task::operator=(std::nullptr_t) noexcept {
    m_work = nullptr;
    return *this;
}

inline bool Task::operator==(const Task& rhs) const noexcept {
    return m_work == rhs.m_work;
}
inline bool Task::operator!=(const Task& rhs) const noexcept {
    return m_work != rhs.m_work;
}
inline void Task::reset() noexcept {
    m_work = nullptr;
}

inline std::size_t Task::hash_value() const noexcept {
    return std::hash<const Work*>{}(m_work);
}

inline std::string_view Task::name() const noexcept {
    return m_work ? m_work->_name() : std::string_view{};
}

inline bool Task::valid() const noexcept {
    return m_work != nullptr;
}

inline std::size_t Task::num_successors() const noexcept {
    return m_work ? m_work->m_num_successors : 0;
}

inline std::size_t Task::num_predecessors() const noexcept {
    return m_work ? m_work->_num_predecessors() : 0;
}

inline std::size_t Task::num_acquires() const noexcept {
    return m_work ? m_work->_num_acquires() : 0;
}

inline std::size_t Task::num_releases() const noexcept {
    return m_work ? m_work->_num_releases() : 0;
}

inline std::size_t Task::num_observers() const noexcept {
    return m_work ? m_work->_num_observers() : 0;
}

inline Task::operator bool() const noexcept {
    return m_work != nullptr;
}

inline TaskType Task::type() const noexcept {
    return m_work ? m_work->type() : TaskType::None;
}

inline bool Task::has_exception_ptr() const noexcept {
    return m_work && m_work->m_exception_ptr;
}

inline std::exception_ptr Task::exception_ptr() const noexcept {
    return m_work ? m_work->m_exception_ptr : std::exception_ptr{};
}

inline std::string Task::dump(Direction dir) const {
    std::ostringstream oss;
    dump(oss, dir);
    return std::move(oss).str();
}

inline void Task::dump(std::ostream& os, Direction dir) const {
    if (!m_work) {
        return;
    }
    os << "direction: " << to_string(dir) << "\n\n";
    m_work->dump(os);
    os << "\n";
}

template <typename S>
    requires std::constructible_from<std::string, S>
inline Task& Task::name(S&& value) {
    TFL_ASSERT(m_work);
    m_work->_set_name(std::forward<S>(value));
    return *this;
}

template <bool Check, typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::derived_from<std::remove_cvref_t<Ts>, Task> && ...)
inline Task& Task::precede(Ts&&... tasks) {
    TFL_ASSERT(m_work);
    TFL_ASSERT((tasks.m_work && ...));
    (m_work->template _precede<Check>(tasks.m_work), ...);
    return *this;
}

template <bool Check, typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::derived_from<std::remove_cvref_t<Ts>, Task> && ...)
inline Task& Task::succeed(Ts&&... tasks) {
    TFL_ASSERT(m_work);
    TFL_ASSERT((tasks.m_work && ...));
    (tasks.m_work->template _precede<Check>(m_work), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
inline Task& Task::remove_predecessor(Ts&&... tasks) noexcept {
    TFL_ASSERT(m_work);
    TFL_ASSERT((tasks.m_work && ...));
    (tasks.m_work->_remove_successor(m_work), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
inline Task& Task::remove_successor(Ts&&... tasks) noexcept {
    TFL_ASSERT(m_work);
    TFL_ASSERT((tasks.m_work && ...));
    (m_work->_remove_successor(tasks.m_work), ...);
    return *this;
}

inline Task& Task::clear_predecessors() noexcept {
    TFL_ASSERT(m_work);
    m_work->_clear_predecessors();
    return *this;
}

inline Task& Task::clear_successors() noexcept {
    TFL_ASSERT(m_work);
    m_work->_clear_successors();
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
inline Task& Task::acquire(Ts&... semaphores) {
    TFL_ASSERT(m_work);
    (m_work->_acquire(std::addressof(semaphores), 1), ...);
    return *this;
}

inline Task& Task::acquire(Semaphore& semaphore, std::size_t count) {
    TFL_ASSERT(m_work);
    m_work->_acquire(std::addressof(semaphore), count);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
inline Task& Task::release(Ts&... semaphores) {
    TFL_ASSERT(m_work);
    (m_work->_release(std::addressof(semaphores), 1), ...);
    return *this;
}

inline Task& Task::release(Semaphore& semaphore, std::size_t count) {
    TFL_ASSERT(m_work);
    m_work->_release(std::addressof(semaphore), count);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
inline Task& Task::remove_acquire(Ts&... semaphores) noexcept {
    TFL_ASSERT(m_work);
    (m_work->_remove_acquire(std::addressof(semaphores)), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
inline Task& Task::remove_release(Ts&... semaphores) noexcept {
    TFL_ASSERT(m_work);
    (m_work->_remove_release(std::addressof(semaphores)), ...);
    return *this;
}

inline Task& Task::clear_acquires() noexcept {
    TFL_ASSERT(m_work);
    m_work->_clear_acquires();
    return *this;
}

inline Task& Task::clear_releases() noexcept {
    TFL_ASSERT(m_work);
    m_work->_clear_releases();
    return *this;
}

template <typename F>
    requires std::invocable<F&, Task>
inline void Task::for_each_predecessor(F&& visitor) noexcept(std::is_nothrow_invocable_v<F&, Task>) {
    if (!m_work) {
        return;
    }
    for (Work* pred : m_work->_predecessors()) {
        std::invoke(visitor, Task{pred});
    }
}

template <typename F>
    requires std::invocable<F&, TaskView>
inline void Task::for_each_predecessor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F&, TaskView>) {
    if (!m_work) {
        return;
    }
    for (const Work* pred : m_work->_predecessors()) {
        std::invoke(visitor, TaskView{*pred});
    }
}

template <typename F>
    requires std::invocable<F&, Task>
inline void Task::for_each_successor(F&& visitor) noexcept(std::is_nothrow_invocable_v<F&, Task>) {
    if (!m_work) {
        return;
    }
    for (Work* succ : m_work->_successors()) {
        std::invoke(visitor, Task{succ});
    }
}

template <typename F>
    requires std::invocable<F&, TaskView>
inline void Task::for_each_successor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F&, TaskView>) {
    if (!m_work) {
        return;
    }
    for (const Work* succ : m_work->_successors()) {
        std::invoke(visitor, TaskView{*succ});
    }
}

template <typename F>
    requires std::invocable<F&, Semaphore&, std::size_t&>
             || std::invocable<F&, Semaphore&>
inline void Task::for_each_acquire(F&& visitor) noexcept(
    std::invocable<F&, Semaphore&, std::size_t&>
        ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
        : std::is_nothrow_invocable_v<F&, Semaphore&>) {
    if (!m_work) {
        return;
    }
    for (auto& req : m_work->_acquires()) {
        if constexpr (std::invocable<F&, Semaphore&, std::size_t&>) {
            std::invoke(visitor, *req.sem, req.count);
        } else {
            std::invoke(visitor, *req.sem);
        }
    }
}

template <typename F>
    requires std::invocable<F&, const Semaphore&, std::size_t>
             || std::invocable<F&, const Semaphore&>
inline void Task::for_each_acquire(F&& visitor) const noexcept(
    std::invocable<F&, const Semaphore&, std::size_t>
        ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
        : std::is_nothrow_invocable_v<F&, const Semaphore&>) {
    if (!m_work) {
        return;
    }
    for (const auto& req : m_work->_acquires()) {
        if constexpr (std::invocable<F&, const Semaphore&, std::size_t>) {
            std::invoke(visitor, *req.sem, req.count);
        } else {
            std::invoke(visitor, *req.sem);
        }
    }
}

template <typename F>
    requires std::invocable<F&, Semaphore&, std::size_t&>
             || std::invocable<F&, Semaphore&>
inline void Task::for_each_release(F&& visitor) noexcept(
    std::invocable<F&, Semaphore&, std::size_t&>
        ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
        : std::is_nothrow_invocable_v<F&, Semaphore&>) {
    if (!m_work) {
        return;
    }
    for (auto& req : m_work->_releases()) {
        if constexpr (std::invocable<F&, Semaphore&, std::size_t&>) {
            std::invoke(visitor, *req.sem, req.count);
        } else {
            std::invoke(visitor, *req.sem);
        }
    }
}

template <typename F>
    requires std::invocable<F&, const Semaphore&, std::size_t>
             || std::invocable<F&, const Semaphore&>
inline void Task::for_each_release(F&& visitor) const noexcept(
    std::invocable<F&, const Semaphore&, std::size_t>
        ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
        : std::is_nothrow_invocable_v<F&, const Semaphore&>) {
    if (!m_work) {
        return;
    }
    for (const auto& req : m_work->_releases()) {
        if constexpr (std::invocable<F&, const Semaphore&, std::size_t>) {
            std::invoke(visitor, *req.sem, req.count);
        } else {
            std::invoke(visitor, *req.sem);
        }
    }
}

template <std::derived_from<TaskObserver> Observer, typename... Args>
    requires std::constructible_from<Observer, Args&&...>
inline std::shared_ptr<Observer> Task::register_observer(Args&&... args) {
    TFL_ASSERT(m_work);
    auto ptr = std::make_shared<Observer>(std::forward<Args>(args)...);
    if (!m_work->m_observers) {
        m_work->m_observers = std::make_unique<Work::ObserverData>();
    }
    m_work->m_observers->observers.emplace_back(std::static_pointer_cast<TaskObserver>(ptr));
    return ptr;
}
template <std::derived_from<TaskObserver> Observer>
inline void Task::unregister_observer(const std::shared_ptr<Observer>& observer) noexcept {
    if (!m_work || !observer || !m_work->m_observers) {
        return;
    }
    auto base = std::static_pointer_cast<TaskObserver>(observer);
    auto& observers = m_work->m_observers->observers;
    for (auto it = observers.begin(); it != observers.end(); ++it) {
        if (*it == base) {
            observers.erase(it);
            break;
        }
    }
    if (m_work->m_observers->empty()) {
        m_work->m_observers.reset();
    }
}

/// @brief 拓扑连线运算符：`a >> b` 等价于 `b.succeed(a)`，建立 a → b 依赖。
template <typename L>
    requires std::same_as<std::remove_cvref_t<L>, Task>
inline Task& operator>>(L&& lhs, Task& rhs) {
    rhs.succeed(std::forward<L>(lhs));
    return rhs;
}

/// @brief 右操作数为右值的重载，建立 lhs → rhs 依赖并返回 rhs。
template <typename L>
    requires std::same_as<std::remove_cvref_t<L>, Task>
inline Task operator>>(L&& lhs, Task&& rhs) {
    rhs.succeed(std::forward<L>(lhs));
    return std::move(rhs);
}

/// @brief 拓扑反向连线运算符：`a << b` 等价于 `b.precede(a)`，建立 b → a 依赖。
template <typename L>
    requires std::same_as<std::remove_cvref_t<L>, Task>
inline Task& operator<<(L&& lhs, Task& rhs) {
    rhs.precede(std::forward<L>(lhs));
    return rhs;
}

/// @brief 右操作数为右值的重载，建立 rhs → lhs 依赖并返回 rhs。
template <typename L>
    requires std::same_as<std::remove_cvref_t<L>, Task>
inline Task operator<<(L&& lhs, Task&& rhs) {
    rhs.precede(std::forward<L>(lhs));
    return std::move(rhs);
}


/// @brief 将 `Task` 的 D2 描述写入输出流。
inline std::ostream& operator << (std::ostream& os, const Task& task) {
    task.dump(os);
    return os;
}

/// @brief 将 `TaskView` 的 D2 描述写入输出流。
inline std::ostream& operator << (std::ostream& os, const TaskView& task) {
    task.dump(os);
    return os;
}

} // namespace tfl


namespace std {
/// @brief 按底层 `Work` 指针身份计算 `tfl::Task` 的标准哈希值。
///
/// 指向同一节点的句柄具有相同哈希；节点销毁后不得继续使用该句柄作为键。
template <>
struct hash<tfl::Task> {
    inline auto operator()(const tfl::Task& t) const noexcept { return t.hash_value(); }
};

/// @brief 按底层 `Work` 引用身份计算 `tfl::TaskView` 的标准哈希值。
///
/// 指向同一节点的视图具有相同哈希；节点销毁后不得继续使用该视图作为键。
template <>
struct hash<tfl::TaskView> {
    inline auto operator()(const tfl::TaskView& tv) const noexcept { return tv.hash_value(); }
};
} // namespace std
