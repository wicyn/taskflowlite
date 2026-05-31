/// @file task.hpp
/// @brief 任务句柄 Task 与只读视图 TaskView
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once
#include <sstream>

#include "work.hpp"
#include "semaphore.hpp"
#include "enums.hpp"

namespace tfl {

/// @brief DAG 节点的轻量级用户层句柄 —— 裸 Work* 上的薄包装。
///
/// 单指针开销，提供 fluent API 操作依赖（precede/succeed）、信号量（acquire/release）、
/// 观察者等节点属性。弱引用语义 —— 不参与生命周期管理，Flow 析构后句柄即刻悬挂。
/// lvalue 上返回 Task& 零拷贝，rvalue 上按值返回堵住悬空引用陷阱。
///
/// precede/succeed 在 Work::m_edges 上双向同步更新前驱与后继，保证 join_counter 正确。
/// hash 基于 Work* 地址，特化 std::hash 支持直接用作 unordered_map 键。
///
/// @note 框架内有多种指向 Work 的句柄（Task/TaskView/AsyncTask），语义截然不同；
/// 需要调度期持有引用应使用强引用的 AsyncTask，而非弱引用的 Task。
///
// ============================================================================
// TaskView - 只读视图
// ============================================================================
/// @brief Task 的只读视图 —— 编译期禁写，专为 TaskObserver 回调场景设计。
///
/// 持有 `const Work&`，将线程安全契约从运行期断言提升到类型系统：
/// 调度回调中绝不允许篡改节点状态，编译错误即是契约。
/// 所有访问器与 Task 同名只读方法一一对应，hash/equality 基于 Work* 地址。
///
/// @note 不可默认构造（持引用），必须由框架内部构造后传出。
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

    /// @brief 获取当前视图的哈希值，基于底层 Work 地址。
    [[nodiscard]] std::size_t hash_value() const noexcept;

    /// @brief 获取任务名称（调试/可视化用），直接读取底层 Work::m_name。
    /// @return 任务名称 view，空字符串表示未命名。
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

    /// @brief 获取底层任务节点类型，直接读取 Work::m_type 枚举。
    /// @return TaskType 枚举值（Basic, Branch, Runtime, Graph 等）。
    [[nodiscard]] TaskType type() const noexcept;

    /// @brief 检测任务执行期间是否已记录异常（读取 Work::m_exception_ptr）。
    /// @return 有异常指针时返回 true。
    [[nodiscard]] bool has_exception() const noexcept;

    /// @brief 获取任务执行期间捕获的异常指针，直接返回 Work::m_exception_ptr。
    /// @return std::exception_ptr，可能为空。
    [[nodiscard]] std::exception_ptr exception() const noexcept;

    /// @brief 将当前任务节点导出为 D2 描述字符串。
    [[nodiscard]] std::string dump(Direction dir = Direction::Default) const;

    /// @brief 将当前任务节点的 D2 描述写入输出流。
    void dump(std::ostream& ostream, Direction dir = Direction::Default) const;

