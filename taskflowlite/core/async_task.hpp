#pragma once

#include "work.hpp"
#include "semaphore.hpp"
#include "topology.hpp"

namespace tfl {

class AsyncTask {
    friend class Work;
    friend class Graph;
    friend class Flow;
    friend class Runtime;
    friend class Executor;
    friend class Branch;

public:

    AsyncTask() noexcept = default;

    ~AsyncTask();

    explicit AsyncTask(std::nullptr_t) noexcept;

    AsyncTask(const AsyncTask& rhs) noexcept;
    AsyncTask& operator=(const AsyncTask& rhs) noexcept;
    AsyncTask(AsyncTask&& rhs) noexcept;
    AsyncTask& operator=(AsyncTask&& rhs) noexcept;
    AsyncTask& operator=(std::nullptr_t) noexcept;

    [[nodiscard]] bool operator==(const AsyncTask& rhs) const noexcept;
    [[nodiscard]] bool operator!=(const AsyncTask& rhs) const noexcept;

    void reset() noexcept;

    // ==================== Getter ====================

    [[nodiscard]] std::size_t hash_value() const noexcept;
    [[nodiscard]] std::size_t use_count() const noexcept; 
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::size_t num_acquires() const noexcept;
    [[nodiscard]] std::size_t num_releases() const noexcept;
    [[nodiscard]] std::size_t num_observers() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool done() const noexcept;
    [[nodiscard]] TaskType type() const noexcept;
    [[nodiscard]] std::string dump(Direction dir = Direction::Default) const;
    void dump(std::ostream& ostream, Direction dir = Direction::Default) const;

    [[nodiscard]] std::string_view name() const noexcept;
    AsyncTask& name(const std::string& name);

    // ==================== 信号量 ====================

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask& acquire(Ts&&... sems);

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask& release(Ts&&... sems);

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask& remove_acquire(Ts&&... sems) noexcept;

    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask& remove_release(Ts&&... sems) noexcept;

    AsyncTask& clear_acquires() noexcept;
    AsyncTask& clear_releases() noexcept;

    // ==================== 遍历 ====================

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

    // ==================== AsyncTask 特有接口 ====================

    template <typename ...Deps>
        requires (std::same_as<std::remove_cvref_t<Deps>, AsyncTask> && ...)
    AsyncTask& start(Deps&&... deps);

    template <std::input_iterator I, std::sentinel_for<I> S>
        requires std::same_as<std::iter_value_t<I>, AsyncTask>
    AsyncTask& start(I first, S last);

    void stop() noexcept;

    void wait() noexcept;

    void get();
private:
    Work* m_work{nullptr};

    AsyncTask(Work* work) noexcept;

