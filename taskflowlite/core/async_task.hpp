/// @file async_task.hpp
/// @brief 提供异步任务句柄 AsyncTask。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once
#include <sstream>

#include "work_factory_fwd.hpp"
#include "work.hpp"
#include "runtime.hpp"
#include "executor.hpp"
#include "semaphore.hpp"
#include "topology.hpp"
#include "traits.hpp"
namespace tfl {

// ============================================================
// AsyncTask - 即发即用的轻量级句柄
// ============================================================

/// @brief 异步任务的引用计数式值类型句柄。
///
/// @details
/// `AsyncTask` 是用户与"运行期动态生成的 Work"打交道的唯一安全句柄。
/// 框架的所有动态调度 API
/// （`Executor::lazy_async` / `Executor::async` / `Runtime::lazy_async` / `Runtime::async` 等）
/// 都返回它。其本质是一个 **手动管理引用计数的 Work\* 包装**，引用计数
/// 寄存在所属 `Topology::m_use_count` 上 —— 计数归零即释放节点内存。
///
/// ============================================================================
///  为什么要单独一种句柄？—— 与 `Task` 的本质差异
/// ============================================================================
///
/// | 维度        | `Task`（弱引用）             | `AsyncTask`（强引用）              |
/// |-------------|------------------------------|------------------------------------|
/// | 节点拥有者  | `Flow::m_graph`              | `Topology` 引用计数                |
/// | 适用图风格  | **静态图** —— 用户编辑期持有  | **动态任务** —— 调度期才存在        |
/// | 拷贝代价    | 拷指针                        | 一次原子 fetch_add                 |
/// | 析构代价    | 0                             | 一次 fetch_sub，归零则销毁节点     |
/// | 失效时机    | Flow 析构即悬挂               | 引用计数归零才释放                 |
/// | 等待 / 取异常 | 不支持（节点无独立生命周期）   | `wait()` / `get()` / `stop()`      |
///
/// 这一切的根本原因：**动态任务没有 Flow 兜底生命周期** —— 任务被异步派发出去后，
/// 用户可能在它跑完前已经退出了创建作用域，必须有句柄持有引用计数维持其内存可达。
///
/// ============================================================================
///  Topology 引用计数 —— 内存释放的精确时机
/// ============================================================================
/// 每个 `AsyncTask` 拷贝 / 构造时 `_incref()`，析构 / 重置时 `_decref()`。
/// 计数归零的瞬间在 `_decref()` 内同步调用 `destroy(m_work)`，把节点归还
/// 内存池 —— 这意味着：
///
/// - 节点的析构时机由 **最后一份句柄释放** 决定，与任务是否完成无关；
/// - 任务执行完毕但用户仍持有 AsyncTask 时，节点保留在内存池里，直到句柄被释放；
/// - `done()` 返回 true 之后 `wait()` 仍是合法的（瞬时返回）。
///
/// 这种"句柄式生命周期"是用户态等待 / 取异常 API（`wait`/`get`）能成立的前提。
///
/// ============================================================================
///  状态查询 vs 控制
/// ============================================================================
/// 查询接口（const）：`running()` / `done()` / `has_exception()` / `name()`
///   均查 `Topology` 状态机或 `Work` 的常量字段，**线程安全**（原子读）。
///
/// 控制接口：
/// - `stop()`   —— 设置 topology 停止标志位（**软中断**：节点 invoke 前会自检）；
/// - `wait()`   —— 阻塞直至 topology 进入 Finished 状态，**不重抛异常**；
/// - `get()`    —— `wait() + _rethrow_exception()`，把节点截留的首个异常重抛。
///
/// `wait()` 与 `get()` 的差异
/// 想拿异常一定要走 get / coget，纯 wait 会静默吞掉。
///
template <typename Mode>
class AsyncTask {
    static_assert(task_mode_tag<Mode>, "AsyncTask<Mode>: Mode must satisfy task_mode_tag");

    friend class Runtime;
    friend class Executor;
    template <typename> friend class AsyncTask;
public:
    /// @brief 默认构造空句柄。
    AsyncTask() = default;

    /// @brief 用基本可调用对象构造异步任务。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    explicit AsyncTask(T&& task, Args&&... args);

    /// @brief 用运行时可调用对象构造异步任务(可动态操纵图结构)。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    explicit AsyncTask(T&& task, Args&&... args);

    /// @brief 用任务图构造,执行一次。
    template <typename Gh>
        requires graph_holder<Gh>
    explicit AsyncTask(Gh&& gh);

    /// @brief 任务图执行一次 + 完成回调。
    template <typename Gh, typename C>
        requires (capturable<C> && graph_holder<Gh> && callback<C>)
    explicit AsyncTask(Gh&& gh, C&& cb);

    /// @brief 任务图循环 num 次。
    template <typename Gh>
        requires graph_holder<Gh>
    explicit AsyncTask(Gh&& gh, std::uint64_t num);