    /// @brief 遍历当前任务的所有前驱只读节点，对每个调用 visitor(TaskView{*predecessor})。
    /// @tparam F 可调用对象，接收 TaskView 只读句柄。
    /// @param visitor 访问器，禁止修改前驱节点属性（TaskView 不可写）。
    template <std::invocable<TaskView> F>
    void for_each_predecessor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F, TaskView>);

    /// @brief 遍历当前任务的所有后继只读节点，对每个调用 visitor(TaskView{*successor})。
    /// @tparam F 可调用对象，接收 TaskView 只读句柄。
    /// @param visitor 访问器，禁止修改后继节点属性。
    template <std::invocable<TaskView> F>
    void for_each_successor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F, TaskView>);

    /// @brief 遍历任务执行前的信号量获取约束，对每个调用 visitor(const Semaphore&, std::size_t) 或 visitor(const Semaphore&)。
    /// @tparam F 可调用对象，可接收 (const Semaphore&, std::size_t) 或仅 (const Semaphore&)。
    /// @param visitor 访问器，信号量与配额均为只读。
    template <typename F>
        requires std::invocable<F, const Semaphore&, std::size_t> || std::invocable<F, const Semaphore&>
    void for_each_acquire(F&& visitor) const noexcept(
        std::invocable<F, const Semaphore&, std::size_t>
            ? std::is_nothrow_invocable_v<F, const Semaphore&, std::size_t>
            : std::is_nothrow_invocable_v<F, const Semaphore&>
        );

    /// @brief 遍历任务执行后的信号量释放约束，对每个调用 visitor(const Semaphore&, std::size_t) 或 visitor(const Semaphore&)。
    /// @tparam F 可调用对象，可接收 (const Semaphore&, std::size_t) 或仅 (const Semaphore&)。
    /// @param visitor 访问器，信号量与配额均为只读。
    template <typename F>
        requires std::invocable<F, const Semaphore&, std::size_t> || std::invocable<F, const Semaphore&>
    void for_each_release(F&& visitor) const noexcept(
        std::invocable<F, const Semaphore&, std::size_t>
            ? std::is_nothrow_invocable_v<F, const Semaphore&, std::size_t>
            : std::is_nothrow_invocable_v<F, const Semaphore&>
        );

private:
    /// @brief 从底层 Work 引用构造只读任务视图。
    explicit TaskView(const Work& work) noexcept : m_work{work} {}

    const Work& m_work;  ///< 底层 Work 节点只读引用，非拥有。
};

inline bool TaskView::operator==(const TaskView& rhs) const noexcept {
    return &m_work == &rhs.m_work;
}

inline bool TaskView::operator!=(const TaskView& rhs) const noexcept {
    return &m_work != &rhs.m_work;
}

inline std::size_t TaskView::hash_value() const noexcept {
    return std::hash<const Work*>{}(&m_work);
}

inline std::string_view TaskView::name() const noexcept {
    return m_work.m_name;
}

inline std::size_t TaskView::num_predecessors() const noexcept {
    return m_work._num_predecessors();
}

inline std::size_t TaskView::num_successors() const noexcept {
    return m_work.m_num_successors;
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
    return m_work.m_type;
}

inline bool TaskView::has_exception() const noexcept {
    return m_work._has_exception();
}

inline std::exception_ptr TaskView::exception() const noexcept {
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

template <std::invocable<TaskView> F>
inline void TaskView::for_each_predecessor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F, TaskView>) {
    for (const Work* pred : m_work._predecessors()) {
        std::invoke(std::forward<F>(visitor), TaskView{*pred});
    }
}

template <std::invocable<TaskView> F>
inline void TaskView::for_each_successor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F, TaskView>) {
    for (const Work* succ : m_work._successors()) {
        std::invoke(std::forward<F>(visitor), TaskView{*succ});
    }
}

template <typename F>
    requires std::invocable<F, const Semaphore&, std::size_t> || std::invocable<F, const Semaphore&>
inline void TaskView::for_each_acquire(F&& visitor) const noexcept(
    std::invocable<F, const Semaphore&, std::size_t>
        ? std::is_nothrow_invocable_v<F, const Semaphore&, std::size_t>
        : std::is_nothrow_invocable_v<F, const Semaphore&>
    ) {
    for (const auto& req : m_work._acquires()) {
        if constexpr (std::invocable<F, const Semaphore&, std::size_t>) {
            std::invoke(visitor, *req.sem, req.count);
        } else {
            std::invoke(visitor, *req.sem);
        }
    }
}

template <typename F>
    requires std::invocable<F, const Semaphore&, std::size_t> || std::invocable<F, const Semaphore&>
