/// @file async_task.hpp
/// @brief AsyncTask —— 单次启动异步任务的配置、共享生命周期、依赖启动与结果访问句柄。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <array>
#include <cstdint>
#include <utility>

#include "runtime.hpp"
#include "executor.hpp"
#include "semaphore.hpp"
#include "topology.hpp"
#include "traits.hpp"
#include "async_future.hpp"
#include "work_factory_fwd.hpp"
namespace tfl {

// ============================================================================
// AsyncTask 定义
// ============================================================================

/// @brief 表示至多成功启动一次、由强引用共享生命周期的异步任务句柄。
///
/// 每个非空 AsyncTask 关联同一个底层 `Work`、其所属 `Topology` 以及对应的
/// `ResultSlot<R>`。句柄通过基类 `AsyncFuture<R>` 持有 Work 强引用；复制只增加
/// 强引用并共享任务状态，移动则转移该引用并使源句柄变为空。
///
/// 新建任务处于未启动状态。任一共享句柄成功将 Topology 从 Idle 切换为 Running 后，
/// 该任务即被永久视为已经启动，之后通过任意副本再次启动都会失败。
///
/// 本类负责启动前的任务配置以及框架内部启动；等待、结果访问、状态查询、异常传播
/// 和协作式停止控制由 `AsyncFuture<R>` 提供。
///
/// @tparam R callable 的结果类型，可为值类型、左值引用类型或 void。
/// @warning 除基类明确支持的并发状态查询、等待与结果访问外，任务名称、信号量和
///          观察者等配置接口不得与任务启动、执行或其他配置修改并发调用。
template <typename R>
class AsyncTask final : public AsyncFuture<R> {

    friend class Runtime;
    friend class SubFlow;
    friend class Executor;
    friend class TaskGroup;
    template <typename> friend class AsyncTask;

    using Base = AsyncFuture<R>;
public:

    /// @brief 构造不关联任何 Work 和结果槽的空 AsyncTask。
    AsyncTask() noexcept = default;

    /// @brief 复制句柄并共享同一个 Work 和结果槽；源句柄允许为空。
    ///
    /// 非空复制通过基类增加底层 Work 的强引用计数，不复制任务本身。
    AsyncTask(const AsyncTask&) noexcept = default;

    /// @brief 释放当前 Work 强引用后，共享另一句柄关联的任务状态。
    /// @return `*this`；源句柄允许为空或与当前对象相同。
    AsyncTask& operator=(const AsyncTask&) noexcept = default;

    /// @brief 移动句柄并接管其 Work 强引用和结果槽指针；源句柄变为空。
    ///
    /// 移动过程不增加底层 Work 的强引用计数。
    AsyncTask(AsyncTask&&) noexcept = default;

    /// @brief 释放当前 Work 强引用并接管另一句柄的关联状态，源句柄随后变为空。
    /// @return `*this`。
    AsyncTask& operator=(AsyncTask&&) noexcept = default;

    /// @brief 释放当前句柄持有的 Work 强引用。
    ///
    /// 是否销毁 Work 由基类引用计数逻辑决定；只有总强引用计数归零时才实际销毁。
    ~AsyncTask() = default;

    /// @brief 释放当前 Work 强引用并把句柄置空。
    /// @return `*this`，用于链式操作。
    AsyncTask& operator=(std::nullptr_t) noexcept;

    /// @brief 显式构造不关联任何任务的空 AsyncTask。
    explicit AsyncTask(std::nullptr_t) noexcept;

    /// @brief 创建尚未启动的普通 callable 异步任务。
    /// @tparam T 满足 `basic_invocable` concept 的 callable 类型。
    /// @param task 要按 `capturable` 规则保存的 callable。
    /// @throws std::bad_alloc 创建 Work、Topology、结果槽或 callable 存储失败。
    /// @note 构造完成后任务仍处于 Idle 状态，不会立即提交执行。
    template <typename T>
        requires (basic_invocable<T> && capturable<T> && std::same_as<R, basic_return_t<T>>)
    explicit AsyncTask(T&& task);

    /// @brief 创建尚未启动、执行时由框架注入 `Runtime&` 的异步任务。
    /// @tparam T 满足 `runtime_invocable` concept 的 callable 类型。
    /// @param task 要按 `capturable` 规则保存的 callable。
    /// @throws std::bad_alloc 创建 Work、Topology、结果槽或 callable 存储失败。
    /// @note 构造完成后任务仍处于 Idle 状态，不会立即提交执行。
    template <typename T>
        requires (runtime_invocable<T> && capturable<T> && std::same_as<R, runtime_return_t<T>>)
    explicit AsyncTask(T&& task);

    /// @brief 创建尚未启动、执行时由框架注入 `SubFlow&` 的异步任务。
    /// @tparam T 满足 `subflow_invocable` concept 的 callable 类型。
    /// @param task 要按 `capturable` 规则保存的 callable。
    /// @throws std::bad_alloc 创建 Work、Topology、结果槽或 callable 存储失败。
    /// @note 构造完成后任务仍处于 Idle 状态，不会立即提交执行。
    /// @warning callable 不得保存框架注入的 `SubFlow&`，该引用仅在本次调用期间有效。
    template <typename T>
        requires (subflow_invocable<T> && capturable<T> && std::same_as<R, subflow_return_t<T>>)
    explicit AsyncTask(T&& task);