    /// @brief 任务图循环 num 次 + 完成回调。
    template <typename Gh, typename C>
        requires (capturable<C> && graph_holder<Gh> && callback<C>)
    explicit AsyncTask(Gh&& gh, std::uint64_t num, C&& cb);

    /// @brief 任务图谓词驱动循环。
    template <typename Gh, typename P>
        requires (capturable<P> && graph_holder<Gh> && predicate<P>)
    explicit AsyncTask(Gh&& gh, P&& pred);

    /// @brief 任务图谓词循环 + 完成回调。
    template <typename Gh, typename P, typename C>
        requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
    explicit AsyncTask(Gh&& gh, P&& pred, C&& cb);

    /// @brief 析构句柄，释放当前持有的任务引用。
    ~AsyncTask();

    /// @brief 构造空句柄（与默认构造等价）。
    explicit AsyncTask(std::nullptr_t) noexcept;

    /// @brief 拷贝构造，增加上游拓扑的引用计数。
    AsyncTask(const AsyncTask& rhs) noexcept;

    /// @brief 拷贝赋值，释放旧引用并增加新引用的计数。
    AsyncTask& operator=(const AsyncTask& rhs) noexcept;

    /// @brief 移动构造，接管所有权，rhs 变为空句柄。
    AsyncTask(AsyncTask&& rhs) noexcept;

    /// @brief 移动赋值，释放旧引用并接管 rhs 的所有权。
    AsyncTask& operator=(AsyncTask&& rhs) noexcept;

    /// @brief 释放当前任务引用，并将句柄置空。
    AsyncTask& operator=(std::nullptr_t) noexcept;

    /// @brief 判断两个句柄是否指向同一个底层任务节点。
    [[nodiscard]] bool operator==(const AsyncTask& rhs) const noexcept;

    /// @brief 判断两个句柄是否指向不同的底层任务节点。
    [[nodiscard]] bool operator!=(const AsyncTask& rhs) const noexcept;

    /// @brief 判断当前句柄是否绑定了有效任务。
    [[nodiscard]] explicit operator bool() const noexcept;

    /// @brief 立即释放当前持有的任务引用，并将句柄置空。
    void reset() noexcept;

    // ==================== 状态查询 ====================

    /// @brief 获取当前句柄的哈希值，基于底层 Work 指针地址。
    /// @note 若句柄为空(m_work == nullptr)，行为未定义。调用前请用 valid() 或 operator bool 检查。
    [[nodiscard]] std::size_t hash_value() const noexcept;

    /// @brief 获取当前异步任务所属 Topology 的引用计数。
    /// @note 若句柄为空(m_work == nullptr)，行为未定义。调用前请用 valid() 或 operator bool 检查。
    [[nodiscard]] std::size_t use_count() const noexcept;

    /// @brief 判断当前句柄是否绑定了底层任务节点。
    [[nodiscard]] bool valid() const noexcept;

    /// @brief 检测任务是否正处于运行或锁定状态。
    /// @note 若句柄为空(m_work == nullptr)，行为未定义。调用前请用 valid() 或 operator bool 检查。
    [[nodiscard]] bool running() const noexcept;

    /// @brief 检测任务是否已经完全执行结束。
    /// @note 若句柄为空(m_work == nullptr)，行为未定义。调用前请用 valid() 或 operator bool 检查。
    [[nodiscard]] bool done() const noexcept;

    /// @brief 获取该异步任务对应的底层节点类型。
    /// @note 若句柄为空(m_work == nullptr)，行为未定义。调用前请用 valid() 或 operator bool 检查。
    [[nodiscard]] TaskType type() const noexcept;

    /// @brief 获取任务名称。
    /// @note 若句柄为空(m_work == nullptr)，行为未定义。调用前请用 valid() 或 operator bool 检查。
    [[nodiscard]] std::string_view name() const noexcept;

    /// @brief 检测任务执行期间是否已经记录异常。
    /// @note 若句柄为空(m_work == nullptr)，行为未定义。调用前请用 valid() 或 operator bool 检查。
    [[nodiscard]] bool has_exception() const noexcept;


    /// @brief 为任务设置易读的名称，用于调试和可视化。
    template <typename S>
        requires std::constructible_from<std::string, S>
    AsyncTask& name(S&& name) &;

    /// @brief 右值限定重载 —— 返回 `AsyncTask` 值以支持链式调用。
    template <typename S>
        requires std::constructible_from<std::string, S>
    AsyncTask name(S&& name) &&;


    // ==================== 可视化 ====================

    /// @brief 将当前异步任务导出为 D2 描述字符串。
    /// @param dir 图布局方向。
    /// @return D2 文本。
    /// @note 若句柄为空(m_work == nullptr)，行为未定义。调用前请用 valid() 或 operator bool 检查。
    [[nodiscard]] std::string dump(Direction dir = Direction::Default) const;

