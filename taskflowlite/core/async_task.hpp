/// @file async_task.hpp
/// @brief 提供异步任务的轻量级句柄 AsyncTask 及其 RAII 管理工具 AsyncGuard。
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include "work.hpp"
#include "semaphore.hpp"
#include "topology.hpp"

namespace tfl {

/// @brief 异步任务的外部操作句柄。
///
/// AsyncTask 是一个轻量级的值类型代理，内部通过引用计数管理顶级任务（Topology）的生命周期。
/// 它允许用户在任务提交后对其进行命名、绑定信号量、注册观察者，并通过 `.start()` 手动触发带依赖的调度。
///
/// @note 该类满足“可拷贝”与“可移动”语义。拷贝一个 AsyncTask 仅增加内部引用计数，不会复制底层任务逻辑。
class AsyncTask {
    friend class Work;
    friend class Graph;
    friend class Flow;
    friend class Runtime;
    friend class Executor;
    friend class Branch;

public:
    /// @brief 构造一个空句柄。
    AsyncTask() noexcept = default;

    /// @brief 析构函数，自动释放一份引用计数。
    ~AsyncTask();

    /// @brief 显式从 nullptr 构造。
    explicit AsyncTask(std::nullptr_t) noexcept;

    AsyncTask(const AsyncTask& rhs) noexcept;
    AsyncTask& operator=(const AsyncTask& rhs) noexcept;
    AsyncTask(AsyncTask&& rhs) noexcept;
    AsyncTask& operator=(AsyncTask&& rhs) noexcept;
    AsyncTask& operator=(std::nullptr_t) noexcept;

    [[nodiscard]] bool operator==(const AsyncTask& rhs) const noexcept;
    [[nodiscard]] bool operator!=(const AsyncTask& rhs) const noexcept;

    /// @brief 立即释放当前持有的任务引用，将句柄置为空。
    void reset() noexcept;

    // ==================== 状态查询 (Getter) ====================

    /// @brief 获取句柄的哈希值（基于底层节点地址）。
    [[nodiscard]] std::size_t hash_value() const noexcept;

    /// @brief 获取当前任务的全局总引用计数。
    [[nodiscard]] std::size_t use_count() const noexcept;

    /// @brief 检测当前句柄是否指向一个有效的任务。
    [[nodiscard]] bool valid() const noexcept;

    /// @brief 获取已注册的 acquire 信号量数量。
    [[nodiscard]] std::size_t num_acquires() const noexcept;

    /// @brief 获取已注册的 release 信号量数量。
    [[nodiscard]] std::size_t num_releases() const noexcept;

    /// @brief 获取当前挂载的观察者数量。
    [[nodiscard]] std::size_t num_observers() const noexcept;

    /// @brief 布尔转换操作符，等价于 valid()。
    [[nodiscard]] explicit operator bool() const noexcept;

    /// @brief 检测任务是否正处于运行或锁定状态。
    [[nodiscard]] bool running() const noexcept;

    /// @brief 检测任务是否已经完全执行结束。
    [[nodiscard]] bool done() const noexcept;

    /// @brief 获取该异步任务对应的底层节点类型。
    [[nodiscard]] TaskType type() const noexcept;

    /// @brief 生成该任务的 D2 可视化字符串。
    [[nodiscard]] std::string dump(Direction dir = Direction::Default) const;

    /// @brief 流式导出该任务的 D2 可视化描述。
    void dump(std::ostream& ostream, Direction dir = Direction::Default) const;

    /// @brief 获取任务名称。
    [[nodiscard]] std::string_view name() const noexcept;

    /// @brief 为任务设置易读的名称，用于调试和可视化。
    AsyncTask& name(const std::string& name);

    // ==================== 信号量管理 ====================

    /// @brief 批量为任务添加执行前的信号量获取约束。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask& acquire(Ts&&... sems);

    /// @brief 批量为任务添加执行后的信号量释放行为。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask& release(Ts&&... sems);

    /// @brief 移除指定的获取约束。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask& remove_acquire(Ts&&... sems) noexcept;

