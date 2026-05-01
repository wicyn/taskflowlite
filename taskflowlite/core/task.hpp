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

/// @brief DAG 节点的轻量级用户层句柄。
///
/// @details
/// `Task` 是 **裸 Work\* 上的薄包装**：内部仅一个指针，对外提供 fluent API
/// 操作底层节点的依赖、信号量、观察者、命名等属性。所有 mutate 操作都直接
/// 落到对应 `Work` 上，自身无任何独立状态。
///
/// 它存在的根本理由是 **分层（layering）**：
/// - `Work` 是框架内部类型，包含原子状态机 / 拓扑指针 / 异常归档等大量内部细节，
///   不能直接暴露给用户（既会泄露 ABI，也容易被误改）。
/// - `Task` 把"用户合法可改的那部分"切片暴露 —— 通过白名单 API 而非禁止名单。
///
/// ============================================================================
///  与同族句柄的边界
/// ============================================================================
/// 框架里"指向 Work 的句柄"有四种，各自语义截然不同 —— 选错会引发难以察觉的
/// 生命周期 bug：
///
/// | 句柄                | 所有权语义     | 拷贝代价  | 生命周期由谁兜底               |
/// |---------------------|----------------|-----------|--------------------------------|
/// | `Task`              | **弱引用**     | 单指针拷贝 | 归属的 `Flow` / `Graph`        |
/// | `TaskView`          | **弱 const**   | 单引用拷贝 | 同 Task，编译期禁止 mutate    |
/// | `AsyncTask`         | **强引用**     | refcount  | `Topology` 引用计数            |
/// | `DeferredAsyncTask` | 同 AsyncTask   | refcount  | 同 AsyncTask，但提供启动期可写 |
///
/// 关键约束：**`Task` 不参与生命周期管理**。Flow 析构后所有 Task 句柄立即悬挂，
/// 这是有意为之 —— 用户在图编辑期持有 Task，编辑结束后自然丢弃；如果需要在
/// 调度阶段拿到任务的引用，应该用 `AsyncTask`。
///
/// 拷贝 / 移动 `Task` 都是 trivial 的指针搬运：
/// - 拷贝：复制指针，原句柄仍有效；
/// - 移动：转移指针，原句柄被置 null（只是为了语义清晰，不是必须）；
/// - 等值：指针等同。
///
/// ============================================================================
///  Fluent API 与依赖建模
/// ============================================================================
/// 拓扑构建走 fluent 链式风格，所有 mutator 返回 `Task&`：
/// @code
///   t1.name("load")
///     .precede(t2, t3)            // t1 → {t2, t3}
///     .acquire(sem_a)             // 进入需 acquire(sem_a)
///     .release(sem_a, sem_b);     // 完成时 release(sem_a, sem_b)
/// @endcode
/// `precede` / `succeed` 在 `Work::m_edges` 上 **双向同步** 更新前驱与后继 ——
/// 这是 DAG 拓扑一致性的硬约束：单边更新会让 join_counter 计算错误、导致死锁
/// 或漏调度。`remove_predecessor` / `remove_successor` / `clear_*` 同理双向。
///
/// 信号量两族 API：
/// - `acquire(Sem...)`               —— 单位计数模式（每个 sem 各占 1）；
/// - `acquire(Sem, count, ...)`      —— 多计数模式（`sem_count_sequence` 概念）。
/// `release` 同构。同一信号量重复 acquire 会累加计数。
///
/// ============================================================================
///  迭代与观察者
/// ============================================================================
/// `for_each_predecessor` / `for_each_successor` / `for_each_acquire` /
/// `for_each_release` 提供**只读式遍历**：访问者拿到的依然是 `Task`（可写）
/// 或信号量引用，遍历期间用户**不应**修改邻接表（迭代器失效未做防护，是编辑期
/// 单线程契约的延续）。
///
/// `register_observer<T>(args...)` 在 `Work::m_observers` 上挂一个 shared_ptr，
/// 调度时被 `_notify_before` / `_notify_after` 回调。返回 shared 是为了让用户
/// 也能持有，方便后续 `unregister_observer`。
///
/// ============================================================================
///  TaskView —— 给 TaskObserver 的安全只读切片
/// ============================================================================
/// `TaskView` 持有 `const Work&`，**编译期** 屏蔽所有 mutator。设计动机：
/// 框架在调度期会把节点信息回调给用户实现的 `TaskObserver`，这个回调时机
/// 跑在 worker 线程上，绝不能允许用户篡改节点状态（会与调度器并发写）。
///
/// 类型系统在此承担安全边界：传 `Task` 给 observer 在编译期就被禁掉，
/// 只能传 `TaskView` —— 比运行期断言更早、也更省。
///
/// ============================================================================
///  哈希与容器
/// ============================================================================
/// `Task` 与 `TaskView` 的 hash 都基于底层 `Work*` 地址（同一节点不同视图
/// 哈希相同）。框架特化了 `std::hash<Task>` 与 `std::hash<TaskView>`，
/// 可直接用作 `unordered_map` / `unordered_set` 的键。
///
/// ============================================================================
///  使用示例
/// ============================================================================
/// @code
///   Flow flow;
///   auto load = flow.emplace([]{ /* ... */ }).name("load");
///   auto proc = flow.emplace([]{ /* ... */ }).name("process");
///   auto save = flow.emplace([]{ /* ... */ }).name("save");
///
///   load.precede(proc, save);    // load → {proc, save}（双向同步）
///   save.succeed(proc);          // 等价 proc.precede(save)
///
///   load.acquire(io_sem).release(io_sem);
///   load.register_observer<MyTracer>("loader");
/// @endcode
///
/// @see Flow              Task 的所有者与生产者
/// @see Work              Task 包装的内部节点
/// @see TaskView          只读对偶，给 TaskObserver 用
/// @see AsyncTask         强引用对偶，参与 topology 引用计数