    /// @brief 将当前异步任务的 D2 描述写入输出流。
    /// @param os 输出流。
    /// @param dir 图布局方向。
    /// @note 若句柄为空(m_work == nullptr)，行为未定义。调用前请用 valid() 或 operator bool 检查。
    void dump(std::ostream& os, Direction dir = Direction::Default) const;

    // ==================== 控制接口 ====================

    /// @brief 获取与该异步任务关联的停止令牌。
    /// @note 若句柄为空(m_work == nullptr)，行为未定义。调用前请用 valid() 或 operator bool 检查。
    [[nodiscard]] std::stop_token stop_token() const noexcept;

    /// @brief 检测是否已收到停止请求。
    /// @note 若句柄为空(m_work == nullptr)，行为未定义。调用前请用 valid() 或 operator bool 检查。
    [[nodiscard]] bool stop_requested() const noexcept ;

    /// @brief 检测停止是否可能（即是否有关联的停止状态）。
    /// @note 若句柄为空(m_work == nullptr)，行为未定义。调用前请用 valid() 或 operator bool 检查。
    [[nodiscard]] bool stop_possible() const noexcept;

    /// @brief 向关联的停止源发出停止请求。
    /// @note 若句柄为空(m_work == nullptr)，行为未定义。调用前请用 valid() 或 operator bool 检查。
    bool request_stop() noexcept;

    /// @brief 阻塞当前线程，直到该异步任务完全执行完毕。
    /// @note 若句柄为空(m_work == nullptr)，行为未定义。调用前请用 valid() 或 operator bool 检查。
    void wait() const noexcept;

    /// @brief 同步等待并重新抛出任务执行期间捕获到的首个异常。
    /// @note 若句柄为空(m_work == nullptr)，行为未定义。调用前请用 valid() 或 operator bool 检查。
    void get();



    // ==================== 信号量 ====================

    /// @brief 获取任务执行前需要获取的信号量数量。
    [[nodiscard]] std::size_t num_acquires() const noexcept;

    /// @brief 获取任务执行后需要释放的信号量数量。
    [[nodiscard]] std::size_t num_releases() const noexcept;

    /// @brief 获取任务已注册的观察者数量。
    [[nodiscard]] std::size_t num_observers() const noexcept;

    /// @brief 为任务添加执行前需要获取的信号量，每个信号量默认占用 1 个配额。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask& acquire(Ts&&... sems) &;

    /// @brief 右值限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask acquire(Ts&&... sems) &&;

    /// @brief 为任务添加执行后需要释放的信号量，每个信号量默认释放 1 个配额。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask& release(Ts&&... sems) &;

    /// @brief 右值限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask release(Ts&&... sems) &&;

    /// @brief 为任务添加执行前需要获取的信号量及对应配额。
    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    AsyncTask& acquire(Ts&&... args) &;

    /// @brief 右值限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    AsyncTask acquire(Ts&&... args) &&;

    /// @brief 为任务添加执行后需要释放的信号量及对应配额。
    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    AsyncTask& release(Ts&&... args) &;

    /// @brief 右值限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    AsyncTask release(Ts&&... args) &&;

    /// @brief 移除任务执行前需要获取的指定信号量约束。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask& remove_acquire(Ts&&... sems) & noexcept;

    /// @brief 右值限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask remove_acquire(Ts&&... sems) && noexcept;

    /// @brief 移除任务执行后需要释放的指定信号量约束。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask& remove_release(Ts&&... sems) & noexcept;

    /// @brief 右值限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask remove_release(Ts&&... sems) && noexcept;

    /// @brief 清空所有执行前信号量获取约束。
    AsyncTask& clear_acquires() & noexcept;

    /// @brief 右值限定重载。
    AsyncTask clear_acquires() && noexcept;

    /// @brief 清空所有执行后信号量释放约束。
    AsyncTask& clear_releases() & noexcept;

    /// @brief 右值限定重载。
    AsyncTask clear_releases() && noexcept;


    /// @brief 遍历任务执行前的信号量获取约束。
    template <typename F>
        requires std::invocable<F&, Semaphore&, std::size_t&>
                 || std::invocable<F&, Semaphore&>
    void for_each_acquire(F&& visitor) noexcept(
        std::invocable<F&, Semaphore&, std::size_t&>
            ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
            : std::is_nothrow_invocable_v<F&, Semaphore&>);

    /// @brief const 重载 —— 遍历时提供只读信号量与配额。
    template <typename F>
        requires std::invocable<F&, const Semaphore&, std::size_t>
                 || std::invocable<F&, const Semaphore&>
    void for_each_acquire(F&& visitor) const noexcept(
        std::invocable<F&, const Semaphore&, std::size_t>
            ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
            : std::is_nothrow_invocable_v<F&, const Semaphore&>);

