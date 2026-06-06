/// @file  async_task.hpp
/// @brief 异步任务句柄 AsyncTask —— 动态 Work 节点的引用计数式值类型包装。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
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
// AsyncTask —— 即发即用的轻量级句柄
// ============================================================

/// @brief 异步任务的引用计数式值类型句柄。
///
/// AsyncTask 是用户与运行期动态生成的 Work 节点打交道的安全句柄。本质是
/// 手动管理引用计数的 Work* 包装，引用计数归零即释放节点内存。与 Task
/// （Flow 内弱引用）不同，AsyncTask 是强引用句柄：动态任务没有 Flow 兜底
/// 生命周期，任务派发后用户可能退出作用域，因此句柄必须持有引用计数。
///
/// @tparam Mode 任务模式 tag：nonrepeat_t（单次，Idle→Running→Finished）
///              或 repeat_t（可重复，Finished→Running 循环复用）。
///
/// @note 非空句柄的所有查询方法线程安全（原子读）；空句柄上调用行为未定义。
template <typename Mode>
class AsyncTask {
    static_assert(task_mode_tag<Mode>, "AsyncTask<Mode>: Mode must satisfy task_mode_tag");

    friend class Runtime;
    friend class Executor;
    template <typename> friend class AsyncTask;
public:
    // ========================================================================
    //  构造与析构
    // ========================================================================

    /// @brief 默认构造空句柄（m_work == nullptr）。
    AsyncTask() = default;

    /// @brief 用基本可调用对象构造异步任务。
    ///
    /// 委托到 make_attached_basic 工厂，创建带有 Topology + Work 的完整任务，
    /// 自动 _incref() 设定初始引用计数为 1。
    ///
    /// @tparam T    满足 basic_invocable concept 的任务体。
    /// @tparam Args 任务参数类型包。
    /// @param task  可调用对象。
    /// @param args  任务参数。
    template <typename T, typename... Args>
        requires (!any_async_task<std::remove_cvref_t<T>> && capturable<T, Args...> && basic_invocable<T, Args...>)
    explicit AsyncTask(T&& task, Args&&... args);

    /// @brief 用运行时可调用对象构造异步任务（可通过 Runtime 动态操纵图结构）。
    ///
    /// @tparam T    满足 runtime_invocable concept 的任务体。
    /// @tparam Args 任务参数类型包。
    /// @param task  可调用对象（签名为 void(Runtime&, Args...) 或其返回版）。
    /// @param args  任务参数。
    template <typename T, typename... Args>
        requires (!any_async_task<std::remove_cvref_t<T>> && capturable<T, Args...> && runtime_invocable<T, Args...>)
    explicit AsyncTask(T&& task, Args&&... args);

    /// @brief 用任务图构造，执行一次。
    /// @tparam Gh 满足 graph_holder concept 的图持有者。
    /// @param gh  任务图持有者。
    template <typename Gh>
        requires graph_holder<Gh>
    explicit AsyncTask(Gh&& gh);

    /// @brief 任务图执行一次 + 完成回调。
    /// @tparam Gh 满足 graph_holder concept。
    /// @tparam C  满足 callback concept。
    /// @param gh  任务图持有者。
    /// @param cb  完成回调。
    template <typename Gh, typename C>
        requires (capturable<C> && graph_holder<Gh> && callback<C>)
    explicit AsyncTask(Gh&& gh, C&& cb);

    /// @brief 任务图循环 num 次。
    /// @tparam Gh  满足 graph_holder concept。
    /// @param gh   任务图持有者。
    /// @param num  循环次数。
    template <typename Gh>
        requires graph_holder<Gh>
    explicit AsyncTask(Gh&& gh, std::uint64_t num);

    /// @brief 任务图循环 num 次 + 完成回调。
    /// @tparam Gh  满足 graph_holder concept。
    /// @tparam C   满足 callback concept。
    /// @param gh   任务图持有者。
    /// @param num  循环次数。
    /// @param cb   完成回调。
    template <typename Gh, typename C>
        requires (capturable<C> && graph_holder<Gh> && callback<C>)
    explicit AsyncTask(Gh&& gh, std::uint64_t num, C&& cb);