    /// @brief 创建尚未启动、执行一次子图的 void 异步任务。
    /// @tparam Gh 满足 `graph_holder` concept 的子图持有者类型。
    /// @tparam C 满足 `callback` concept 的完成回调类型。
    /// @param gh 按 graph_holder 规则捕获或借用的子图。
    /// @param callback 子图执行结束后调用的无参完成回调。
    /// @note 该重载等价于循环次数为 1 的子图任务。
    template <graph_holder Gh, callback C = noop_callback>
        requires (capturable<C> && std::same_as<R, void>)
    explicit AsyncTask(Gh&& gh, C&& callback = C{});

    /// @brief 创建尚未启动、循环执行子图指定次数的 void 异步任务。
    /// @tparam Gh 满足 `graph_holder` concept 的子图持有者类型。
    /// @tparam C 满足 `callback` concept 的完成回调类型。
    /// @param gh 按 graph_holder 规则捕获或借用的子图。
    /// @param num 子图执行次数；0 表示不执行子图。
    /// @param callback 全部循环结束后调用的无参完成回调。
    template <graph_holder Gh, callback C = noop_callback>
        requires (capturable<C> && std::same_as<R, void>)
    explicit AsyncTask(Gh&& gh, std::uint64_t num, C&& callback = C{});

    /// @brief 创建尚未启动、由终止谓词控制子图循环的 void 异步任务。
    /// @tparam Gh 满足 `graph_holder` concept 的子图持有者类型。
    /// @tparam P 满足 `predicate` concept 的终止谓词类型。
    /// @tparam C 满足 `callback` concept 的完成回调类型。
    /// @param gh 按 graph_holder 规则捕获或借用的子图。
    /// @param predicate 每轮执行子图前调用；返回 true 时结束循环。
    /// @param callback 循环结束后调用的无参完成回调。
    template <graph_holder Gh, predicate P, callback C = noop_callback>
        requires (capturable<P, C> && std::same_as<R, void>)
    explicit AsyncTask(Gh&& gh, P&& predicate, C&& callback = C{});

    /// @brief 获取底层 Work 保存的任务名称视图。
    /// @return 指向 Work 内部名称字符串的 `std::string_view`。
    /// @pre 句柄有效。
    /// @warning 通过任一共享句柄修改任务名称、Work 被销毁或当前句柄失去关联后，
    ///          已取得的视图都可能失效。
    [[nodiscard]] std::string_view name() const noexcept;

    /// @brief 设置底层 Work 的任务名称并返回左值句柄。
    /// @tparam S 可构造 `std::string` 的名称类型。
    /// @param name 新名称。
    /// @return `*this`。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    /// @note 修改名称可能使此前取得的 `name()` 视图失效。
    template <typename S>
        requires std::constructible_from<std::string, S>
    AsyncTask& name(S&& name) &;

    /// @brief 在右值句柄上设置底层 Work 的任务名称。
    /// @tparam S 可构造 `std::string` 的名称类型。
    /// @param name 新名称。
    /// @return 移动后的句柄，便于临时对象链式调用。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    template <typename S>
        requires std::constructible_from<std::string, S>
    AsyncTask name(S&& name) &&;

    /// @brief 获取当前注册的执行前信号量获取请求数量。
    /// @return acquire 配置项数量；空句柄返回 0。
    /// @note 返回的是请求项数量，不是所有请求配额 count 的总和。
    [[nodiscard]] std::size_t num_acquires() const noexcept;

    /// @brief 获取当前注册的执行后信号量释放请求数量。
    /// @return release 配置项数量；空句柄返回 0。
    /// @note 返回的是请求项数量，不是所有释放配额 count 的总和。
    [[nodiscard]] std::size_t num_releases() const noexcept;

    /// @brief 获取当前注册到任务上的观察者数量。
    /// @return observer 列表长度；空句柄返回 0。
    [[nodiscard]] std::size_t num_observers() const noexcept;


    // ============================================================================
    // 信号量管理
    // ============================================================================

    /// @brief 为任务添加一个或多个执行前信号量获取请求，每个请求 1 个配额。
    /// @tparam Ts 非空的 `Semaphore` 类型包。
    /// @param semaphores 要借用的信号量；仅保存对象地址，不转移所有权。
    /// @return `*this`。
    /// @throws Exception 任一信号量已经存在于 acquire 列表。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    /// @warning 只要对应 acquire 请求仍然注册且任务尚可能访问它，Semaphore 对象就必须保持存活。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
    AsyncTask& acquire(Ts&... semaphores) &;

    /// @brief 在右值句柄上添加一个或多个执行前信号量获取请求，每个请求 1 个配额。
    /// @tparam Ts 非空的 `Semaphore` 类型包。
    /// @param semaphores 要借用的信号量；仅保存对象地址，不转移所有权。
    /// @return 移动后的句柄，便于临时对象链式调用。
    /// @throws Exception 任一信号量已经存在于 acquire 列表。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    /// @warning 只要对应 acquire 请求仍然注册且任务尚可能访问它，Semaphore 对象就必须保持存活。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
    AsyncTask acquire(Ts&... semaphores) &&;