    /// @brief 遍历任务执行后的信号量释放约束。
    template <typename F>
        requires std::invocable<F&, Semaphore&, std::size_t&>
                 || std::invocable<F&, Semaphore&>
    void for_each_release(F&& visitor) noexcept(
        std::invocable<F&, Semaphore&, std::size_t&>
            ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
            : std::is_nothrow_invocable_v<F&, Semaphore&>);

    /// @brief const 重载 —— 遍历时提供只读信号量与配额。
    template <typename F>
        requires std::invocable<F&, const Semaphore&, std::size_t>
                 || std::invocable<F&, const Semaphore&>
    void for_each_release(F&& visitor) const noexcept(
        std::invocable<F&, const Semaphore&, std::size_t>
            ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
            : std::is_nothrow_invocable_v<F&, const Semaphore&>);

    // ==================== 观察者 ====================

    /// @brief 注册一个任务观察者，在任务执行前后接收回调。
    template <std::derived_from<TaskObserver> Observer, typename... Args>
        requires std::constructible_from<Observer, Args...>
    [[nodiscard]] std::shared_ptr<Observer> register_observer(Args&&... args);

    /// @brief 注销指定任务观察者。
    template <std::derived_from<TaskObserver> Observer>
    void unregister_observer(std::shared_ptr<Observer> ptr) noexcept;

protected:
    Work* m_work{nullptr};

    /// @brief 从底层 Work 指针构造句柄，并增加引用计数。
    explicit AsyncTask(Work* work) noexcept;

    /// @brief 增加当前任务所属 Topology 的引用计数。
    void _incref() noexcept;