class Task {
    friend class Flow;
    friend class Runtime;
    friend class Executor;

public:
    explicit Task() = default;
    explicit Task(std::nullptr_t) noexcept;

    Task(const Task& rhs) noexcept;
    Task& operator=(const Task& rhs) noexcept;
    Task(Task&& rhs) noexcept;
    Task& operator=(Task&& rhs) noexcept;
    Task& operator=(std::nullptr_t) noexcept;

    [[nodiscard]] bool operator==(const Task& rhs) const noexcept;
    [[nodiscard]] bool operator!=(const Task& rhs) const noexcept;

    void reset() noexcept;

    // ========================================================================
    //  状态查询
    // ========================================================================
    [[nodiscard]] std::size_t hash_value() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::size_t num_successors() const noexcept;
    [[nodiscard]] std::size_t num_predecessors() const noexcept;
    [[nodiscard]] std::size_t num_acquires() const noexcept;
    [[nodiscard]] std::size_t num_releases() const noexcept;
    [[nodiscard]] std::size_t num_observers() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] bool has_exception() const noexcept;
    [[nodiscard]] TaskType type() const noexcept;
    std::exception_ptr exception() const noexcept;

    [[nodiscard]] std::string dump(Direction dir = Direction::Default) const;
    void dump(std::ostream& ostream, Direction dir = Direction::Default) const;

    // ========================================================================
    //  拓扑构建
    // ========================================================================
    template <typename S>
        requires std::constructible_from<std::string, S>
    Task& name(S&& name);

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    Task& precede(Ts&&... ts);

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    Task& succeed(Ts&&... ts);

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    Task& remove_predecessor(Ts&&... ts) noexcept;

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
    Task& remove_successor(Ts&&... ts) noexcept;

    Task& clear_predecessors() noexcept;
    Task& clear_successors() noexcept;

    // ========================================================================
    //  信号量管理
    // ========================================================================
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    Task& acquire(Ts&&... sems);

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    Task& release(Ts&&... sems);

    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    Task& acquire(Ts&&... args);

    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    Task& release(Ts&&... args);

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    Task& remove_acquire(Ts&&... sems) noexcept;

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    Task& remove_release(Ts&&... sems) noexcept;

    Task& clear_acquires() noexcept;
    Task& clear_releases() noexcept;

    // ========================================================================
    //  迭代访问
    // ========================================================================
    template <std::invocable<Task> F>
    void for_each_predecessor(F&& visitor)
        noexcept(std::is_nothrow_invocable_v<F, Task>);

    template <std::invocable<Task> F>
    void for_each_successor(F&& visitor)
        noexcept(std::is_nothrow_invocable_v<F, Task>);

    template <typename F>
        requires std::invocable<F, Semaphore&, std::size_t&> || std::invocable<F, Semaphore&>
    void for_each_acquire(F&& visitor) noexcept(
        std::invocable<F, Semaphore&, std::size_t&>
            ? std::is_nothrow_invocable_v<F, Semaphore&, std::size_t&>
            : std::is_nothrow_invocable_v<F, Semaphore&>
        );

    template <typename F>
        requires std::invocable<F, Semaphore&, std::size_t&> || std::invocable<F, Semaphore&>
    void for_each_release(F&& visitor) noexcept(
        std::invocable<F, Semaphore&, std::size_t&>
            ? std::is_nothrow_invocable_v<F, Semaphore&, std::size_t&>
            : std::is_nothrow_invocable_v<F, Semaphore&>
        );

    // ========================================================================
    //  观察者管理
    // ========================================================================
    template <std::derived_from<TaskObserver> Observer, typename... Args>
        requires std::constructible_from<Observer, Args...>
    [[nodiscard]] std::shared_ptr<Observer> register_observer(Args&&... args);

    template <std::derived_from<TaskObserver> Observer>
    void unregister_observer(std::shared_ptr<Observer> ptr) noexcept;