    /// @brief 为任务添加一个执行前信号量获取请求。
    /// @param semaphore 要借用的信号量；仅保存对象地址，不转移所有权。
    /// @param count 执行前需要获取的配额；0 表示忽略本次请求。
    /// @return `*this`。
    /// @throws Exception semaphore 已存在于 acquire 列表。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    /// @warning 只要该 acquire 请求仍然注册且任务尚可能访问它，semaphore 就必须保持存活。
    AsyncTask& acquire(Semaphore& semaphore, std::size_t count) &;

    /// @brief 在右值句柄上添加一个指定配额的执行前信号量获取请求。
    /// @param semaphore 要借用的信号量；仅保存对象地址，不转移所有权。
    /// @param count 执行前需要获取的配额；0 表示忽略本次请求。
    /// @return 移动后的句柄。
    /// @throws Exception semaphore 已存在于 acquire 列表。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    /// @warning 只要该 acquire 请求仍然注册且任务尚可能访问它，semaphore 就必须保持存活。
    AsyncTask acquire(Semaphore& semaphore, std::size_t count) &&;

    /// @brief 为任务添加一个或多个执行后信号量释放请求，每个请求 1 个配额。
    /// @tparam Ts 非空的 `Semaphore` 类型包。
    /// @param semaphores 要借用的信号量；仅保存对象地址，不转移所有权。
    /// @return `*this`。
    /// @throws Exception 任一信号量已经存在于 release 列表。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    /// @warning 只要对应 release 请求仍然注册且任务尚可能访问它，Semaphore 对象就必须保持存活。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
    AsyncTask& release(Ts&... semaphores) &;

    /// @brief 在右值句柄上添加一个或多个执行后信号量释放请求，每个请求 1 个配额。
    /// @tparam Ts 非空的 `Semaphore` 类型包。
    /// @param semaphores 要借用的信号量；仅保存对象地址，不转移所有权。
    /// @return 移动后的句柄。
    /// @throws Exception 任一信号量已经存在于 release 列表。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    /// @warning 只要对应 release 请求仍然注册且任务尚可能访问它，Semaphore 对象就必须保持存活。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
    AsyncTask release(Ts&... semaphores) &&;

    /// @brief 为任务添加一个执行后信号量释放请求。
    /// @param semaphore 要借用的信号量；仅保存对象地址，不转移所有权。
    /// @param count 执行后需要释放的配额；0 表示忽略本次请求。
    /// @return `*this`。
    /// @throws Exception semaphore 已存在于 release 列表。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    /// @warning 只要该 release 请求仍然注册且任务尚可能访问它，semaphore 就必须保持存活。
    AsyncTask& release(Semaphore& semaphore, std::size_t count) &;

    /// @brief 在右值句柄上添加一个指定配额的执行后信号量释放请求。
    /// @param semaphore 要借用的信号量；仅保存对象地址，不转移所有权。
    /// @param count 执行后需要释放的配额；0 表示忽略本次请求。
    /// @return 移动后的句柄。
    /// @throws Exception semaphore 已存在于 release 列表。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    /// @warning 只要该 release 请求仍然注册且任务尚可能访问它，semaphore 就必须保持存活。
    AsyncTask release(Semaphore& semaphore, std::size_t count) &&;


    /// @brief 移除一个或多个执行前信号量获取请求。
    /// @tparam Ts 非空的 `Semaphore` 类型包。
    /// @param semaphores 要从 acquire 列表移除的信号量；不存在的项被忽略。
    /// @return `*this`。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
    AsyncTask& remove_acquire(Ts&... semaphores) & noexcept;

    /// @brief 在右值句柄上移除一个或多个执行前信号量获取请求。
    /// @tparam Ts 非空的 `Semaphore` 类型包。
    /// @param semaphores 要移除的信号量；不存在的项被忽略。
    /// @return 移动后的句柄。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
    AsyncTask remove_acquire(Ts&... semaphores) && noexcept;

    /// @brief 移除一个或多个执行后信号量释放请求。
    /// @tparam Ts 非空的 `Semaphore` 类型包。
    /// @param semaphores 要从 release 列表移除的信号量；不存在的项被忽略。
    /// @return `*this`。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
    AsyncTask& remove_release(Ts&... semaphores) & noexcept;

    /// @brief 在右值句柄上移除一个或多个执行后信号量释放请求。
    /// @tparam Ts 非空的 `Semaphore` 类型包。
    /// @param semaphores 要移除的信号量；不存在的项被忽略。
    /// @return 移动后的句柄。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
    AsyncTask remove_release(Ts&... semaphores) && noexcept;

    /// @brief 清空全部执行前信号量获取请求。
    /// @return `*this`。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    AsyncTask& clear_acquires() & noexcept;

    /// @brief 在右值句柄上清空全部执行前信号量获取请求。
    /// @return 移动后的句柄。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    AsyncTask clear_acquires() && noexcept;

    /// @brief 清空全部执行后信号量释放请求。
    /// @return `*this`。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    AsyncTask& clear_releases() & noexcept;

    /// @brief 在右值句柄上清空全部执行后信号量释放请求。
    /// @return 移动后的句柄。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    AsyncTask clear_releases() && noexcept;