    /// @brief 减少当前任务所属 Topology 的引用计数；归零时销毁底层任务节点。
    void _decref() noexcept;

};

// ============================================================
// AsyncTask 内联实现
// ============================================================
template <typename Mode>
inline void AsyncTask<Mode>::_incref() noexcept {
    if (m_work) m_work->m_topology->_incref();
}

template <typename Mode>
inline void AsyncTask<Mode>::_decref() noexcept {
    if (m_work && m_work->m_topology->_decref()) {
        destroy(m_work);
    }
}

template <typename Mode>
inline AsyncTask<Mode>::AsyncTask(Work* w) noexcept : m_work{w} {
    _incref();
}

// ============================================================================
//  任务工厂构造 —— 全部委托到 AsyncTask(Work*),自动 _incref
// ============================================================================

template <typename Mode>
template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
inline AsyncTask<Mode>::AsyncTask(T&& task, Args&&... args)
    : AsyncTask{make_attached_basic<anchor::none_t>(
          /*executor=*/ nullptr,
          /*parent=*/nullptr,
          std::forward<T>(task),
          std::forward<Args>(args)...)} {}

template <typename Mode>
template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
inline AsyncTask<Mode>::AsyncTask(T&& task, Args&&... args)
    : AsyncTask{make_attached_runtime<anchor::none_t>(
          /*executor=*/ nullptr,
          /*parent=*/nullptr,
          std::forward<T>(task),
          std::forward<Args>(args)...)} {}

// ---- 任务图族:层叠委托,逐级补默认值 ----

template <typename Mode>
template <typename Gh>
    requires graph_holder<Gh>
inline AsyncTask<Mode>::AsyncTask(Gh&& gh)
    : AsyncTask{std::forward<Gh>(gh), 1ULL} {}

template <typename Mode>
template <typename Gh, typename C>
    requires (capturable<C> && graph_holder<Gh> && callback<C>)
inline AsyncTask<Mode>::AsyncTask(Gh&& gh, C&& cb)
    : AsyncTask{std::forward<Gh>(gh), 1ULL, std::forward<C>(cb)} {}

template <typename Mode>
template <typename Gh>
    requires graph_holder<Gh>
inline AsyncTask<Mode>::AsyncTask(Gh&& gh, std::uint64_t num)
    : AsyncTask{std::forward<Gh>(gh), num, []() noexcept {}} {}

template <typename Mode>
template <typename Gh, typename C>
    requires (capturable<C> && graph_holder<Gh> && callback<C>)
inline AsyncTask<Mode>::AsyncTask(Gh&& gh, std::uint64_t num, C&& cb)
    : AsyncTask{std::forward<Gh>(gh),
                [num, remaining = num]() mutable noexcept -> bool {
                    if constexpr (std::same_as<Mode, task_mode::repeat_t>) {
                        if (remaining-- == 0) [[unlikely]] {
                            remaining = num;
                            return true;
                        }
                        return false;
                    } else {
                        return num-- == 0;
                    }
                },
                std::forward<C>(cb)} {}

template <typename Mode>
template <typename Gh, typename P>
    requires (capturable<P> && graph_holder<Gh> && predicate<P>)
inline AsyncTask<Mode>::AsyncTask(Gh&& gh, P&& pred)
    : AsyncTask{std::forward<Gh>(gh),
                std::forward<P>(pred),
                []() noexcept {}} {}

template <typename Mode>
template <typename Gh, typename P, typename C>
    requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
inline AsyncTask<Mode>::AsyncTask(Gh&& gh, P&& pred, C&& cb)
    : AsyncTask{make_attached_flow<anchor::none_t>(
          /*executor=*/ nullptr,
          /*parent=*/nullptr,
          std::forward<Gh>(gh),
          std::forward<P>(pred),
          std::forward<C>(cb))} {}

template <typename Mode>
inline AsyncTask<Mode>::~AsyncTask() {
    _decref();
}

template <typename Mode>
inline AsyncTask<Mode>::AsyncTask(std::nullptr_t) noexcept : m_work{nullptr} {}

template <typename Mode>
inline AsyncTask<Mode>::AsyncTask(const AsyncTask& rhs) noexcept : m_work{rhs.m_work} {
    _incref();
}

template <typename Mode>
inline AsyncTask<Mode>& AsyncTask<Mode>::operator=(const AsyncTask& rhs) noexcept {
    if (this != std::addressof(rhs)) {
        _decref();
        m_work = rhs.m_work;
        _incref();
    }
    return *this;
}

template <typename Mode>
inline AsyncTask<Mode>::AsyncTask(AsyncTask&& rhs) noexcept
    : m_work{std::exchange(rhs.m_work, nullptr)} {}

template <typename Mode>
inline AsyncTask<Mode>& AsyncTask<Mode>::operator=(AsyncTask&& rhs) noexcept {
    if (this != std::addressof(rhs)) {
        _decref();
        m_work = std::exchange(rhs.m_work, nullptr);
    }
    return *this;
}

template <typename Mode>
inline AsyncTask<Mode>& AsyncTask<Mode>::operator=(std::nullptr_t) noexcept {
    _decref();
    m_work = nullptr;
    return *this;
}

template <typename Mode>
inline bool AsyncTask<Mode>::operator==(const AsyncTask& rhs) const noexcept {
    return m_work == rhs.m_work;
}

template <typename Mode>
inline bool AsyncTask<Mode>::operator!=(const AsyncTask& rhs) const noexcept {
    return m_work != rhs.m_work;
}

template <typename Mode>
inline AsyncTask<Mode>::operator bool() const noexcept {
    return m_work != nullptr;
}

template <typename Mode>
inline void AsyncTask<Mode>::reset() noexcept {
    _decref();
    m_work = nullptr;
}

template <typename Mode>
inline std::size_t AsyncTask<Mode>::hash_value() const noexcept {
    return std::hash<Work*>{}(m_work);
}

template <typename Mode>
inline std::size_t AsyncTask<Mode>::use_count() const noexcept {
    return m_work->m_topology->_use_count();
}

template <typename Mode>
inline bool AsyncTask<Mode>::valid() const noexcept {
    return m_work != nullptr;
}

template <typename Mode>
inline bool AsyncTask<Mode>::running() const noexcept {
    return m_work->m_topology->_is_running();
}

template <typename Mode>
inline bool AsyncTask<Mode>::done() const noexcept {
    return m_work->m_topology->_is_finished();
}

template <typename Mode>
inline TaskType AsyncTask<Mode>::type() const noexcept {
    return m_work->m_type;
}

template <typename Mode>
inline std::string_view AsyncTask<Mode>::name() const noexcept {
    return m_work->m_name;
}

template <typename Mode>
inline bool AsyncTask<Mode>::has_exception() const noexcept {
    return m_work->_has_exception();
}

template <typename Mode>
template <typename S>
    requires std::constructible_from<std::string, S>
inline AsyncTask<Mode>& AsyncTask<Mode>::name(S&& n) & {
    m_work->m_name = std::forward<S>(n);
    return *this;
}

template <typename Mode>
template <typename S>
    requires std::constructible_from<std::string, S>
inline AsyncTask<Mode> AsyncTask<Mode>::name(S&& n) && {
    m_work->m_name = std::forward<S>(n);
    return std::move(*this);
}

template <typename Mode>
inline std::string AsyncTask<Mode>::dump(Direction dir) const {
    std::ostringstream oss;
    dump(oss, dir);
    return std::move(oss).str();
}

template <typename Mode>
inline void AsyncTask<Mode>::dump(std::ostream& os, Direction dir) const {
    os << "direction: " << to_string(dir) << "\n\n";
    m_work->dump(os);
    os << "\n";
}

template <typename Mode>
inline std::stop_token AsyncTask<Mode>::stop_token() const noexcept{
    return m_work->m_topology->m_stop_source.get_token();
}

template <typename Mode>
inline bool AsyncTask<Mode>::stop_requested() const noexcept{
    return m_work->m_topology->m_stop_source.stop_requested();
}

template <typename Mode>
inline bool AsyncTask<Mode>::stop_possible() const noexcept{
    return m_work->m_topology->m_stop_source.stop_possible();
}

template <typename Mode>
inline bool AsyncTask<Mode>::request_stop() noexcept{
    return m_work->m_topology->m_stop_source.request_stop();
}

template <typename Mode>
inline void AsyncTask<Mode>::wait() const noexcept {
    m_work->m_topology->_wait();
}

template <typename Mode>
inline void AsyncTask<Mode>::get() {
    m_work->m_topology->_wait();
    m_work->_rethrow_exception();
}

template <typename Mode>
inline std::size_t AsyncTask<Mode>::num_acquires() const noexcept {
    return m_work->_num_acquires();
}

template <typename Mode>
inline std::size_t AsyncTask<Mode>::num_releases() const noexcept {
    return m_work->_num_releases();
}

template <typename Mode>
inline std::size_t AsyncTask<Mode>::num_observers() const noexcept {
    return m_work->_num_observers();
}

template <typename Mode>
template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline AsyncTask<Mode>& AsyncTask<Mode>::acquire(Ts&&... sems) & {
    (m_work->_acquire(&sems, 1ULL), ...);
    return *this;
}

template <typename Mode>
template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline AsyncTask<Mode> AsyncTask<Mode>::acquire(Ts&&... sems) && {
    (m_work->_acquire(&sems, 1ULL), ...);
    return std::move(*this);
}

template <typename Mode>
template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline AsyncTask<Mode>& AsyncTask<Mode>::release(Ts&&... sems) & {
    (m_work->_release(&sems, 1ULL), ...);
    return *this;
}

template <typename Mode>
template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline AsyncTask<Mode> AsyncTask<Mode>::release(Ts&&... sems) && {
    (m_work->_release(&sems, 1ULL), ...);
    return std::move(*this);
}

template <typename Mode>
template <typename... Ts>
    requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
inline AsyncTask<Mode>& AsyncTask<Mode>::acquire(Ts&&... args) & {
    auto tup = std::forward_as_tuple(std::forward<Ts>(args)...);
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (m_work->_acquire(&std::get<Is * 2>(tup),
                          static_cast<std::size_t>(std::get<Is * 2 + 1>(tup))), ...);
    }(std::make_index_sequence<sizeof...(Ts) / 2>{});
    return *this;
}