private:
    Work* m_work{nullptr};
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
    os << "direction: " << to_string(dir) << "\n\n";
    m_work->dump(os);
    os << "\n";
}

template <typename S>
    requires std::constructible_from<std::string, S>
inline Task& Task::name(S&& name) {
    m_work->m_name = std::forward<S>(name);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
Task& Task::precede(Ts&&... ts) {
    (m_work->_precede(ts.m_work), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
Task& Task::succeed(Ts&&... ts) {
    (ts.m_work->_precede(m_work), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
Task& Task::remove_predecessor(Ts&&... ts) noexcept {
    (ts.m_work->_remove_successor(m_work), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Task> && ...)
Task& Task::remove_successor(Ts&&... ts) noexcept {
    (m_work->_remove_successor(ts.m_work), ...);
    return *this;
}

inline Task& Task::clear_predecessors() noexcept {
    m_work->_clear_predecessors(); return *this;
}

inline Task& Task::clear_successors() noexcept {
    m_work->_clear_successors(); return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
Task& Task::acquire(Ts&&... sems) {
    (m_work->_acquire(&sems, 1ULL), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
Task& Task::release(Ts&&... sems) {
    (m_work->_release(&sems, 1ULL), ...);
    return *this;
}


template <typename... Ts>
    requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
Task& Task::acquire(Ts&&... args) {
    auto tup = std::forward_as_tuple(std::forward<Ts>(args)...);
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (m_work->_acquire(&std::get<Is * 2>(tup),
                          static_cast<std::size_t>(std::get<Is * 2 + 1>(tup))), ...);
    }(std::make_index_sequence<sizeof...(Ts) / 2>{});
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
Task& Task::release(Ts&&... args) {
    auto tup = std::forward_as_tuple(std::forward<Ts>(args)...);
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (m_work->_release(&std::get<Is * 2>(tup),
                          static_cast<std::size_t>(std::get<Is * 2 + 1>(tup))), ...);
    }(std::make_index_sequence<sizeof...(Ts) / 2>{});
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
Task& Task::remove_acquire(Ts&&... sems) noexcept {
    (m_work->_remove_acquire(&sems), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
Task& Task::remove_release(Ts&&... sems) noexcept {
    (m_work->_remove_release(&sems), ...);
    return *this;
}

inline Task& Task::clear_acquires() noexcept { m_work->_clear_acquires(); return *this; }
inline Task& Task::clear_releases() noexcept { m_work->_clear_releases(); return *this; }

template <std::invocable<Task> F>
void Task::for_each_predecessor(F&& visitor) noexcept(std::is_nothrow_invocable_v<F, Task>) {
    for (Work* pred : m_work->_predecessors()) {
        std::invoke(visitor, Task{pred});
    }
}

template <std::invocable<Task> F>
void Task::for_each_successor(F&& visitor) noexcept(std::is_nothrow_invocable_v<F, Task>) {
    for (Work* succ : m_work->_successors()) {
        std::invoke(visitor, Task{succ});
    }
}

template <typename F>
    requires std::invocable<F, Semaphore&, std::size_t&> || std::invocable<F, Semaphore&>
void Task::for_each_acquire(F&& visitor) noexcept(
    std::invocable<F, Semaphore&, std::size_t&>
        ? std::is_nothrow_invocable_v<F, Semaphore&, std::size_t&>
        : std::is_nothrow_invocable_v<F, Semaphore&>
    ) {
    for (const auto& req : m_work->_acquires()) {
        if constexpr (std::invocable<F, Semaphore&, std::size_t&>) {
            std::invoke(visitor, *req.sem, req.count);
        } else {
            std::invoke(visitor, *req.sem);
        }
    }
}

template <typename F>
    requires std::invocable<F, Semaphore&, std::size_t&> || std::invocable<F, Semaphore&>
void Task::for_each_release(F&& visitor) noexcept(
    std::invocable<F, Semaphore&, std::size_t&>
        ? std::is_nothrow_invocable_v<F, Semaphore&, std::size_t&>
        : std::is_nothrow_invocable_v<F, Semaphore&>
    ) {
    for (const auto& req : m_work->_releases()) {
        if constexpr (std::invocable<F, Semaphore&, std::size_t&>) {
            std::invoke(visitor, *req.sem, req.count);
        } else {
            std::invoke(visitor, *req.sem);
        }
    }
}

template <std::derived_from<TaskObserver> Observer, typename... Args>
    requires std::constructible_from<Observer, Args...>
std::shared_ptr<Observer> Task::register_observer(Args&&... args) {
    auto ptr = std::make_shared<Observer>(std::forward<Args>(args)...);
    if (!m_work->m_observers) {
        m_work->m_observers = std::make_unique<Work::ObserverData>();
    }
    m_work->m_observers->observers.emplace_back(std::static_pointer_cast<TaskObserver>(ptr));
    return ptr;
}

template <std::derived_from<TaskObserver> Observer>
void Task::unregister_observer(std::shared_ptr<Observer> ptr) noexcept {
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

// ============================================================================
// TaskView - 只读视图
// ============================================================================
/// @brief Task 的 const 视图代理 —— 跨 API 边界传递节点信息的安全形态。
///
/// @details
/// `TaskView` 与 `Task` 持有同一份 `Work` 指针 / 引用，但 **编译期** 禁掉所有
/// mutator。专为 `TaskObserver::on_before` / `on_after` 等用户实现的回调
/// 设计 —— 框架希望让用户读节点信息，但不允许在调度回调中改它。
///
/// 这是把"线程安全契约"从运行期断言提升到 **类型系统** 的典型手法：
/// 不需要 assert、不需要锁、不需要文档警告 —— 编译错误就是契约本身。
///
/// 所有访问器与 `Task` 的同名只读访问器一一对应，hash / equality 也按
/// `Work*` 地址语义，因此 `Task` 与 `TaskView` 在容器中可以互查。
///
/// @note `TaskView` 不可默认构造（持引用），必须由框架内部构造后传出。
/// @see Task        可写对偶
/// @see TaskObserver  使用 TaskView 的回调接口
class TaskView {
    friend class Executor;
    friend class Branch;
    friend class MultiBranch;
    friend class Jump;
    friend class MultiJump;
    friend class Task;

public:
    [[nodiscard]] bool operator==(const TaskView& rhs) const noexcept;
    [[nodiscard]] bool operator!=(const TaskView& rhs) const noexcept;
    [[nodiscard]] std::size_t hash_value() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] std::size_t num_successors() const noexcept;
    [[nodiscard]] std::size_t num_predecessors() const noexcept;
    [[nodiscard]] std::size_t num_acquires() const noexcept;
    [[nodiscard]] std::size_t num_releases() const noexcept;
    [[nodiscard]] std::size_t num_observers() const noexcept;
    [[nodiscard]] TaskType type() const noexcept;
    [[nodiscard]] bool has_exception() const noexcept;
    [[nodiscard]] std::exception_ptr exception() const noexcept;
    [[nodiscard]] std::string dump(Direction dir = Direction::Default) const;
    void dump(std::ostream& ostream, Direction dir = Direction::Default) const;

    template <std::invocable<TaskView> F>
    void for_each_predecessor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F, TaskView>);

    template <std::invocable<TaskView> F>
    void for_each_successor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F, TaskView>);

    template <typename F>
        requires std::invocable<F, const Semaphore&, std::size_t> || std::invocable<F, const Semaphore&>
    void for_each_acquire(F&& visitor) const noexcept(
        std::invocable<F, const Semaphore&, std::size_t>
            ? std::is_nothrow_invocable_v<F, const Semaphore&, std::size_t>
            : std::is_nothrow_invocable_v<F, const Semaphore&>
        );

    template <typename F>
        requires std::invocable<F, const Semaphore&, std::size_t> || std::invocable<F, const Semaphore&>
    void for_each_release(F&& visitor) const noexcept(
        std::invocable<F, const Semaphore&, std::size_t>
            ? std::is_nothrow_invocable_v<F, const Semaphore&, std::size_t>
            : std::is_nothrow_invocable_v<F, const Semaphore&>
        );

private:
    explicit TaskView(const Work& work) noexcept : m_work{work} {}
    const Work& m_work;
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
void TaskView::for_each_predecessor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F, TaskView>) {
    for (const Work* pred : m_work._predecessors()) {
        std::invoke(std::forward<F>(visitor), TaskView{*pred});
    }
}

template <std::invocable<TaskView> F>
void TaskView::for_each_successor(F&& visitor) const noexcept(std::is_nothrow_invocable_v<F, TaskView>) {
    for (const Work* succ : m_work._successors()) {
        std::invoke(std::forward<F>(visitor), TaskView{*succ});
    }
}

template <typename F>
    requires std::invocable<F, const Semaphore&, std::size_t> || std::invocable<F, const Semaphore&>
void TaskView::for_each_acquire(F&& visitor) const noexcept(
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
void TaskView::for_each_release(F&& visitor) const noexcept(
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

inline std::ostream& operator << (std::ostream& os, const Task& task) {
    task.dump(os);
    return os;
}

inline std::ostream& operator << (std::ostream& os, const TaskView& task) {
    task.dump(os);
    return os;
}

} // namespace tfl


namespace std {
template <>
struct hash<tfl::Task> {
    inline auto operator()(const tfl::Task& t) const noexcept { return t.hash_value(); }
};

template <>
struct hash<tfl::TaskView> {
    inline auto operator()(const tfl::TaskView& tv) const noexcept { return tv.hash_value(); }
};
} // namespace std