    /// @brief 按存储顺序遍历全部 acquire 请求，并允许修改其配额。
    /// @tparam F 可接收 `(Semaphore&, std::size_t&)` 或仅接收 `Semaphore&` 的 visitor 类型。
    /// @param visitor 对每个 acquire 请求调用一次；接收 count 引用的重载可以原地修改配额。
    /// @pre 句柄有效，且不得与任务启动、执行或配置修改并发调用。
    /// @note 如果 F 同时匹配两种调用形式，优先调用 `(Semaphore&, std::size_t&)` 形式。
    template <typename F>
        requires std::invocable<F&, Semaphore&, std::size_t&> || std::invocable<F&, Semaphore&>
    void for_each_acquire(F&& visitor) noexcept(std::invocable<F&, Semaphore&, std::size_t&>
                                                    ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
                                                    : std::is_nothrow_invocable_v<F&, Semaphore&>);

    /// @brief 按存储顺序只读遍历全部 acquire 请求。
    /// @tparam F 可接收 `(const Semaphore&, std::size_t)` 或仅接收 `const Semaphore&` 的 visitor 类型。
    /// @param visitor 对每个 acquire 请求调用一次。
    /// @pre 句柄有效，且不得与 acquire 配置修改并发调用。
    /// @note 如果 F 同时匹配两种调用形式，优先调用 `(const Semaphore&, std::size_t)` 形式。
    template <typename F>
        requires std::invocable<F&, const Semaphore&, std::size_t> || std::invocable<F&, const Semaphore&>
    void for_each_acquire(F&& visitor) const noexcept(std::invocable<F&, const Semaphore&, std::size_t>
                                                          ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
                                                          : std::is_nothrow_invocable_v<F&, const Semaphore&>);

    /// @brief 按存储顺序遍历全部 release 请求，并允许修改其配额。
    /// @tparam F 可接收 `(Semaphore&, std::size_t&)` 或仅接收 `Semaphore&` 的 visitor 类型。
    /// @param visitor 对每个 release 请求调用一次；接收 count 引用的重载可以原地修改配额。
    /// @pre 句柄有效，且不得与任务启动、执行或配置修改并发调用。
    /// @note 如果 F 同时匹配两种调用形式，优先调用 `(Semaphore&, std::size_t&)` 形式。
    template <typename F>
        requires std::invocable<F&, Semaphore&, std::size_t&> || std::invocable<F&, Semaphore&>
    void for_each_release(F&& visitor) noexcept(std::invocable<F&, Semaphore&, std::size_t&>
                                                    ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
                                                    : std::is_nothrow_invocable_v<F&, Semaphore&>);

    /// @brief 按存储顺序只读遍历全部 release 请求。
    /// @tparam F 可接收 `(const Semaphore&, std::size_t)` 或仅接收 `const Semaphore&` 的 visitor 类型。
    /// @param visitor 对每个 release 请求调用一次。
    /// @pre 句柄有效，且不得与 release 配置修改并发调用。
    /// @note 如果 F 同时匹配两种调用形式，优先调用 `(const Semaphore&, std::size_t)` 形式。
    template <typename F>
        requires std::invocable<F&, const Semaphore&, std::size_t> || std::invocable<F&, const Semaphore&>
    void for_each_release(F&& visitor) const noexcept(std::invocable<F&, const Semaphore&, std::size_t>
                                                          ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
                                                          : std::is_nothrow_invocable_v<F&, const Semaphore&>);

    /// @brief 构造一个观察者并注册到当前任务。
    /// @tparam Observer 派生自 `TaskObserver` 的具体观察者类型。
    /// @tparam Args Observer 构造参数类型。
    /// @param args 完美转发给 Observer 构造函数的参数。
    /// @return 新建观察者的 `std::shared_ptr<Observer>`；Work 同时保存一个基类共享引用。
    /// @pre 句柄有效，且不得与任务启动、执行或观察者列表修改并发调用。
    /// @throws std::bad_alloc 创建 Observer、ObserverData 或扩展观察者容器失败。
    template <std::derived_from<TaskObserver> Observer, typename... Args>
        requires std::constructible_from<Observer, Args...>
    [[nodiscard]] std::shared_ptr<Observer> register_observer(Args&&... args);

    /// @brief 从当前任务的观察者列表中移除首次匹配的观察者。
    /// @tparam Observer 派生自 `TaskObserver` 的具体观察者类型。
    /// @param observer 要注销的观察者；空指针或未注册对象被忽略。
    /// @pre 句柄有效，且不得与任务启动、执行或观察者列表修改并发调用。
    /// @note 只释放 Work 持有的对应共享引用，不影响调用方持有的 shared_ptr。
    template <std::derived_from<TaskObserver> Observer>
    void unregister_observer(const std::shared_ptr<Observer>& observer) noexcept;

private:
    using Base::m_work;
    using Base::m_result;

    /// @brief 由已经创建的 Work 与结果槽构造有效 AsyncTask。
    /// @param work 任务对应的底层 Work。
    /// @param result 与 work 绑定的结果槽。
    /// @pre work 与 result 均非空，且具有匹配的生命周期。
    explicit AsyncTask(Work* work, ResultSlot<R>* result) noexcept
        : Base{work, result} {}