    /// @brief 任务图谓词驱动循环：pred 返回 true 时停止。
    /// @tparam Gh   满足 graph_holder concept。
    /// @tparam P    满足 predicate concept。
    /// @param gh    任务图持有者。
    /// @param pred  循环谓词。
    template <typename Gh, typename P>
        requires (capturable<P> && graph_holder<Gh> && predicate<P>)
    explicit AsyncTask(Gh&& gh, P&& pred);

    /// @brief 任务图谓词循环 + 完成回调。
    /// @tparam Gh   满足 graph_holder concept。
    /// @tparam P    满足 predicate concept。
    /// @tparam C    满足 callback concept。
    /// @param gh    任务图持有者。
    /// @param pred  循环谓词。
    /// @param cb    完成回调。
    template <typename Gh, typename P, typename C>
        requires (capturable<P, C> && graph_holder<Gh> && predicate<P> && callback<C>)
    explicit AsyncTask(Gh&& gh, P&& pred, C&& cb);

    /// @brief 析构句柄，释放当前持有的任务引用。
    ///
    /// 调用 _decref()：若引用计数归零则同步销毁 Work 节点。
    ~AsyncTask();

    /// @brief 构造空句柄（与默认构造等价）。
    explicit AsyncTask(std::nullptr_t) noexcept;

    /// @brief 拷贝构造，增加上游 Topology 的引用计数（原子 fetch_add）。
    AsyncTask(const AsyncTask& rhs) noexcept;

    /// @brief 拷贝赋值，释放旧引用并增加新引用的计数。
    ///
    /// 自赋值安全（this != &rhs 检查）。
    AsyncTask& operator=(const AsyncTask& rhs) noexcept;

    /// @brief 移动构造，接管所有权，rhs 变为空句柄。
    AsyncTask(AsyncTask&& rhs) noexcept;

    /// @brief 移动赋值，释放旧引用并接管 rhs 的所有权。
    AsyncTask& operator=(AsyncTask&& rhs) noexcept;

    /// @brief 释放当前任务引用，并将句柄置空。
    AsyncTask& operator=(std::nullptr_t) noexcept;

    /// @brief 判断两个句柄是否指向同一个底层任务节点（Work* 地址比较）。
    [[nodiscard]] bool operator==(const AsyncTask& rhs) const noexcept;

    /// @brief 判断两个句柄是否指向不同的底层任务节点。
    [[nodiscard]] bool operator!=(const AsyncTask& rhs) const noexcept;

    /// @brief 判断当前句柄是否绑定了有效任务（m_work != nullptr）。
    [[nodiscard]] explicit operator bool() const noexcept;

    /// @brief 立即释放当前持有的任务引用，并将句柄置空。等价于 operator=(nullptr)。
    void reset() noexcept;

    // ==================== 状态查询 ====================

    /// @brief 获取当前句柄的哈希值，基于底层 Work* 地址。
    ///
    /// @note 若句柄为空（m_work == nullptr），行为未定义。
    ///       调用前请用 valid() 或 operator bool 检查。
    [[nodiscard]] std::size_t hash_value() const noexcept;

    /// @brief 获取当前异步任务所属 Topology 的引用计数。
    ///
    /// 反映当前有多少个 AsyncTask 句柄（及内部引用）共享同一 Topology。
    ///
    /// @note 若句柄为空（m_work == nullptr），行为未定义。
    [[nodiscard]] std::size_t use_count() const noexcept;

    /// @brief 判断当前句柄是否绑定了底层任务节点。
    ///
    /// 等价于 operator bool()，但以命名函数形式提供。
    [[nodiscard]] bool valid() const noexcept;

    /// @brief 检测任务是否正处于 Running 或 Locking 状态。
    ///
    /// 底层查询 Topology::_is_running()，返回 Topology::State 非 Idle 且
    /// 非 Finished。
    ///
    /// @note 若句柄为空（m_work == nullptr），行为未定义。
    [[nodiscard]] bool running() const noexcept;