    /// @brief 移除指定的释放行为。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask& remove_release(Ts&&... sems) noexcept;

    /// @brief 清空该任务上绑定的所有 acquire 信号量。
    AsyncTask& clear_acquires() noexcept;

    /// @brief 清空该任务上绑定的所有 release 信号量。
    AsyncTask& clear_releases() noexcept;

    // ==================== 迭代访问 ====================

    /// @brief 遍历所有的获取约束信号量。
    template <std::invocable<Semaphore&> F>
    void for_each_acquire(F&& visitor)
        noexcept(std::is_nothrow_invocable_v<F, Semaphore&>);

    /// @brief 遍历所有的释放行为信号量。
    template <std::invocable<Semaphore&> F>
    void for_each_release(F&& visitor)
        noexcept(std::is_nothrow_invocable_v<F, Semaphore&>);

    // ==================== 观察者通知 ====================

    /// @brief 为该任务动态注册一个新的生命周期观察者。
    /// @tparam Observer 观察者类型，必须派生自 TaskObserver。
    /// @return 返回指向新创建观察者的共享指针。
    template <std::derived_from<TaskObserver> Observer, typename... Args>
        requires std::constructible_from<Observer, Args...>
    [[nodiscard]] std::shared_ptr<Observer> register_observer(Args&&... args);

    /// @brief 卸载并注销指定的观察者。
    template <std::derived_from<TaskObserver> Observer>
    void unregister_observer(std::shared_ptr<Observer> ptr) noexcept;

    // ==================== 核心控制接口 ====================

    /// @brief 手动启动该异步任务，并声明其依赖的前驱任务列表。
    /// @param deps 变长参数包，包含所有必须先于本任务完成的前驱。
    /// @pre 任务必须处于 Idle 状态且未被启动过。
    template <typename ...Deps>
        requires (std::same_as<std::remove_cvref_t<Deps>, AsyncTask> && ...)
    AsyncTask& start(Deps&&... deps);

    /// @brief 手动启动该异步任务，支持通过迭代器范围传递依赖列表。
    template <std::input_iterator I, std::sentinel_for<I> S>
        requires std::same_as<std::iter_value_t<I>, AsyncTask>
    AsyncTask& start(I first, S last);

    /// @brief 请求取消该任务的执行。
    /// @note 仅设置停止标志。若任务尚未运行，则会跳过 invoke 直接进入拆解期。
    void stop() noexcept;

    /// @brief 阻塞当前线程，直到该异步任务完全执行完毕。
    void wait() noexcept;

    /// @brief 同步等待任务结束，并重新抛出任务执行期间捕获到的首个异常。
    void get();

private:
    Work* m_work{nullptr}; ///< 指向内部工作节点的弱引用

    /// @brief 内部构造函数，由工厂方法调用。
    explicit AsyncTask(Work* work) noexcept;

    void _incref() noexcept;
    void _decref() noexcept;
};

// ============================================================
// AsyncTask 内联实现
// ============================================================

inline void AsyncTask::_incref() noexcept {
    // Why: 增加所属拓扑的原子引用计数，确保即便原始 Executor 或 Flow 被销毁，该异步链路依然存活。
    if(m_work) m_work->m_topology->_incref();
}

inline void AsyncTask::_decref() noexcept {
    // Why: 减少引用计数。当最后一份句柄消失且任务已完结时，触发深度销毁流程，回收 Work 与 Topology 的复合内存。
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
        // Why: 先放手旧的，再拥抱新的。
        _decref();
        m_work = rhs.m_work;
        _incref();
    }
    return *this;
}

inline AsyncTask::AsyncTask(AsyncTask&& rhs) noexcept
    : m_work{std::exchange(rhs.m_work, nullptr)} {
    // Why: 移动构造无需修改引用计数，直接窃取指针即可实现 O(1) 的所有权转移。
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
    // Why: 将变参包收纳为静态数组，复用迭代器版本的 start 逻辑，降低模板膨胀风险。
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
    // Why: 阻塞等待后立即尝试解压可能的异常包，模拟 std::future::get 的经典语义。
    m_work->m_topology->_wait();
    m_work->_rethrow_exception();
}