inline void TaskView::for_each_release(F&& visitor) const noexcept(
    std::invocable<F, const Semaphore&, std::size_t>
        ? std::is_nothrow_invocable_v<F, const Semaphore&, std::size_t>
        : std::is_nothrow_invocable_v<F, const Semaphore&>
    ) {
    for (const auto& req : m_work._releases()) {
        if constexpr (std::invocable<F, const Semaphore&, std::size_t>) {
            std::invoke(visitor, *req.sem, req.count);
        } else {
            std::invoke(visitor, *req.sem);
        }
    }
}



class Task {
    friend class Flow;
    friend class Runtime;
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

    // ========================================================================
    //  状态查询
    // ========================================================================

    /// @brief 返回句柄的哈希值，基于底层 Work 指针地址，支持 unordered_map 直接使用。
    /// @return std::hash<const Work*> 结果。
    [[nodiscard]] std::size_t hash_value() const noexcept;

    /// @brief 获取任务名称（调试/可视化用），直接读取底层 Work::m_name。
    /// @return 任务名称 view，可能为空字符串。
    [[nodiscard]] std::string_view name() const noexcept;

    /// @brief 判断当前句柄是否绑定了有效任务节点（m_work != nullptr）。
    /// @return 绑定有效节点返回 true，默认构造/置空/悬空返回 false。
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

    /// @brief 检测任务执行期间是否已记录异常（读取 Work::m_exception_ptr）。
    /// @return 有异常指针时返回 true。
    [[nodiscard]] bool has_exception() const noexcept;

    /// @brief 获取底层任务节点类型，直接读取 Work::m_type 枚举。
    /// @return TaskType 枚举值（Basic, Branch, Runtime, Graph 等）。
    [[nodiscard]] TaskType type() const noexcept;

    /// @brief 获取任务执行期间捕获的异常指针，直接返回 Work::m_exception_ptr。
    /// @return std::exception_ptr，可能为空。
    std::exception_ptr exception() const noexcept;

    /// @brief 将当前任务节点导出为 D2 描述字符串。
    [[nodiscard]] std::string dump(Direction dir = Direction::Default) const;

    /// @brief 将当前任务节点的 D2 描述写入输出流。
    void dump(std::ostream& ostream, Direction dir = Direction::Default) const;

    // ========================================================================
    //  拓扑构建
    // ========================================================================

    /// @brief 设置任务名称，直接写入 Work::m_name，用于调试和可视化。
    /// @tparam S 任何可用于构造 std::string 的类型。
    /// @param name 新的任务名称。
    /// @return *this（lvalue 链式调用）。
    template <typename S>
        requires std::constructible_from<std::string, S>
    Task& name(S&& name) &;

    /// @brief 右值限定重载 —— 返回 `Task` 值以支持临时对象链式调用。
    template <typename S>
        requires std::constructible_from<std::string, S>
    Task name(S&& name) &&;

    /// @brief 将当前任务设为 @p ts 中每个任务的前驱，建立 this -> each(t) 依赖。
    /// @tparam Ts 每个参数必须为 Task 类型。
    /// @param ts 一个或多个后继任务。
    /// @return *this（lvalue 链式调用）。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    Task& precede(Ts&&... ts) &;

    /// @brief 右值限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    Task precede(Ts&&... ts) &&;

    /// @brief 将当前任务设为 @p ts 中每个任务的后继，建立 each(t) -> this 依赖。
    /// @tparam Ts 每个参数必须为 Task 类型。
    /// @param ts 一个或多个前驱任务。
    /// @return *this（lvalue 链式调用）。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    Task& succeed(Ts&&... ts) &;

    /// @brief 右值限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    Task succeed(Ts&&... ts) &&;

    /// @brief 移除当前任务与 @p ts 中各任务的前驱关系（即解除 each(t) -> this 依赖）。
    /// @tparam Ts 每个参数必须为 Task 类型。
    /// @param ts 要解除的前驱任务。
    /// @return *this（lvalue 链式调用）。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    Task& remove_predecessor(Ts&&... ts) & noexcept;

    /// @brief 右值限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    Task remove_predecessor(Ts&&... ts) && noexcept;

