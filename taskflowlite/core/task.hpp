#pragma once

#include "work.hpp"
#include "semaphore.hpp"
#include "enums.hpp"
namespace tfl {

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

    // ==================== Getter ====================

    [[nodiscard]] std::size_t hash_value() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::size_t num_successors() const noexcept;
    [[nodiscard]] std::size_t num_predecessors() const noexcept;
    [[nodiscard]] std::size_t num_acquires() const noexcept;
    [[nodiscard]] std::size_t num_releases() const noexcept;
    [[nodiscard]] std::size_t num_observers() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] bool has_exception_ptr() const noexcept;
    [[nodiscard]] TaskType type() const noexcept;
    [[nodiscard]] std::string dump(Direction dir = Direction::Default) const;
    void dump(std::ostream& ostream, Direction dir = Direction::Default) const;

    std::exception_ptr exception_ptr() const noexcept;

    // ==================== 链式方法 ====================

    Task& name(const std::string& n);

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

    // ==================== 信号量 ====================

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    Task& acquire(Ts&&... sems);

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    Task& release(Ts&&... sems);

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    Task& remove_acquire(Ts&&... sems) noexcept;

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    Task& remove_release(Ts&&... sems) noexcept;

    Task& clear_acquires() noexcept;
    Task& clear_releases() noexcept;

    // ==================== 遍历 ====================

    template <std::invocable<Task> F>
    void for_each_predecessor(F&& visitor)
        noexcept(std::is_nothrow_invocable_v<F, Task>);

    template <std::invocable<Task> F>
    void for_each_successor(F&& visitor)
        noexcept(std::is_nothrow_invocable_v<F, Task>);

    template <std::invocable<Semaphore&> F>
    void for_each_acquire(F&& visitor)
        noexcept(std::is_nothrow_invocable_v<F, Semaphore&>);

    template <std::invocable<Semaphore&> F>
    void for_each_release(F&& visitor)
        noexcept(std::is_nothrow_invocable_v<F, Semaphore&>);

    // ==================== Observer ====================

    template <std::derived_from<TaskObserver> Observer, typename... Args>
        requires std::constructible_from<Observer, Args...>
    [[nodiscard]] std::shared_ptr<Observer> register_observer(Args&&... args);

    template <std::derived_from<TaskObserver> Observer>
    void unregister_observer(std::shared_ptr<Observer> ptr) noexcept;
private:
    Work* m_work{nullptr};

    explicit Task(Work* work) noexcept;
};

// ============================================================
// Task 实现
// ============================================================

// ==================== 构造/赋值/比较 ====================

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

// ==================== Getter ====================

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
    return m_work->m_observers ? m_work->m_observers->observers.size() : 0;
}

inline Task::operator bool() const noexcept {
    return m_work != nullptr;
}
bool Task::has_exception_ptr() const noexcept {
    return m_work->m_exception_ptr != nullptr;
}

std::exception_ptr Task::exception_ptr() const noexcept {
    return m_work->m_exception_ptr;
}

inline TaskType Task::type() const noexcept {
    return m_work->type();
}

inline std::string Task::dump(Direction dir) const {
    std::string out;
    out += "direction: ";
    out += to_string(dir);
    out += "\n\n";

    out += m_work->dump();
    out += "\n";
    return out;
}

inline void Task::dump(std::ostream& os, Direction dir) const {
    os << "direction: " << to_string(dir) << "\n\n";
    m_work->dump(os);
    os << "\n";
}

// ==================== 链式方法 ====================

inline Task& Task::name(const std::string& n) {
    m_work->m_name = n;
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
    m_work->_clear_predecessors();
    return *this;
}

inline Task& Task::clear_successors() noexcept {
    m_work->_clear_successors();
    return *this;
}