template <typename Mode>
template <typename... Ts>
    requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
inline AsyncTask<Mode> AsyncTask<Mode>::acquire(Ts&&... args) && {
    auto tup = std::forward_as_tuple(std::forward<Ts>(args)...);
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (m_work->_acquire(&std::get<Is * 2>(tup),
                          static_cast<std::size_t>(std::get<Is * 2 + 1>(tup))), ...);
    }(std::make_index_sequence<sizeof...(Ts) / 2>{});
    return std::move(*this);
}

template <typename Mode>
template <typename... Ts>
    requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
inline AsyncTask<Mode>& AsyncTask<Mode>::release(Ts&&... args) & {
    auto tup = std::forward_as_tuple(std::forward<Ts>(args)...);
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (m_work->_release(&std::get<Is * 2>(tup),
                          static_cast<std::size_t>(std::get<Is * 2 + 1>(tup))), ...);
    }(std::make_index_sequence<sizeof...(Ts) / 2>{});
    return *this;
}

template <typename Mode>
template <typename... Ts>
    requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
inline AsyncTask<Mode> AsyncTask<Mode>::release(Ts&&... args) && {
    auto tup = std::forward_as_tuple(std::forward<Ts>(args)...);
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (m_work->_release(&std::get<Is * 2>(tup),
                          static_cast<std::size_t>(std::get<Is * 2 + 1>(tup))), ...);
    }(std::make_index_sequence<sizeof...(Ts) / 2>{});
    return std::move(*this);
}

template <typename Mode>
template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline AsyncTask<Mode>& AsyncTask<Mode>::remove_acquire(Ts&&... sems) & noexcept {
    (m_work->_remove_acquire(&sems), ...);
    return *this;
}

template <typename Mode>
template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline AsyncTask<Mode> AsyncTask<Mode>::remove_acquire(Ts&&... sems) && noexcept {
    (m_work->_remove_acquire(&sems), ...);
    return std::move(*this);
}

template <typename Mode>
template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline AsyncTask<Mode>& AsyncTask<Mode>::remove_release(Ts&&... sems) & noexcept {
    (m_work->_remove_release(&sems), ...);
    return *this;
}

template <typename Mode>
template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
inline AsyncTask<Mode> AsyncTask<Mode>::remove_release(Ts&&... sems) && noexcept {
    (m_work->_remove_release(&sems), ...);
    return std::move(*this);
}

template <typename Mode>
inline AsyncTask<Mode>& AsyncTask<Mode>::clear_acquires() & noexcept {
    m_work->_clear_acquires();
    return *this;
}

template <typename Mode>
inline AsyncTask<Mode> AsyncTask<Mode>::clear_acquires() && noexcept {
    m_work->_clear_acquires();
    return std::move(*this);
}