    /// @brief 移除当前任务与 @p ts 中各任务的后继关系（即解除 this -> each(t) 依赖）。
    /// @tparam Ts 每个参数必须为 Task 类型。
    /// @param ts 要解除的后继任务。
    /// @return *this（lvalue 链式调用）。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    Task& remove_successor(Ts&&... ts) & noexcept;

    /// @brief 右值限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    Task remove_successor(Ts&&... ts) && noexcept;

    /// @brief 清空所有前驱关系，解除所有节点到当前任务的依赖。
    /// @return *this（lvalue 链式调用）。
    Task& clear_predecessors() & noexcept;

    /// @brief 右值限定重载。
    Task clear_predecessors() && noexcept;

    /// @brief 清空所有后继关系，解除当前任务到所有节点的依赖。
    /// @return *this（lvalue 链式调用）。
    Task& clear_successors() & noexcept;

    /// @brief 右值限定重载。
    Task clear_successors() && noexcept;

    // ========================================================================
    //  信号量管理
    // ========================================================================

    /// @brief 声明任务执行前需获取的信号量，每个默认占用 1 个配额。
    /// @tparam Ts 每个参数必须为 Semaphore 类型。
    /// @param sems 一个或多个信号量引用。
    /// @return *this（lvalue 链式调用）。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    Task& acquire(Ts&&... sems) &;

    /// @brief 右值限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    Task acquire(Ts&&... sems) &&;

    /// @brief 声明任务执行后需释放的信号量，每个默认释放 1 个配额。
    /// @tparam Ts 每个参数必须为 Semaphore 类型。
    /// @param sems 一个或多个信号量引用。
    /// @return *this（lvalue 链式调用）。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    Task& release(Ts&&... sems) &;

    /// @brief 右值限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    Task release(Ts&&... sems) &&;

    /// @brief 声明任务执行前需获取的信号量及对应配额（交错参数：sem1, count1, sem2, count2, ...）。
    /// @tparam Ts 参数类型必须满足 sem_count_sequence（Semaphore 与整型交错排列）。
    /// @param args 成对的 (Semaphore&, std::size_t) 交错参数。
    /// @return *this（lvalue 链式调用）。
    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    Task& acquire(Ts&&... args) &;

    /// @brief 右值限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    Task acquire(Ts&&... args) &&;

    /// @brief 声明任务执行后需释放的信号量及对应配额（交错参数：sem1, count1, sem2, count2, ...）。
    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    Task& release(Ts&&... args) &;

    /// @brief 右值限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    Task release(Ts&&... args) &&;

    /// @brief 移除执行前指定的信号量获取约束（按信号量引用地址匹配移除）。
    /// @param sems 要移除的信号量引用。
    /// @return *this（lvalue 链式调用）。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    Task& remove_acquire(Ts&&... sems) & noexcept;

    /// @brief 右值限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    Task remove_acquire(Ts&&... sems) && noexcept;

    /// @brief 移除执行后指定的信号量释放约束（按信号量引用地址匹配移除）。
    /// @param sems 要移除的信号量引用。
    /// @return *this（lvalue 链式调用）。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    Task& remove_release(Ts&&... sems) & noexcept;

    /// @brief 右值限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    Task remove_release(Ts&&... sems) && noexcept;

    /// @brief 移除所有执行前信号量获取约束，释放 acquire 列表。
    /// @return *this（lvalue 链式调用）。
    Task& clear_acquires() & noexcept;

    /// @brief 右值限定重载。
    Task clear_acquires() && noexcept;

    /// @brief 移除所有执行后信号量释放约束，释放 release 列表。
    /// @return *this（lvalue 链式调用）。
    Task& clear_releases() & noexcept;

    /// @brief 右值限定重载。
    Task clear_releases() && noexcept;

    // ========================================================================
    //  迭代访问
    // ========================================================================