    /// @brief 从工厂返回的 `(Work*, ResultSlot<R>*)` 状态对构造 AsyncTask。
    /// @param state 工厂创建的 Work 与结果槽指针对。
    /// @pre state.first 与 state.second 均非空且相互匹配。
    explicit AsyncTask(std::pair<Work*, ResultSlot<R>*> state) noexcept
        : AsyncTask{state.first, state.second} {}

    /// @brief 将任务作为 parent 的子异步任务启动，并按依赖状态决定是否立即调度。
    /// @tparam Deps 前驱异步任务类型包。
    /// @tparam InheritTopology 是否借用父停止域；false 时仍通过 Work 父链参与等待。
    /// @param parent 当前子任务所属的父 Work。
    /// @param worker 当前执行父 Work 的 Worker。
    /// @param executor 执行该任务的 Executor。
    /// @param deps 可选前驱任务；只有所有未完成依赖解除后当前任务才进入调度队列。
    /// @throws Exception 当前句柄为空，或该任务已经被任一共享句柄成功启动。
    ///
    /// 启动成功后当前 Work 关联 parent，Topology 继承父停止域，并为执行生命周期增加
    /// 一个 Work 强引用；同时 parent 的 join_counter 增加 1，保证父任务等待该异步任务完成。
    template <bool InheritTopology, async_task... Deps>
    void _start(Work& parent, Worker& worker, Executor& executor, Deps&&... deps) {
        Work* work = m_work;

        if (!work) [[unlikely]] {
            throw Exception{"AsyncTask: empty task."};
        }

        Topology* topology = work->m_topology;
        auto& control = topology->m_control;
        auto current = control.load(std::memory_order_acquire);

        for (;;) {
            // CAS 只允许从未锁定的 Idle 状态启动；LOCKED 期间自然竞争失败并重试。
            current &= ~Topology::Control::LOCKED;

            if (Topology::Control::status(current) != Topology::Control::Status::Idle) [[unlikely]] {
                throw Exception{"AsyncTask: task can only be started once."};
            }

            const auto running = Topology::Control::set_status(current, Topology::Control::Status::Running);

            if (control.compare_exchange_weak(current,
                                              running,
                                              std::memory_order_acquire,
                                              std::memory_order_acquire)) {
                break;
            }
        }

        work->m_parent = std::addressof(parent);

        if constexpr (InheritTopology) {
            topology->m_parent = parent.m_topology;
            TFL_ASSERT(topology->m_parent);
        } else {
            topology->m_parent = nullptr;
        }

        topology->m_executor = std::addressof(executor);

        work->_increment_ref();
        parent.m_join_counter.fetch_add(1, std::memory_order_relaxed);

        if constexpr (sizeof...(Deps) != 0) {
            std::array<Work*, sizeof...(Deps)> predecessors{deps.m_work...};
            std::size_t num_predecessors = sizeof...(Deps);

            work->m_join_counter.store(num_predecessors, std::memory_order_relaxed);
            executor._link_predecessors(work, predecessors.begin(), predecessors.end(), num_predecessors);

            if (num_predecessors == 0) {
                executor._schedule(worker, work);
            }
        } else {
            executor._schedule(worker, work);
        }
    }