template <typename Mode>
inline AsyncTask<Mode>& AsyncTask<Mode>::clear_releases() & noexcept {
    m_work->_clear_releases();
    return *this;
}

template <typename Mode>
inline AsyncTask<Mode> AsyncTask<Mode>::clear_releases() && noexcept {
    m_work->_clear_releases();
    return std::move(*this);
}


template <typename Mode>
template <typename F>
    requires std::invocable<F&, Semaphore&, std::size_t&>
             || std::invocable<F&, Semaphore&>
inline void AsyncTask<Mode>::for_each_acquire(F&& visitor) noexcept(
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

template <typename Mode>
template <typename F>
    requires std::invocable<F&, const Semaphore&, std::size_t>
             || std::invocable<F&, const Semaphore&>
inline void AsyncTask<Mode>::for_each_acquire(F&& visitor) const noexcept(
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

template <typename Mode>
template <typename F>
    requires std::invocable<F&, Semaphore&, std::size_t&>
             || std::invocable<F&, Semaphore&>
inline void AsyncTask<Mode>::for_each_release(F&& visitor) noexcept(
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

template <typename Mode>
template <typename F>
    requires std::invocable<F&, const Semaphore&, std::size_t>
             || std::invocable<F&, const Semaphore&>
inline void AsyncTask<Mode>::for_each_release(F&& visitor) const noexcept(
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

template <typename Mode>
template <std::derived_from<TaskObserver> Observer, typename... Args>
    requires std::constructible_from<Observer, Args...>
inline std::shared_ptr<Observer> AsyncTask<Mode>::register_observer(Args&&... args) {
    auto ptr = std::make_shared<Observer>(std::forward<Args>(args)...);
    if (!m_work->m_observers) {
        m_work->m_observers = std::make_unique<Work::ObserverData>();
    }
    m_work->m_observers->observers.emplace_back(
        std::static_pointer_cast<TaskObserver>(ptr));
    return ptr;
}

template <typename Mode>
template <std::derived_from<TaskObserver> Observer>
inline void AsyncTask<Mode>::unregister_observer(std::shared_ptr<Observer> ptr) noexcept {
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

/// @brief 将 `AsyncTask<Mode>` 的 D2 描述写入输出流。
template <typename Mode>
inline std::ostream& operator<<(std::ostream& os, const AsyncTask<Mode>& task) {
    task.dump(os);
    return os;
}

template <typename T, typename... Deps>
    requires (any_async_task<T> && (nonrepeat_async_task<Deps> && ...))
inline auto Executor::submit(T&& task, Deps&&... deps)
    -> std::conditional_t<std::is_lvalue_reference_v<T>,  std::remove_reference_t<T>&, std::remove_cvref_t<T>> {
    std::array<AsyncTask<task_mode::nonrepeat_t>, sizeof...(Deps)> arr{
        static_cast<AsyncTask<task_mode::nonrepeat_t>>(std::forward<Deps>(deps))...
    };
    return submit(std::forward<T>(task), arr.begin(), arr.end());
}


template <typename T, std::input_iterator I, std::sentinel_for<I> S>
    requires (any_async_task<T> && nonrepeat_async_task<std::iter_value_t<I>>)
inline auto Executor::submit(T&& task, I first, S last)
    -> std::conditional_t<std::is_lvalue_reference_v<T>, std::remove_reference_t<T>&, std::remove_cvref_t<T>> {
    if (!task.m_work) {
        throw Exception("Executor::submit: empty task.");
    }
    Work* work = task.m_work;
    auto* topo = work->m_topology;

    if constexpr (nonrepeat_async_task<T>) {
        // 单次模式:严格 Idle → Running,字段都是构造期初值(parent=nullptr / implicit=NONE / explicit=ANCHORED),无需写
        auto expected = Topology::State::Idle;
        if (!topo->m_state.compare_exchange_strong(
                expected, Topology::State::Running,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            throw Exception("AsyncTask Error: Task is not in Idle state.");
        }
    } else if constexpr (repeat_async_task<T>) {
        // 可重复模式
        while (true) {
            auto cur = topo->m_state.load(std::memory_order_acquire);
            if (cur == Topology::State::Running) [[unlikely]] {
                throw Exception("AsyncTask Error: Task is already running.");
            }
            if (cur == Topology::State::Locking) [[unlikely]] {
                continue;
            }
            auto expected = cur;
            if (topo->m_state.compare_exchange_weak(
                    expected, Topology::State::Running,
                    std::memory_order_acq_rel, std::memory_order_acquire)) [[likely]] {
                auto old_exp = work->m_explicit.load(std::memory_order_relaxed);
                if (old_exp & Work::Explicit::CAUGHT) [[unlikely]] {
                    work->m_exception_ptr = nullptr;
                }
                if (topo->m_stop_source.stop_requested()) [[unlikely]] {
                    topo->m_stop_source = std::stop_source{};
                }
                break;
            }
        }
    } else {
        // 这里能 grep 到,以后加新 mode 时就来这里加分支
        static_assert(sizeof(T) == 0, "Unhandled task mode");
    }

    work->m_parent   = nullptr;
    work->m_implicit = Work::Implicit::NONE;
    work->m_explicit.store(Work::Explicit::ANCHORED, std::memory_order_relaxed);
    topo->m_executor = this;   // 唯一每次都必须写的(可能换 executor)
    topo->_incref();
    std::size_t num_predecessors = static_cast<std::size_t>(std::ranges::distance(first, last));
    work->m_join_counter.store(num_predecessors, std::memory_order_relaxed);
    _process_dependent(work, first, last, num_predecessors);
    if (num_predecessors == 0) {
        if (Worker* wr = _this_worker(); wr) {
            _schedule(*wr, work);
        } else {
            _schedule(work);
        }
    }
    return std::forward<T>(task);
}

template <anchor_tag A, typename T, typename... Deps>
    requires (any_async_task<T> && (nonrepeat_async_task<Deps> && ...))
inline auto Runtime::submit(T&& task, Deps&&... deps)
    -> std::conditional_t<std::is_lvalue_reference_v<T>,  std::remove_reference_t<T>&, std::remove_cvref_t<T>> {
    std::array<AsyncTask<task_mode::nonrepeat_t>, sizeof...(Deps)> arr{
        static_cast<AsyncTask<task_mode::nonrepeat_t>>(std::forward<Deps>(deps))...
    };
    return submit<A>(std::forward<T>(task), arr.begin(), arr.end());
}

template <anchor_tag A, typename T, std::input_iterator I, std::sentinel_for<I> S>
    requires (any_async_task<T> && nonrepeat_async_task<std::iter_value_t<I>>)
inline auto Runtime::submit(T&& task, I first, S last)
    -> std::conditional_t<std::is_lvalue_reference_v<T>, std::remove_reference_t<T>&, std::remove_cvref_t<T>> {
    if (!task.m_work) {
        throw Exception("Runtime::submit: empty task.");
    }
    Work* work = task.m_work;
    auto* topo = work->m_topology;

    if constexpr (nonrepeat_async_task<T>) {
        auto expected = Topology::State::Idle;
        if (!topo->m_state.compare_exchange_strong(
                expected, Topology::State::Running,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            throw Exception("AsyncTask Error: Task is not in Idle state.");
        }
    } else if constexpr (repeat_async_task<T>) {
        while (true) {
            auto cur = topo->m_state.load(std::memory_order_acquire);
            if (cur == Topology::State::Running) [[unlikely]] {
                throw Exception("AsyncTask Error: Task is already running.");
            }
            if (cur == Topology::State::Locking) [[unlikely]] {
                continue;
            }
            auto expected = cur;
            if (topo->m_state.compare_exchange_weak(
                    expected, Topology::State::Running,
                    std::memory_order_acq_rel, std::memory_order_acquire)) [[likely]] {
                auto old_exp = work->m_explicit.load(std::memory_order_relaxed);
                if (old_exp & Work::Explicit::CAUGHT) [[unlikely]] {
                    work->m_exception_ptr = nullptr;
                }
                if (topo->m_stop_source.stop_requested()) [[unlikely]] {
                    topo->m_stop_source = std::stop_source{};
                }
                break;
            }
        }
    } else {
        // 这里能 grep 到,以后加新 mode 时就来这里加分支
        static_assert(sizeof(T) == 0, "Unhandled task mode");
    }

    // 公共段:Runtime::submit 每次都要重写 parent/implicit/explicit/executor
    work->m_parent   = std::addressof(m_work);
    work->m_implicit = detail::anchor_implicit<A>();
    work->m_explicit.store(detail::anchor_explicit<A>(), std::memory_order_relaxed);
    topo->m_executor = std::addressof(m_executor);
    topo->_incref();
    m_work.m_join_counter.fetch_add(1, std::memory_order_relaxed);
    std::size_t num_predecessors = static_cast<std::size_t>(std::ranges::distance(first, last));
    work->m_join_counter.store(num_predecessors, std::memory_order_relaxed);
    m_executor._process_dependent(work, first, last, num_predecessors);
    if (num_predecessors == 0) {
        m_executor._schedule(m_worker, work);
    }

    return std::forward<T>(task);
}

}  // namespace tfl

// ==================== 标准库扩展 ====================

namespace std {
/// @brief `tfl::AsyncTask<Mode>` 的哈希支持,委托给 `AsyncTask::hash_value()`。
template <typename Mode>
struct hash<tfl::AsyncTask<Mode>> {
    inline std::size_t operator()(const tfl::AsyncTask<Mode>& task) const noexcept {
        return task.hash_value();
    }
};
}  // namespace std