    /// @brief 遍历当前任务的所有前驱任务，对每个调用 visitor(Task{predecessor})。
    /// @tparam F 可调用对象，接收 Task 句柄。
    /// @param visitor 访问器，非 const 重载允许修改前驱节点的属性。
    template <std::invocable<Task> F>
    void for_each_predecessor(F&& visitor) noexcept(std::is_nothrow_invocable_v<F&, Task>);

    /// @brief const 重载 —— 遍历时提供 `TaskView` 只读句柄，禁止修改前驱节点。
    template <std::invocable<TaskView> F>
    void for_each_predecessor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F&, TaskView>);

    /// @brief 遍历当前任务的所有后继任务，对每个调用 visitor(Task{successor})。
    /// @tparam F 可调用对象，接收 Task 句柄。
    /// @param visitor 访问器，非 const 重载允许修改后继节点的属性。
    template <std::invocable<Task> F>
    void for_each_successor(F&& visitor) noexcept(std::is_nothrow_invocable_v<F&, Task>);

    /// @brief const 重载 —— 遍历时提供 `TaskView` 只读句柄，禁止修改后继节点。
    template <std::invocable<TaskView> F>
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


    // ========================================================================
    //  观察者管理
    // ========================================================================

    /// @brief 注册任务观察者，在任务执行前后接收回调。
    /// @tparam Observer TaskObserver 的派生类型。
    /// @param args 构造 Observer 所需参数。
    /// @return 已注册观察者的 shared_ptr，可用于后续注销。
    template <std::derived_from<TaskObserver> Observer, typename... Args>
        requires std::constructible_from<Observer, Args...>
    [[nodiscard]] std::shared_ptr<Observer> register_observer(Args&&... args);

    /// @brief 注销指定任务观察者。
    /// @param ptr register_observer 返回的观察者指针。
    template <std::derived_from<TaskObserver> Observer>
    void unregister_observer(std::shared_ptr<Observer> ptr) noexcept;
private:
    Work* m_work{nullptr};  ///< 底层 Work 节点指针，非拥有引用。

    /// @brief 从底层 Work 指针构造任务句柄。
    explicit Task(Work* work) noexcept;
};

// ============================================================================
// Task Implementation
// ============================================================================

inline Task::Task(Work* work) noexcept : m_work{work} {}
inline Task::Task(std::nullptr_t) noexcept : m_work{nullptr} {}
inline Task::Task(const Task& rhs) noexcept : m_work{rhs.m_work} {}

inline Task& Task::operator=(const Task& rhs) noexcept {
    m_work = rhs.m_work;
    return *this;
}

inline Task::Task(Task&& rhs) noexcept : m_work{rhs.m_work} {
    rhs.m_work = nullptr;
}