    /// @brief 检测任务是否已完全执行结束（Topology::State == Finished）。
    ///
    /// 一旦返回 true，wait() 将瞬时返回，get() 可安全调用。
    ///
    /// @note 若句柄为空（m_work == nullptr），行为未定义。
    [[nodiscard]] bool done() const noexcept;

    /// @brief 获取底层 Work 节点的运行时类型标签，用于区分控制流节点与普通任务。
    /// @return TaskType 枚举值（None / Basic / Runtime / Graph / Branch / MultiBranch / Jump / MultiJump 等）。
    /// @note 若句柄为空（m_work == nullptr），行为未定义。
    [[nodiscard]] TaskType type() const noexcept;

    /// @brief 获取任务名称（string_view，零拷贝）。
    ///
    /// 名称在构造时由工厂设置，可通过 name() 修改。
    ///
    /// @note 若句柄为空（m_work == nullptr），行为未定义。
    [[nodiscard]] std::string_view name() const noexcept;

    /// @brief 检测任务执行期间是否已记录异常。
    ///
    /// 查询 Work::m_explicit 中的 EXCEPTION 标志位（relaxed 读）。
    ///
    /// @note 若句柄为空（m_work == nullptr），行为未定义。
    [[nodiscard]] bool has_exception() const noexcept;


    /// @brief 为任务设置易读的名称（lvalue 限定），用于调试和可视化。
    ///
    /// @tparam S 可构造 std::string 的类型。
    /// @param n  新名称。
    /// @return   *this 引用，支持链式调用。
    template <typename S>
        requires std::constructible_from<std::string, S>
    AsyncTask& name(S&& name) &;

    /// @brief 为任务设置易读的名称（rvalue 限定），返回 AsyncTask 值以支持链式。
    ///
    /// @tparam S 可构造 std::string 的类型。
    /// @param n  新名称。
    /// @return   移动后的 AsyncTask 值。
    template <typename S>
        requires std::constructible_from<std::string, S>
    AsyncTask name(S&& name) &&;


    // ==================== 可视化 ====================

    /// @brief 将当前异步任务导出为 D2 描述字符串。
    ///
    /// @param dir 图布局方向（Direction::TopToBottom 等）。
    /// @return    D2 文本。
    /// @note 若句柄为空（m_work == nullptr），行为未定义。
    [[nodiscard]] std::string dump(Direction dir = Direction::Default) const;

    /// @brief 将当前异步任务的 D2 描述写入输出流。
    ///
    /// @param os  输出流。
    /// @param dir 图布局方向。
    /// @note 若句柄为空（m_work == nullptr），行为未定义。
    void dump(std::ostream& os, Direction dir = Direction::Default) const;

    // ==================== 控制接口 ====================

    /// @brief 获取与该异步任务关联的 stop_token。
    ///
    /// 可用于在任务体内部（通过 Runtime 传入）轮询 stop_requested()。
    ///
    /// @note 若句柄为空（m_work == nullptr），行为未定义。
    [[nodiscard]] std::stop_token stop_token() const noexcept;

    /// @brief 检测是否已收到停止请求。
    ///
    /// 底层查询 topology->m_stop_source.stop_requested()。
    ///
    /// @note 若句柄为空（m_work == nullptr），行为未定义。
    [[nodiscard]] bool stop_requested() const noexcept ;

    /// @brief 检测是否可能向该任务发送停止请求。
    /// @return true 若关联的 stop_source 拥有有效的 stop_state（构造时即建立，从不失效）。
    /// @note 若句柄为空（m_work == nullptr），行为未定义。
    [[nodiscard]] bool stop_possible() const noexcept;

    /// @brief 向关联的 stop_source 发出停止请求（软中断）。
    ///
    /// 设置了 stop 标志后，任务节点在 invoke 前会自检并跳过执行。
    /// 但不会强制中止正在执行中的节点。
    ///
    /// @return true 若停止请求已被发出，false 若已有人请求过。
    /// @note 若句柄为空（m_work == nullptr），行为未定义。
    bool request_stop() noexcept;