    /// @brief 将任务作为顶层异步任务启动，并按依赖状态决定是否立即调度。
    /// @tparam Deps 前驱异步任务类型包。
    /// @param executor 执行该任务的 Executor。
    /// @param deps 可选前驱任务；只有所有未完成依赖解除后当前任务才进入调度队列。
    /// @throws Exception 当前句柄为空，或该任务已经被任一共享句柄成功启动。
    ///
    /// 启动成功后 Work 不关联父任务，Topology 绑定 executor，并为执行生命周期增加一个
    /// Work 强引用；同时增加 Executor 的活动 Topology 计数，完成时由执行路径对称释放。
    template <async_task... Deps>
    void _start(Executor& executor, Deps&&... deps) {
        Work* work = m_work;

        if (!work) [[unlikely]] {
            throw Exception{"AsyncTask: empty task."};
        }

        Topology* topology = work->m_topology;
        auto& control = topology->m_control;
        auto current = control.load(std::memory_order_acquire);

        for (;;) {
            // CAS 只允许从未锁定的 Idle 状态启动；LOCKED 期间自然竞争失败并重试。
            current &= ~Topology::Control::LOCKED;

            if (Topology::Control::status(current) != Topology::Control::Status::Idle) [[unlikely]] {
                throw Exception{"AsyncTask: task can only be started once."};
            }

            const auto running = Topology::Control::set_status(current, Topology::Control::Status::Running);

            if (control.compare_exchange_weak(current,
                                              running,
                                              std::memory_order_acquire,
                                              std::memory_order_acquire)) {
                break;
            }
        }

        work->m_parent = nullptr;
        topology->m_parent = nullptr;
        topology->m_executor = std::addressof(executor);

        work->_increment_ref();
        executor._increment_topology();

        if constexpr (sizeof...(Deps) != 0) {
            std::array<Work*, sizeof...(Deps)> predecessors{deps.m_work...};
            std::size_t num_predecessors = sizeof...(Deps);

            work->m_join_counter.store(num_predecessors, std::memory_order_relaxed);
            executor._link_predecessors(work, predecessors.begin(), predecessors.end(), num_predecessors);

            if (num_predecessors == 0) {
                if (Worker* worker = executor._this_worker()) {
                    executor._schedule(*worker, work);
                } else {
                    executor._schedule(work);
                }
            }
        } else {
            if (Worker* worker = executor._this_worker()) {
                executor._schedule(*worker, work);
            } else {
                executor._schedule(work);
            }
        }
    }
};

// ============================================================================
// AsyncTask 实现
// ============================================================================

template <typename R>
AsyncTask<R>::AsyncTask(std::nullptr_t) noexcept
    : Base{nullptr} {
}

template <typename R>
AsyncTask<R>& AsyncTask<R>::operator=(std::nullptr_t) noexcept {
    Base::operator=(nullptr);
    return *this;
}

template <typename R>
template <typename T>
    requires (basic_invocable<T> && capturable<T> && std::same_as<R, basic_return_t<T>>)
AsyncTask<R>::AsyncTask(T&& task)
    : AsyncTask{make_async_task_basic(std::forward<T>(task))} {
}

template <typename R>
template <typename T>
    requires (runtime_invocable<T> && capturable<T> && std::same_as<R, runtime_return_t<T>>)
AsyncTask<R>::AsyncTask(T&& task)
    : AsyncTask{make_async_task_runtime(std::forward<T>(task))} {
}

template <typename R>
template <typename T>
    requires (subflow_invocable<T> && capturable<T> && std::same_as<R, subflow_return_t<T>>)
AsyncTask<R>::AsyncTask(T&& task)
    : AsyncTask{make_async_task_subflow(std::forward<T>(task))} {
}

template <typename R>
template <graph_holder Gh, callback C>
    requires (capturable<C> && std::same_as<R, void>)
AsyncTask<R>::AsyncTask(Gh&& gh, C&& callback)
    : AsyncTask{std::forward<Gh>(gh), 1ULL, std::forward<C>(callback)} {
}

template <typename R>
template <graph_holder Gh, callback C>
    requires (capturable<C> && std::same_as<R, void>)
AsyncTask<R>::AsyncTask(Gh&& gh, std::uint64_t num, C&& callback)
    : AsyncTask{std::forward<Gh>(gh), [num]() mutable noexcept -> bool { return num-- == 0; }, std::forward<C>(callback)} {
}

template <typename R>
template <graph_holder Gh, predicate P, callback C>
    requires (capturable<P, C> && std::same_as<R, void>)
AsyncTask<R>::AsyncTask(Gh&& gh, P&& predicate, C&& callback)
    : AsyncTask{make_async_task_module(std::forward<Gh>(gh), std::forward<P>(predicate), std::forward<C>(callback))} {
}

// ============================================================================
// 名称
// ============================================================================

template <typename R>
std::string_view AsyncTask<R>::name() const noexcept {
    return m_work ? m_work->_name() : std::string_view{};
}

template <typename R>
template <typename S>
    requires std::constructible_from<std::string, S>
AsyncTask<R>& AsyncTask<R>::name(S&& value) & {
    TFL_ASSERT(m_work && "AsyncTask must reference a valid Work.");
    m_work->_set_name(std::forward<S>(value));
    return *this;
}

template <typename R>
template <typename S>
    requires std::constructible_from<std::string, S>
AsyncTask<R> AsyncTask<R>::name(S&& value) && {
    static_cast<AsyncTask&>(*this).name(std::forward<S>(value));
    return std::move(*this);
}

// ============================================================================
// 信号量数量
// ============================================================================

template <typename R>
std::size_t AsyncTask<R>::num_acquires() const noexcept {
    return m_work ? m_work->_num_acquires() : 0;
}

template <typename R>
std::size_t AsyncTask<R>::num_releases() const noexcept {
    return m_work ? m_work->_num_releases() : 0;
}

template <typename R>
std::size_t AsyncTask<R>::num_observers() const noexcept {
    return m_work ? m_work->_num_observers() : 0;
}

// ============================================================================
// 信号量配置
// ============================================================================

template <typename R>
template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
AsyncTask<R>& AsyncTask<R>::acquire(Ts&... semaphores) & {
    TFL_ASSERT(m_work && "AsyncTask must reference a valid Work.");
    (m_work->_acquire(std::addressof(semaphores), std::size_t{1}), ...);
    return *this;
}

template <typename R>
template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
AsyncTask<R> AsyncTask<R>::acquire(Ts&... semaphores) && {
    static_cast<AsyncTask&>(*this).acquire(semaphores...);
    return std::move(*this);
}

template <typename R>
AsyncTask<R>& AsyncTask<R>::acquire(Semaphore& semaphore, std::size_t count) & {
    TFL_ASSERT(m_work && "AsyncTask must reference a valid Work.");
    m_work->_acquire(std::addressof(semaphore), count);
    return *this;
}

template <typename R>
AsyncTask<R> AsyncTask<R>::acquire(Semaphore& semaphore, std::size_t count) && {
    static_cast<AsyncTask&>(*this).acquire(semaphore, count);
    return std::move(*this);
}

template <typename R>
template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
AsyncTask<R>& AsyncTask<R>::release(Ts&... semaphores) & {
    TFL_ASSERT(m_work && "AsyncTask must reference a valid Work.");
    (m_work->_release(std::addressof(semaphores), std::size_t{1}), ...);
    return *this;
}

template <typename R>
template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
AsyncTask<R> AsyncTask<R>::release(Ts&... semaphores) && {
    static_cast<AsyncTask&>(*this).release(semaphores...);
    return std::move(*this);
}

template <typename R>
AsyncTask<R>& AsyncTask<R>::release(Semaphore& semaphore, std::size_t count) & {
    TFL_ASSERT(m_work && "AsyncTask must reference a valid Work.");
    m_work->_release(std::addressof(semaphore), count);
    return *this;
}

template <typename R>
AsyncTask<R> AsyncTask<R>::release(Semaphore& semaphore, std::size_t count) && {
    static_cast<AsyncTask&>(*this).release(semaphore, count);
    return std::move(*this);
}


// ============================================================================
// 删除和清空信号量配置
// ============================================================================

template <typename R>
template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
AsyncTask<R>& AsyncTask<R>::remove_acquire(Ts&... semaphores) & noexcept {
    TFL_ASSERT(m_work && "AsyncTask must reference a valid Work.");
    (m_work->_remove_acquire(std::addressof(semaphores)), ...);
    return *this;
}

template <typename R>
template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
AsyncTask<R> AsyncTask<R>::remove_acquire(Ts&... semaphores) && noexcept {
    static_cast<AsyncTask&>(*this).remove_acquire(semaphores...);
    return std::move(*this);
}

template <typename R>
template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
AsyncTask<R>& AsyncTask<R>::remove_release(Ts&... semaphores) & noexcept {
    TFL_ASSERT(m_work && "AsyncTask must reference a valid Work.");
    (m_work->_remove_release(std::addressof(semaphores)), ...);
    return *this;
}

template <typename R>
template <typename... Ts>
    requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
AsyncTask<R> AsyncTask<R>::remove_release(Ts&... semaphores) && noexcept {
    static_cast<AsyncTask&>(*this).remove_release(semaphores...);
    return std::move(*this);
}

template <typename R>
AsyncTask<R>& AsyncTask<R>::clear_acquires() & noexcept {
    TFL_ASSERT(m_work && "AsyncTask must reference a valid Work.");
    m_work->_clear_acquires();
    return *this;
}

template <typename R>
AsyncTask<R> AsyncTask<R>::clear_acquires() && noexcept {
    static_cast<AsyncTask&>(*this).clear_acquires();
    return std::move(*this);
}

template <typename R>
AsyncTask<R>& AsyncTask<R>::clear_releases() & noexcept {
    TFL_ASSERT(m_work && "AsyncTask must reference a valid Work.");
    m_work->_clear_releases();
    return *this;
}

template <typename R>
AsyncTask<R> AsyncTask<R>::clear_releases() && noexcept {
    static_cast<AsyncTask&>(*this).clear_releases();
    return std::move(*this);
}
// ============================================================================
// 遍历信号量配置
// ============================================================================

template <typename R>
template <typename F>
    requires std::invocable<F&, Semaphore&, std::size_t&> || std::invocable<F&, Semaphore&>
void AsyncTask<R>::for_each_acquire(F&& visitor) noexcept(std::invocable<F&, Semaphore&, std::size_t&>
                                                              ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
                                                              : std::is_nothrow_invocable_v<F&, Semaphore&>) {
    TFL_ASSERT(m_work && "AsyncTask must reference a valid Work.");
    for (auto& request : m_work->_acquires()) {
        if constexpr (std::invocable<F&, Semaphore&, std::size_t&>) {
            std::invoke(visitor, *request.sem, request.count);
        } else {
            std::invoke(visitor, *request.sem);
        }
    }
}

template <typename R>
template <typename F>
    requires std::invocable<F&, const Semaphore&, std::size_t> || std::invocable<F&, const Semaphore&>
void AsyncTask<R>::for_each_acquire(F&& visitor) const noexcept(std::invocable<F&, const Semaphore&, std::size_t>
                                                                    ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
                                                                    : std::is_nothrow_invocable_v<F&, const Semaphore&>) {
    TFL_ASSERT(m_work && "AsyncTask must reference a valid Work.");
    for (const auto& request : m_work->_acquires()) {
        if constexpr (std::invocable<F&, const Semaphore&, std::size_t>) {
            std::invoke(visitor, *request.sem, request.count);
        } else {
            std::invoke(visitor, *request.sem);
        }
    }
}

template <typename R>
template <typename F>
    requires std::invocable<F&, Semaphore&, std::size_t&> || std::invocable<F&, Semaphore&>
void AsyncTask<R>::for_each_release(F&& visitor) noexcept(std::invocable<F&, Semaphore&, std::size_t&>
                                                              ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
                                                              : std::is_nothrow_invocable_v<F&, Semaphore&>) {
    TFL_ASSERT(m_work && "AsyncTask must reference a valid Work.");
    for (auto& request : m_work->_releases()) {
        if constexpr (std::invocable<F&, Semaphore&, std::size_t&>) {
            std::invoke(visitor, *request.sem, request.count);
        } else {
            std::invoke(visitor, *request.sem);
        }
    }
}

template <typename R>
template <typename F>
    requires std::invocable<F&, const Semaphore&, std::size_t> || std::invocable<F&, const Semaphore&>
void AsyncTask<R>::for_each_release(F&& visitor) const noexcept(std::invocable<F&, const Semaphore&, std::size_t>
                                                                    ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
                                                                    : std::is_nothrow_invocable_v<F&, const Semaphore&>) {
    TFL_ASSERT(m_work && "AsyncTask must reference a valid Work.");
    for (const auto& request : m_work->_releases()) {
        if constexpr (std::invocable<F&, const Semaphore&, std::size_t>) {
            std::invoke(visitor, *request.sem, request.count);
        } else {
            std::invoke(visitor, *request.sem);
        }
    }
}


// ============================================================================
// 观察者
// ============================================================================

template <typename R>
template <std::derived_from<TaskObserver> Observer, typename... Args>
    requires std::constructible_from<Observer, Args...>
std::shared_ptr<Observer> AsyncTask<R>::register_observer(Args&&... args) {
    TFL_ASSERT(m_work && "AsyncTask must reference a valid Work.");
    auto observer = std::make_shared<Observer>(std::forward<Args>(args)...);

    if (!m_work->m_observers) {
        m_work->m_observers = std::make_unique<Work::ObserverData>();
    }

    m_work->m_observers->observers.emplace_back(std::static_pointer_cast<TaskObserver>(observer));
    return observer;
}

template <typename R>
template <std::derived_from<TaskObserver> Observer>
void AsyncTask<R>::unregister_observer(const std::shared_ptr<Observer>& observer) noexcept {
    TFL_ASSERT(m_work && "AsyncTask must reference a valid Work.");
    if (!m_work->m_observers || !observer) {
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

    if (observers.empty()) {
        m_work->m_observers.reset();
    }
}

// ============================================================================
// AsyncTask CTAD
// ============================================================================

/// @brief 从普通 callable 推导对应返回类型的 AsyncTask。
template <typename T>
    requires (basic_invocable<T> && capturable<T>)
AsyncTask(T&&) -> AsyncTask<basic_return_t<T>>;

/// @brief 从接收 Runtime 的 callable 推导对应返回类型的 AsyncTask。
template <typename T>
    requires (runtime_invocable<T> && capturable<T>)
AsyncTask(T&&) -> AsyncTask<runtime_return_t<T>>;

/// @brief 从接收 SubFlow 的 callable 推导对应返回类型的 AsyncTask。
template <typename T>
    requires (subflow_invocable<T> && capturable<T>)
AsyncTask(T&&) -> AsyncTask<subflow_return_t<T>>;

/// @brief 从单次子图任务构造参数推导 `AsyncTask<void>`。
template <graph_holder Gh, callback C = noop_callback>
    requires capturable<C>
AsyncTask(Gh&&, C&& = C{}) -> AsyncTask<void>;

/// @brief 从定次循环子图任务构造参数推导 `AsyncTask<void>`。
template <graph_holder Gh, callback C = noop_callback>
    requires capturable<C>
AsyncTask(Gh&&, std::uint64_t, C&& = C{}) -> AsyncTask<void>;

/// @brief 从谓词循环子图任务构造参数推导 `AsyncTask<void>`。
template <graph_holder Gh, predicate P, callback C = noop_callback>
    requires capturable<P, C>
AsyncTask(Gh&&, P&&, C&& = C{}) -> AsyncTask<void>;

// ============================================================================
// 输出流
// ============================================================================

/// @brief 将 AsyncTask 的 D2 描述写入输出流。
/// @tparam R 任务结果类型。
/// @param stream 接收 D2 文本的目标输出流。
/// @param task 要导出的异步任务句柄。
/// @return stream，支持连续插入。
/// @throws Exception task 为空。
template <typename R>
std::ostream& operator<<(std::ostream& stream, const AsyncTask<R>& task) {
    task.dump(stream);
    return stream;
}




static_assert(async_future<AsyncFuture<int>>);
static_assert(async_future<AsyncFuture<int>&>);
static_assert(async_future<const AsyncFuture<int>&>);

static_assert(async_future<AsyncTask<int>>);
static_assert(async_future<AsyncTask<int>&>);
static_assert(async_future<const AsyncTask<int>&>);

static_assert(!async_future<int>);
static_assert(!async_future<std::string>);

static_assert(async_task<AsyncTask<int>>);
static_assert(async_task<AsyncTask<int>&>);

static_assert(!async_task<AsyncFuture<int>>);

}  // namespace tfl

// ==================== 标准库扩展 ====================
namespace std {

/// @brief 为 `tfl::AsyncTask<R>` 提供基于底层 Work 身份的标准哈希支持。
///
/// 哈希语义与 AsyncFuture 的任务身份比较保持一致：共享同一个 Work 的 AsyncTask
/// 具有相同哈希值，空句柄按空 Work 指针计算。
///
/// @tparam R 异步任务的结果类型。
template <typename R>
struct hash<tfl::AsyncTask<R>> {
    /// @brief 计算 AsyncTask 所关联底层 Work 的哈希值。
    /// @param task 要计算哈希值的异步任务句柄。
    /// @return `task.hash_value()`；空句柄按空 Work 指针计算。
    std::size_t operator()(const tfl::AsyncTask<R>& task) const noexcept {
        return task.hash_value();
    }
};

}  // namespace std