// ==================== 信号量 ====================

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
Task& Task::acquire(Ts&&... sems) {
    (m_work->_acquire(&sems), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
Task& Task::release(Ts&&... sems) {
    (m_work->_release(&sems), ...);
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

inline Task& Task::clear_acquires() noexcept {
    m_work->_clear_acquires();
    return *this;
}

inline Task& Task::clear_releases() noexcept {
    m_work->_clear_releases();
    return *this;
}

// ==================== 遍历 ====================

template <std::invocable<Task> F>
void Task::for_each_predecessor(F&& visitor)
    noexcept(std::is_nothrow_invocable_v<F, Task>) {
    for (Work* pred : m_work->_predecessors()) {
        std::invoke(visitor, Task{pred});
    }
}

template <std::invocable<Task> F>
void Task::for_each_successor(F&& visitor)
    noexcept(std::is_nothrow_invocable_v<F, Task>) {
    for (Work* succ : m_work->_successors()) {
        std::invoke(visitor, Task{succ});
    }
}

template <std::invocable<Semaphore&> F>
void Task::for_each_acquire(F&& visitor)
    noexcept(std::is_nothrow_invocable_v<F, Semaphore&>) {
    for (Semaphore* sem : m_work->_acquires()) {
        std::invoke(visitor, *sem);
    }
}

template <std::invocable<Semaphore&> F>
void Task::for_each_release(F&& visitor)
    noexcept(std::is_nothrow_invocable_v<F, Semaphore&>) {
    for (Semaphore* sem : m_work->_releases()) {
        std::invoke(visitor, *sem);
    }
}

// ==================== Observer ====================
template <std::derived_from<TaskObserver> Observer, typename... Args>
    requires std::constructible_from<Observer, Args...>
std::shared_ptr<Observer> Task::register_observer(Args&&... args) {
    auto ptr = std::make_shared<Observer>(std::forward<Args>(args)...);
    if (!m_work->m_observers) {
        m_work->m_observers = std::make_unique<Work::ObserverData>();
    }
    m_work->m_observers->observers.emplace_back(
        std::static_pointer_cast<TaskObserver>(ptr));
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

// ----------------------------------------------------------------------------
// Task View
// ----------------------------------------------------------------------------

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

    // ==================== Getter ====================

    [[nodiscard]] std::size_t hash_value() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] std::size_t num_successors() const noexcept;
    [[nodiscard]] std::size_t num_predecessors() const noexcept;
    [[nodiscard]] std::size_t num_acquires() const noexcept;
    [[nodiscard]] std::size_t num_releases() const noexcept;
    [[nodiscard]] std::size_t num_observers() const noexcept;

    [[nodiscard]] TaskType type() const noexcept;
    [[nodiscard]] std::string dump() const;
    [[nodiscard]] bool has_exception_ptr() const noexcept;
    [[nodiscard]] std::exception_ptr exception_ptr() const noexcept;

    // ==================== 遍历 ====================

    template <std::invocable<TaskView> F>
    void for_each_predecessor(F&& visitor) const
        noexcept(std::is_nothrow_invocable_v<F, TaskView>);

    template <std::invocable<TaskView> F>
    void for_each_successor(F&& visitor) const
        noexcept(std::is_nothrow_invocable_v<F, TaskView>);

    template <std::invocable<const Semaphore&> F>
    void for_each_acquire(F&& visitor) const
        noexcept(std::is_nothrow_invocable_v<F, const Semaphore&>);

    template <std::invocable<const Semaphore&> F>
    void for_each_release(F&& visitor) const
        noexcept(std::is_nothrow_invocable_v<F, const Semaphore&>);

private:
    explicit TaskView(const Work& work) noexcept : m_work{work} {}

    const Work& m_work;
};
// ========== TaskView Inline Implementation ==========

inline bool TaskView::operator==(const TaskView& rhs) const noexcept {
    return &m_work == &rhs.m_work;
}

inline bool TaskView::operator!=(const TaskView& rhs) const noexcept {
    return &m_work != &rhs.m_work;
}

// ==================== Getter ====================

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
    return m_work.m_observers ? m_work.m_observers->observers.size() : 0;
}

inline TaskType TaskView::type() const noexcept {
    return m_work.type();
}

inline std::string TaskView::dump() const {
    return m_work.dump();
}

inline bool TaskView::has_exception_ptr() const noexcept {
    return m_work.m_exception_ptr != nullptr;
}

inline std::exception_ptr TaskView::exception_ptr() const noexcept {
    return m_work.m_exception_ptr;
}

// ==================== 遍历 ====================

template <std::invocable<TaskView> F>
void TaskView::for_each_predecessor(F&& visitor) const
    noexcept(std::is_nothrow_invocable_v<F, TaskView>) {
    for (const Work* pred : m_work._predecessors()) {
        std::invoke(std::forward<F>(visitor), TaskView{*pred});
    }
}

template <std::invocable<TaskView> F>
void TaskView::for_each_successor(F&& visitor) const
    noexcept(std::is_nothrow_invocable_v<F, TaskView>) {
    for (const Work* succ : m_work._successors()) {
        std::invoke(std::forward<F>(visitor), TaskView{*succ});
    }
}

template <std::invocable<const Semaphore&> F>
void TaskView::for_each_acquire(F&& visitor) const
    noexcept(std::is_nothrow_invocable_v<F, const Semaphore&>) {
    for (const Semaphore* sem : m_work._acquires()) {
        std::invoke(std::forward<F>(visitor), *sem);
    }
}

template <std::invocable<const Semaphore&> F>
void TaskView::for_each_release(F&& visitor) const
    noexcept(std::is_nothrow_invocable_v<F, const Semaphore&>) {
    for (const Semaphore* sem : m_work._releases()) {
        std::invoke(std::forward<F>(visitor), *sem);
    }
}

} // namespace tfl

// ========== std::hash 特化 ==========
namespace std {

template <>
struct hash<tfl::Task> {
    inline auto operator()(const tfl::Task& t) const noexcept {
        return t.hash_value();
    }
};

template <>
struct hash<tfl::TaskView> {
    inline auto operator()(const tfl::TaskView& tv) const noexcept {
        return tv.hash_value();
    }
};

}  // end of namespace std ----------------------------------------------------