    /// @brief 阻塞当前线程，直到该异步任务完全执行完毕。
    ///
    /// 底层调用 topology->_wait()，通过原子 wait 原语挂起直到
    /// Topology::State == Finished。
    ///
    /// @warning 不重抛异常。若任务执行期间抛了异常，wait() 静默返回。
    ///          需要获取异常请使用 get()。
    /// @note 若句柄为空（m_work == nullptr），行为未定义。
    void wait() const noexcept;

    /// @brief 同步等待并重新抛出任务执行期间捕获到的首个异常。
    ///
    /// 等价于 wait() + _rethrow_exception()。若任务未抛异常，则正常返回。
    ///
    /// @throw 任务执行期间抛出的任何异常（捕获并归档在 Work::m_exception_ptr 中）。
    /// @note 若句柄为空（m_work == nullptr），行为未定义。
    void get();



    // ==================== 信号量 ====================

    /// @brief 获取任务执行前需要获取的信号量数量。
    /// @return 通过 acquire() 注册的信号量约束个数。
    [[nodiscard]] std::size_t num_acquires() const noexcept;

    /// @brief 获取任务执行后需要释放的信号量数量。
    /// @return 通过 release() 注册的信号量约束个数。
    [[nodiscard]] std::size_t num_releases() const noexcept;

    /// @brief 获取任务已注册的生命周期观察者数量。
    /// @return 通过 register_observer() 注册的 TaskObserver 个数。
    [[nodiscard]] std::size_t num_observers() const noexcept;

    /// @brief 为任务添加执行前需要获取的信号量（lvalue 限定，每个默认 1 配额）。
    ///
    /// @tparam Ts 均为 Semaphore 类型。
    /// @param sems 一个或多个信号量引用。
    /// @return     *this 引用，支持链式。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask& acquire(Ts&&... sems) &;

    /// @brief acquire 的 rvalue 限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask acquire(Ts&&... sems) &&;

    /// @brief 为任务添加执行后需要释放的信号量（lvalue 限定，每个默认 1 配额）。
    ///
    /// @tparam Ts 均为 Semaphore 类型。
    /// @param sems 一个或多个信号量引用。
    /// @return     *this 引用，支持链式。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask& release(Ts&&... sems) &;

    /// @brief release 的 rvalue 限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask release(Ts&&... sems) &&;

    /// @brief 为任务添加执行前需要获取的信号量及对应配额。
    ///
    /// @tparam Ts 交替格式：(Semaphore, count) 对。参数个数必须 >= 2 且为偶数。
    /// @param args 交替的信号量引用和整数配额。
    /// @return     *this 引用，支持链式。
    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    AsyncTask& acquire(Ts&&... args) &;

    /// @brief acquire(Sem,count) 的 rvalue 限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    AsyncTask acquire(Ts&&... args) &&;

    /// @brief 为任务添加执行后需要释放的信号量及对应配额。
    ///
    /// @tparam Ts 交替格式：(Semaphore, count) 对。
    /// @param args 交替的信号量引用和整数配额。
    /// @return     *this 引用，支持链式。
    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    AsyncTask& release(Ts&&... args) &;

    /// @brief release(Sem,count) 的 rvalue 限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) >= 2) && (sizeof...(Ts) % 2 == 0) && sem_count_sequence<Ts...>
    AsyncTask release(Ts&&... args) &&;

    /// @brief 移除任务执行前需要获取的指定信号量约束（lvalue 限定）。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask& remove_acquire(Ts&&... sems) & noexcept;

    /// @brief remove_acquire 的 rvalue 限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask remove_acquire(Ts&&... sems) && noexcept;

    /// @brief 移除任务执行后需要释放的指定信号量约束（lvalue 限定）。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask& remove_release(Ts&&... sems) & noexcept;

    /// @brief remove_release 的 rvalue 限定重载。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<std::remove_cvref_t<Ts>, Semaphore> && ...)
    AsyncTask remove_release(Ts&&... sems) && noexcept;

    /// @brief 清空所有执行前信号量获取约束（lvalue 限定）。
    AsyncTask& clear_acquires() & noexcept;

    /// @brief clear_acquires 的 rvalue 限定重载。
    AsyncTask clear_acquires() && noexcept;