    void _incref() noexcept;
    void _decref() noexcept;
};

// ============================================================
// AsyncTask 实现
// ============================================================
inline void AsyncTask::_incref() noexcept {
    if(m_work) m_work->m_topology->_incref();
}
inline void AsyncTask::_decref() noexcept {
    if(m_work && m_work->m_topology->_decref()) {
        Work::destroy(m_work);
    }
}

inline AsyncTask::AsyncTask(Work* w) noexcept
    : m_work{w} {
    _incref();
}

inline AsyncTask::~AsyncTask() {
    _decref();
}

inline AsyncTask::AsyncTask(std::nullptr_t) noexcept
    : m_work{nullptr} {}

inline AsyncTask::AsyncTask(const AsyncTask& rhs) noexcept
    : m_work{rhs.m_work} {
    _incref();
}

inline AsyncTask& AsyncTask::operator=(const AsyncTask& rhs) noexcept {
    if (this != &rhs) {
        _decref();
        m_work = rhs.m_work;
        _incref();
    }
    return *this;
}

inline AsyncTask::AsyncTask(AsyncTask&& rhs) noexcept
     : m_work{std::exchange(rhs.m_work, nullptr)} {
}

inline AsyncTask& AsyncTask::operator=(AsyncTask&& rhs) noexcept {
    if (this != &rhs) {
        _decref();
        m_work = rhs.m_work;
        rhs.m_work = nullptr;
    }
    return *this;
}

inline AsyncTask& AsyncTask::operator=(std::nullptr_t) noexcept {
    _decref();
    m_work = nullptr;
    return *this;
}

inline bool AsyncTask::operator==(const AsyncTask& rhs) const noexcept {
    return m_work == rhs.m_work;
}

inline bool AsyncTask::operator!=(const AsyncTask& rhs) const noexcept {
    return m_work != rhs.m_work;
}

inline AsyncTask::operator bool() const noexcept {
    return m_work != nullptr;
}


template <typename ...Deps>
    requires (std::same_as<std::remove_cvref_t<Deps>, AsyncTask> && ...)
inline AsyncTask& AsyncTask::start(Deps&& ...deps) {
    std::array<AsyncTask, sizeof...(Deps)> arr{ std::forward<Deps>(deps)... };
    return start(arr.begin(), arr.end());
}

inline void AsyncTask::stop() noexcept {
    m_work->m_topology->_stop();
}

inline void AsyncTask::wait() noexcept {
    m_work->m_topology->_wait();
}
inline void AsyncTask::get() {
    m_work->m_topology->_wait();
    m_work->_rethrow_exception();
}

/////////////////////////////////////////////////////////////////////////////////////
inline std::size_t AsyncTask::hash_value() const noexcept {
    return std::hash<Work*>{}(m_work);
}

inline std::size_t AsyncTask::use_count() const noexcept {
    return m_work->m_topology->m_use_count.load(std::memory_order_relaxed);
}

inline bool AsyncTask::running() const noexcept {
    return m_work->m_topology->_is_running();
}

inline bool AsyncTask::done() const noexcept {
    return m_work->m_topology->_is_finished();
}

inline TaskType AsyncTask::type() const noexcept {
    return m_work->type();
}

inline std::string AsyncTask::dump(Direction dir) const {
    std::string out;
    out += "direction: ";
    out += to_string(dir);
    out += "\n\n";

    out += m_work->dump();
    out += "\n";
    return out;
}

inline void AsyncTask::dump(std::ostream& os, Direction dir) const {
    os << "direction: " << to_string(dir) << "\n\n";
    m_work->dump(os);
    os << "\n";
}

inline void AsyncTask::reset() noexcept {
    _decref();
    m_work = nullptr;
}

inline bool AsyncTask::valid() const noexcept {
    return m_work != nullptr;
}

inline std::string_view AsyncTask::name() const noexcept {
    return m_work->m_name;
}

inline AsyncTask& AsyncTask::name(const std::string& name) {
    m_work->m_name = name;
    return *this;
}


inline std::size_t AsyncTask::num_acquires() const noexcept {
    return m_work->_num_acquires();
}

inline std::size_t AsyncTask::num_releases() const noexcept {
    return m_work->_num_releases();
}

inline std::size_t AsyncTask::num_observers() const noexcept {
    return m_work->m_observers ? m_work->m_observers->observers.size() : 0;
}


// ==================== 信号量 ====================

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
AsyncTask& AsyncTask::acquire(Ts&&... sems) {
    (m_work->_acquire(&sems), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
AsyncTask& AsyncTask::release(Ts&&... sems) {
    (m_work->_release(&sems), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
AsyncTask& AsyncTask::remove_acquire(Ts&&... sems) noexcept {
    (m_work->_remove_acquire(&sems), ...);
    return *this;
}

template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
AsyncTask& AsyncTask::remove_release(Ts&&... sems) noexcept {
    (m_work->_remove_release(&sems), ...);
    return *this;
}

inline AsyncTask& AsyncTask::clear_acquires() noexcept {
    m_work->_clear_acquires();
    return *this;
}

inline AsyncTask& AsyncTask::clear_releases() noexcept {
    m_work->_clear_releases();
    return *this;
}

// ==================== 遍历 ====================

template <std::invocable<Semaphore&> F>
void AsyncTask::for_each_acquire(F&& visitor)
    noexcept(std::is_nothrow_invocable_v<F, Semaphore&>) {
    for (Semaphore* sem : m_work->_acquires()) {
        std::invoke(visitor, *sem);
    }
}

template <std::invocable<Semaphore&> F>
void AsyncTask::for_each_release(F&& visitor)
    noexcept(std::is_nothrow_invocable_v<F, Semaphore&>) {
    for (Semaphore* sem : m_work->_releases()) {
        std::invoke(visitor, *sem);
    }
}

// ==================== Observer ====================
template <std::derived_from<TaskObserver> Observer, typename... Args>
    requires std::constructible_from<Observer, Args...>
std::shared_ptr<Observer> AsyncTask::register_observer(Args&&... args) {
    auto ptr = std::make_shared<Observer>(std::forward<Args>(args)...);
    if (!m_work->m_observers) {
        m_work->m_observers = std::make_unique<Work::ObserverData>();
    }
    m_work->m_observers->observers.emplace_back(
        std::static_pointer_cast<TaskObserver>(ptr));
    return ptr;
}

// Task::unregister_observer()
template <std::derived_from<TaskObserver> Observer>
void AsyncTask::unregister_observer(std::shared_ptr<Observer> ptr) noexcept {
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


// 标签类型
struct adopt_start_t { explicit adopt_start_t() = default; };
inline constexpr adopt_start_t adopt_start{};

// ============================================================
// AsyncGuard - 单任务，接管所有权
// ============================================================
class [[nodiscard]] AsyncGuard {

    void* operator new(std::size_t) = delete;
    void operator delete(void*) = delete;

public:
    AsyncGuard(AsyncTask task) : m_task(std::move(task)) {
        m_task.start();
    }

    template <typename... Deps>
        requires (sizeof...(Deps) > 0) && (std::same_as<std::remove_cvref_t<Deps>, AsyncTask> && ...)
    AsyncGuard(AsyncTask task, Deps&&... deps) : m_task(std::move(task)) {
        m_task.start(std::forward<Deps>(deps)...);
    }

    template <typename... Deps>
        requires (sizeof...(Deps) > 0) && (std::same_as<std::remove_cvref_t<Deps>, AsyncGuard> && ...)
    AsyncGuard(AsyncTask task, Deps&&... deps) : m_task(std::move(task)) {
        m_task.start(deps.m_task...);
    }

    AsyncGuard(AsyncTask task, adopt_start_t) noexcept : m_task(std::move(task)) {}

    ~AsyncGuard() noexcept {
        if (m_task) m_task.wait();
    }

    AsyncGuard(const AsyncGuard&) = delete;
    AsyncGuard& operator=(const AsyncGuard&) = delete;

    AsyncGuard(AsyncGuard&&) noexcept = default;
    AsyncGuard& operator=(AsyncGuard&& rhs) noexcept {
        if (this != &rhs) {
            if (m_task) m_task.wait();
            m_task = std::move(rhs.m_task);
        }
        return *this;
    }
private:
    AsyncTask m_task;
};

}  // namespace tfl

// ==================== std::hash 特化 ====================
namespace std {

template <>
struct hash<tfl::AsyncTask> {
    std::size_t operator()(const tfl::AsyncTask& task) const noexcept {
        return task.hash_value();
    }
};

}  // namespace std