inline Task& Task::operator=(Task&& rhs) noexcept {
    if (this != &rhs) {
        m_work = rhs.m_work;
        rhs.m_work = nullptr;
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
    return m_work->m_name;
}

inline bool Task::valid() const noexcept {
    return m_work != nullptr;
}

inline std::size_t Task::num_successors() const noexcept {
    return m_work->m_num_successors;
}

inline std::size_t Task::num_predecessors() const noexcept {
    return m_work->_num_predecessors();
}

inline std::size_t Task::num_acquires() const noexcept {
    return m_work->_num_acquires();
}

inline std::size_t Task::num_releases() const noexcept {
    return m_work->_num_releases();
}

inline std::size_t Task::num_observers() const noexcept {
    return m_work->_num_observers();
}

inline Task::operator bool() const noexcept {
    return m_work != nullptr;
}

inline bool Task::has_exception() const noexcept {
    return m_work->_has_exception();
}

inline std::exception_ptr Task::exception() const noexcept {
    return m_work->m_exception_ptr;
}

inline TaskType Task::type() const noexcept {
    return m_work->m_type;
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
inline Task& Task::name(S&& name) & {
    m_work->m_name = std::forward<S>(name);
    return *this;
}

template <typename S>
    requires std::constructible_from<std::string, S>
inline Task Task::name(S&& name) && {
    m_work->m_name = std::forward<S>(name);
    return std::move(*this);
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
inline Task& Task::precede(Ts&&... ts) & {
    (m_work->_precede(ts.m_work), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
inline Task Task::precede(Ts&&... ts) && {
    (m_work->_precede(ts.m_work), ...);
    return std::move(*this);
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
inline Task& Task::succeed(Ts&&... ts) & {
    (ts.m_work->_precede(m_work), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
inline Task Task::succeed(Ts&&... ts) && {
    (ts.m_work->_precede(m_work), ...);
    return std::move(*this);
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
inline Task& Task::remove_predecessor(Ts&&... ts) & noexcept {
    (ts.m_work->_remove_successor(m_work), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
inline Task Task::remove_predecessor(Ts&&... ts) && noexcept {
    (ts.m_work->_remove_successor(m_work), ...);
    return std::move(*this);
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
inline Task& Task::remove_successor(Ts&&... ts) & noexcept {
    (m_work->_remove_successor(ts.m_work), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
inline Task Task::remove_successor(Ts&&... ts) && noexcept {
    (m_work->_remove_successor(ts.m_work), ...);
    return std::move(*this);
}

inline Task& Task::clear_predecessors() & noexcept {
    m_work->_clear_predecessors(); return *this;
}

inline Task Task::clear_predecessors() && noexcept {
    m_work->_clear_predecessors(); return std::move(*this);
}

inline Task& Task::clear_successors() & noexcept {
    m_work->_clear_successors(); return *this;
}

inline Task Task::clear_successors() && noexcept {
    m_work->_clear_successors(); return std::move(*this);
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline Task& Task::acquire(Ts&&... sems) & {
    (m_work->_acquire(&sems, 1ULL), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline Task Task::acquire(Ts&&... sems) && {
    (m_work->_acquire(&sems, 1ULL), ...);
    return std::move(*this);
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline Task& Task::release(Ts&&... sems) & {
    (m_work->_release(&sems, 1ULL), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline Task Task::release(Ts&&... sems) && {
    (m_work->_release(&sems, 1ULL), ...);
    return std::move(*this);
}

template <typename... Ts>
    requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
inline Task& Task::acquire(Ts&&... args) & {
    auto tup = std::forward_as_tuple(std::forward<Ts>(args)...);
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (m_work->_acquire(&std::get<Is * 2>(tup),
                          static_cast<std::size_t>(std::get<Is * 2 + 1>(tup))), ...);
    }(std::make_index_sequence<sizeof...(Ts) / 2>{});
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
inline Task Task::acquire(Ts&&... args) && {
    auto tup = std::forward_as_tuple(std::forward<Ts>(args)...);
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (m_work->_acquire(&std::get<Is * 2>(tup),
                          static_cast<std::size_t>(std::get<Is * 2 + 1>(tup))), ...);
    }(std::make_index_sequence<sizeof...(Ts) / 2>{});
    return std::move(*this);
}

template <typename... Ts>
    requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
inline Task& Task::release(Ts&&... args) & {
    auto tup = std::forward_as_tuple(std::forward<Ts>(args)...);
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (m_work->_release(&std::get<Is * 2>(tup),
                          static_cast<std::size_t>(std::get<Is * 2 + 1>(tup))), ...);
    }(std::make_index_sequence<sizeof...(Ts) / 2>{});
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
inline Task Task::release(Ts&&... args) && {
    auto tup = std::forward_as_tuple(std::forward<Ts>(args)...);
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (m_work->_release(&std::get<Is * 2>(tup),
                          static_cast<std::size_t>(std::get<Is * 2 + 1>(tup))), ...);
    }(std::make_index_sequence<sizeof...(Ts) / 2>{});
    return std::move(*this);
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline Task& Task::remove_acquire(Ts&&... sems) & noexcept {
    (m_work->_remove_acquire(&sems), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline Task Task::remove_acquire(Ts&&... sems) && noexcept {
    (m_work->_remove_acquire(&sems), ...);
    return std::move(*this);
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline Task& Task::remove_release(Ts&&... sems) & noexcept {
    (m_work->_remove_release(&sems), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline Task Task::remove_release(Ts&&... sems) && noexcept {
    (m_work->_remove_release(&sems), ...);
    return std::move(*this);
}

inline Task& Task::clear_acquires() & noexcept {
    m_work->_clear_acquires(); return *this;
}

inline Task Task::clear_acquires() && noexcept {
    m_work->_clear_acquires(); return std::move(*this);
}

inline Task& Task::clear_releases() & noexcept {
    m_work->_clear_releases(); return *this;
}

inline Task Task::clear_releases() && noexcept {
    m_work->_clear_releases(); return std::move(*this);
}

template <std::invocable<Task> F>
inline void Task::for_each_predecessor(F&& visitor) noexcept(std::is_nothrow_invocable_v<F&, Task>) {
    for (Work* pred : m_work->_predecessors()) {
        std::invoke(visitor, Task{pred});
    }
}

template <std::invocable<TaskView> F>
inline void Task::for_each_predecessor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F&, TaskView>) {
    for (const Work* pred : m_work->_predecessors()) {
        std::invoke(visitor, TaskView{*pred});
    }
}

template <std::invocable<Task> F>
inline void Task::for_each_successor(F&& visitor) noexcept(std::is_nothrow_invocable_v<F&, Task>) {
    for (Work* succ : m_work->_successors()) {
        std::invoke(visitor, Task{succ});
    }
}

template <std::invocable<TaskView> F>
inline void Task::for_each_successor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F&, TaskView>) {
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
    for (const auto& req : m_work->_releases()) {
        if constexpr (std::invocable<F&, const Semaphore&, std::size_t>) {
            std::invoke(visitor, *req.sem, req.count);
        } else {
            std::invoke(visitor, *req.sem);
        }
    }
}

template <std::derived_from<TaskObserver> Observer, typename... Args>
    requires std::constructible_from<Observer, Args...>
inline std::shared_ptr<Observer> Task::register_observer(Args&&... args) {
    auto ptr = std::make_shared<Observer>(std::forward<Args>(args)...);
    if (!m_work->m_observers) {
        m_work->m_observers = std::make_unique<Work::ObserverData>();
    }
    m_work->m_observers->observers.emplace_back(std::static_pointer_cast<TaskObserver>(ptr));
    return ptr;
}

template <std::derived_from<TaskObserver> Observer>
inline void Task::unregister_observer(std::shared_ptr<Observer> ptr) noexcept {
    if (!m_work->m_observers) return;
    auto base = std::static_pointer_cast<TaskObserver>(ptr);
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

/// @brief 右值限定重载。
template <typename L>
    requires std::same_as<std::remove_cvref_t<L>, Task>
inline Task operator>>(L&& lhs, Task&& rhs) {
    rhs.succeed(std::forward<L>(lhs));
    return std::move(rhs);
}

/// @brief 拓扑连线运算符：`a << b` 等价于 `b.precede(a)`，建立 a → b 依赖。
template <typename L>
    requires std::same_as<std::remove_cvref_t<L>, Task>
inline Task& operator<<(L&& lhs, Task& rhs) {
    rhs.precede(std::forward<L>(lhs));
    return rhs;
}

/// @brief 右值限定重载。
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
/// @brief `tfl::Task` 的哈希支持，委托给 `Task::hash_value()`。
template <>
struct hash<tfl::Task> {
    inline auto operator()(const tfl::Task& t) const noexcept { return t.hash_value(); }
};

/// @brief `tfl::TaskView` 的哈希支持，委托给 `TaskView::hash_value()`。
template <>
struct hash<tfl::TaskView> {
    inline auto operator()(const tfl::TaskView& tv) const noexcept { return tv.hash_value(); }
};
} // namespace std