    /// @brief 清空所有执行后信号量释放约束（lvalue 限定）。
    AsyncTask& clear_releases() & noexcept;

    /// @brief clear_releases 的 rvalue 限定重载。
    AsyncTask clear_releases() && noexcept;


    /// @brief 遍历任务执行前的信号量获取约束（mutable 版）。
    ///
    /// @tparam F 可调用对象，签名为 void(Semaphore&, size_t&) 或 void(Semaphore&)。
    /// @param visitor 遍历回调。
    template <typename F>
        requires std::invocable<F&, Semaphore&, std::size_t&>
                 || std::invocable<F&, Semaphore&>
    void for_each_acquire(F&& visitor) noexcept(
        std::invocable<F&, Semaphore&, std::size_t&>
            ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
            : std::is_nothrow_invocable_v<F&, Semaphore&>);

    /// @brief 遍历任务执行前的信号量获取约束（const 版）。
    ///
    /// @tparam F 可调用对象，签名为 void(const Semaphore&, size_t) 或
    ///           void(const Semaphore&)。
    /// @param visitor 遍历回调。
    template <typename F>
        requires std::invocable<F&, const Semaphore&, std::size_t>
                 || std::invocable<F&, const Semaphore&>
    void for_each_acquire(F&& visitor) const noexcept(
        std::invocable<F&, const Semaphore&, std::size_t>
            ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
            : std::is_nothrow_invocable_v<F&, const Semaphore&>);

    /// @brief 遍历任务执行后的信号量释放约束（mutable 版）。
    ///
    /// @tparam F 可调用对象，签名为 void(Semaphore&, size_t&) 或 void(Semaphore&)。
    /// @param visitor 遍历回调。
    template <typename F>
        requires std::invocable<F&, Semaphore&, std::size_t&>
                 || std::invocable<F&, Semaphore&>
    void for_each_release(F&& visitor) noexcept(
        std::invocable<F&, Semaphore&, std::size_t&>
            ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
            : std::is_nothrow_invocable_v<F&, Semaphore&>);

    /// @brief 遍历任务执行后的信号量释放约束（const 版）。
    ///
    /// @tparam F 可调用对象，签名为 void(const Semaphore&, size_t) 或
    ///           void(const Semaphore&)。
    /// @param visitor 遍历回调。
    template <typename F>
        requires std::invocable<F&, const Semaphore&, std::size_t>
                 || std::invocable<F&, const Semaphore&>
    void for_each_release(F&& visitor) const noexcept(
        std::invocable<F&, const Semaphore&, std::size_t>
            ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
            : std::is_nothrow_invocable_v<F&, const Semaphore&>);

    // ==================== 观察者 ====================

    /// @brief 注册一个任务观察者，在任务执行前后接收回调。
    ///
    /// @tparam Observer std::derived_from<TaskObserver> 的具体类型。
    /// @tparam Args     构造 Observer 的参数类型。
    /// @param args      转发给 Observer 构造函数的参数。
    /// @return          指向已注册观察者的 shared_ptr，可用于后续注销。
    template <std::derived_from<TaskObserver> Observer, typename... Args>
        requires std::constructible_from<Observer, Args...>
    [[nodiscard]] std::shared_ptr<Observer> register_observer(Args&&... args);

    /// @brief 注销指定任务观察者。
    ///
    /// @tparam Observer std::derived_from<TaskObserver> 的具体类型。
    /// @param ptr       由 register_observer 返回的 shared_ptr。
    template <std::derived_from<TaskObserver> Observer>
    void unregister_observer(std::shared_ptr<Observer> ptr) noexcept;

protected:
    Work* m_work{nullptr};  ///< 底层 Work 节点指针（空句柄时为 nullptr）

    /// @brief 从底层 Work* 构造句柄，并增加引用计数（供工厂函数和 friend 使用）。
    ///
    /// @param work 指向已构造 Work 节点的指针。
    explicit AsyncTask(Work* work) noexcept;

    /// @brief 增加当前任务所属 Topology 的引用计数（原子操作）。
    void _incref() noexcept;