inline std::size_t AsyncTask::hash_value() const noexcept {
    return std::hash<Work*>{}(m_work);
}

inline std::size_t AsyncTask::use_count() const noexcept {
    // Why: 采用 relaxed 序读取。由于 use_count 仅供调试或估计，不需要强同步开销。
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

template <std::derived_from<TaskObserver> Observer, typename... Args>
    requires std::constructible_from<Observer, Args...>
std::shared_ptr<Observer> AsyncTask::register_observer(Args&&... args) {
    auto ptr = std::make_shared<Observer>(std::forward<Args>(args)...);
    if (!m_work->m_observers) {
        m_work->m_observers = std::make_unique<Work::ObserverData>();
    }
    // Why: 将具体观察者向上转型为抽象基类后存储。
    m_work->m_observers->observers.emplace_back(
        std::static_pointer_cast<TaskObserver>(ptr));
    return ptr;
}

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
    if (observers.empty()) {
        m_work->m_observers.reset();
    }
}

/// @brief 内部标签类型，用于构造函数重载分派。
struct adopt_start_t { explicit adopt_start_t() = default; };

/// @brief 指示 AsyncGuard 应当接管一个已经处于启动状态的任务。
inline constexpr adopt_start_t adopt_start{};

// ============================================================
// AsyncGuard - 单任务作用域守护者
// ============================================================

/// @brief 符合 RAII 准则的任务作用域守卫。
///
/// AsyncGuard 强制要求任务在构造时启动，并在析构时同步等待任务结束。
/// 适用于那些必须在当前函数作用域结束前完成的异步逻辑。
class [[nodiscard]] AsyncGuard {
    // Why: 禁止堆分配。AsyncGuard 必须作为局部变量在栈上使用，才能发挥其 RAII 守卫的作用。
    void* operator new(std::size_t) = delete;
    void operator delete(void*) = delete;

public:
    /// @brief 构造并立即启动任务。
    explicit AsyncGuard(AsyncTask task) : m_task(std::move(task)) {
        m_task.start();
    }

    /// @brief 构造、声明依赖并启动任务。
    template <typename... Deps>
        requires (sizeof...(Deps) > 0) && (std::same_as<std::remove_cvref_t<Deps>, AsyncTask> && ...)
    AsyncGuard(AsyncTask task, Deps&&... deps) : m_task(std::move(task)) {
        m_task.start(std::forward<Deps>(deps)...);
    }

    /// @brief 构造、声明对其他守卫任务的依赖并启动。
    template <typename... Deps>
        requires (sizeof...(Deps) > 0) && (std::same_as<std::remove_cvref_t<Deps>, AsyncGuard> && ...)
    AsyncGuard(AsyncTask task, Deps&&... deps) : m_task(std::move(task)) {
        m_task.start(deps.m_task...);
    }

    /// @brief 接管一个已在运行的任务。
    AsyncGuard(AsyncTask task, adopt_start_t) noexcept : m_task(std::move(task)) {}

    /// @brief 析构并阻塞等待任务落盘。
    ~AsyncGuard() noexcept {
        if (m_task) m_task.wait();
    }

    AsyncGuard(const AsyncGuard&) = delete;
    AsyncGuard& operator=(const AsyncGuard&) = delete;

    AsyncGuard(AsyncGuard&&) noexcept = default;

    AsyncGuard& operator=(AsyncGuard&& rhs) noexcept {
        if (this != &rhs) {
            // Why: 若当前守卫已持有任务，必须先履行同步等待的承诺，再进行接管。
            if (m_task) m_task.wait();
            m_task = std::move(rhs.m_task);
        }
        return *this;
    }
private:
    AsyncTask m_task;
};

}  // namespace tfl

// ==================== 标准库扩展 ====================

namespace std {
/// @brief 为 AsyncTask 提供 std::hash 支持，以便将其存入无序容器。
template <>
struct hash<tfl::AsyncTask> {
    std::size_t operator()(const tfl::AsyncTask& task) const noexcept {
        return task.hash_value();
    }
};
}  // namespace std