    /// @brief 减少当前任务所属 Topology 的引用计数；归零时销毁底层任务节点。
    ///
    /// 调用 topology->_decref()，若返回 true 则同步调用 destroy(m_work)。
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
//  任务工厂构造 —— 全部委托到 AsyncTask(Work*)，自动 _incref
// ============================================================================

template <typename Mode>
template <typename T, typename... Args>
    requires (!any_async_task<std::remove_cvref_t<T>> && capturable<T, Args...> && basic_invocable<T, Args...>)
inline AsyncTask<Mode>::AsyncTask(T&& task, Args&&... args)
    : AsyncTask{make_attached_basic<anchor::none_t>(
          /*executor=*/ nullptr,
          /*parent=*/nullptr,
          std::forward<T>(task),
          std::forward<Args>(args)...)} {}

template <typename Mode>
template <typename T, typename... Args>
    requires (!any_async_task<std::remove_cvref_t<T>> && capturable<T, Args...> && runtime_invocable<T, Args...>)
inline AsyncTask<Mode>::AsyncTask(T&& task, Args&&... args)
    : AsyncTask{make_attached_runtime<anchor::none_t>(
          /*executor=*/ nullptr,
          /*parent=*/nullptr,
          std::forward<T>(task),
          std::forward<Args>(args)...)} {}

// 任务图族：层叠委托，逐级补默认值

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
                    if constexpr (std::same_as<Mode, task_mode::nonrepeat_t>) {
                        // nonrepeat_t: 单次递减，归零时停止
                        return num-- == 0;
                    } else if constexpr (std::same_as<Mode, task_mode::repeat_t>) {
                        // repeat_t: 每轮重置 remaining，循环复用
                        if (remaining-- == 0) [[unlikely]] {
                            remaining = num;
                            return true;
                        }
                        return false;
                    } else {
                        static_assert(sizeof(Mode) == 0, "Unhandled task mode.");
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
    if (!m_work) {
        return;
    }
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

/// @brief 将 AsyncTask<Mode> 的 D2 描述写入输出流（自由函数）。
/// @relates AsyncTask
template <typename Mode>
inline std::ostream& operator<<(std::ostream& os, const AsyncTask<Mode>& task) {
    task.dump(os);
    return os;
}

// ============================================================================
//  Executor::submit 与 Runtime::submit 实现
//  —— 由于依赖 AsyncTask 定义，必须放在此文件中
// ============================================================================

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
        // nonrepeat: 严格 Idle → Running CAS 原子状态转移
        auto expected = Topology::State::Idle;
        if (!topo->m_state.compare_exchange_strong(
                expected, Topology::State::Running,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            throw Exception("AsyncTask Error: Task is not in Idle state.");
        }
    } else if constexpr (repeat_async_task<T>) {
        // repeat: 允许 Finished → Running 转换，但禁止 Running 并发提交
        for (;;) {
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
                // 清理上一轮的异常标志和 stop_source
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
        // 未来添加新 Mode 时的编译期提示
        static_assert(sizeof(T) == 0, "Unhandled task mode.");
    }

    // 公共段：每次 submit 都要重写 parent / implicit / explicit / executor
    work->m_parent   = nullptr;
    work->m_implicit = Work::Implicit::NONE;
    work->m_explicit.store(Work::Explicit::ANCHORED, std::memory_order_relaxed);
    topo->m_executor = this;   // 唯一每次都必须写的（可能换 executor）
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
        for (;;) {
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
        // 未来添加新 Mode 时的编译期提示
        static_assert(sizeof(T) == 0, "Unhandled task mode.");
    }

    // 公共段：Runtime::submit 每次都要重写 parent / implicit / explicit / executor
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
/// @brief tfl::AsyncTask<Mode> 的 std::hash 特化，委托给 AsyncTask::hash_value()。
///
/// @tparam Mode 任务模式 tag（nonrepeat_t 或 repeat_t）。
template <typename Mode>
struct hash<tfl::AsyncTask<Mode>> {
    inline std::size_t operator()(const tfl::AsyncTask<Mode>& task) const noexcept {
        return task.hash_value();
    }
};
}  // namespace std
