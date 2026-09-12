/// @file async_task.hpp
/// @brief AsyncTask —— 单次启动异步任务的配置、共享生命周期、依赖启动与结果访问句柄。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
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
/// 每个非空 AsyncTask 关联同一个底层 Work、其所属 Topology 和对应的 ResultSlot<R>。
/// 句柄通过 AsyncFuture<R> 持有 Work 强引用；复制共享任务状态，移动转移句柄关联。
///
/// 非空任务由 Executor::defer_async 创建，创建时绑定执行器，状态为 Idle。
/// 调用 start(deps...) 时保存前驱依赖并提交任务；前驱必须已经启动或完成。
/// 任一共享句柄成功启动后，其他共享句柄不能再次启动同一任务。
///
/// 未启动任务不计入 Executor 的活动拓扑数量；最后一份强引用释放时正常销毁。
/// 已启动任务持有额外执行引用，执行完成后释放执行引用并归还活动拓扑计数。
/// 保存的前驱强引用在当前 Work 真正销毁时释放。
///
/// 本类提供任务配置与启动；等待、结果访问、异常传播和停止请求由 AsyncFuture<R> 提供。
///
/// @tparam R 任务结果类型，可为值类型、左值引用类型或 void。
/// @warning 创建任务的 Executor 必须在 start 调用及任务执行期间保持有效。
/// @warning 任务配置不得与启动、执行或其他配置修改并发进行。
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


    /// @brief 保存前驱依赖，并通过创建时绑定的 Executor 启动当前任务。
    /// @tparam Deps 满足 async_future concept 的前驱句柄类型包。
    /// @param deps 前驱 AsyncTask / AsyncFuture 句柄，支持不同结果类型。
    /// @return 当前句柄的左值引用。
    /// @throws Exception 当前句柄为空、任务已经启动、依赖自身或前驱处于 Idle。
    /// @pre Executor 必须在本次调用和任务执行期间保持有效。
    /// @pre 当前句柄和传入依赖句柄不得被并发重置或移动。
    /// @pre 任务配置不得与启动或执行并发修改。
    /// @note 空依赖忽略；重复依赖按传入次数保存，不移动或清空前驱句柄。
    /// @note 每个依赖条目持有一份前驱强引用，直到当前 Work 真正销毁。
    /// @note 本函数不等待任务完成，暂不处理内存分配失败后的提交恢复。
    template <async_future... Deps>
    AsyncTask& start(Deps&&... deps) &;

    /// @brief 在右值句柄上启动当前任务，并返回移动后的句柄。
    /// @tparam Deps 满足 async_future concept 的前驱句柄类型包。
    /// @param deps 前驱 AsyncTask / AsyncFuture 句柄，支持不同结果类型。
    /// @return 按值返回移动后的句柄，源句柄随后变为空。
    /// @throws Exception 当前句柄为空、任务已经启动、依赖自身或前驱处于 Idle。
    /// @pre Executor 必须在本次调用和任务执行期间保持有效。
    /// @pre 当前句柄和传入依赖句柄不得被并发重置或移动。
    /// @pre 任务配置不得与启动或执行并发修改。
    /// @note 依赖处理规则与左值版本一致，成功启动后才移动当前句柄。
    template <async_future... Deps>
    AsyncTask start(Deps&&... deps) &&;

    /// @brief 获取底层 Work 保存的任务名称视图。
    /// @return 指向 Work 内部名称字符串的 std::string_view；空句柄返回空视图。
    /// @warning 修改任务名称或 Work 被销毁后，已取得的视图可能失效。
    [[nodiscard]] std::string_view name() const noexcept;

    /// @brief 设置底层 Work 的任务名称并返回左值句柄。
    /// @tparam S 可构造 std::string 的名称类型。
    /// @param name 新名称。
    /// @return `*this`。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    /// @note 修改名称可能使此前取得的 name() 视图失效。
    template <typename S>
        requires std::constructible_from<std::string, S>
    AsyncTask& name(S&& name) &;

    /// @brief 在右值句柄上设置底层 Work 的任务名称。
    /// @tparam S 可构造 std::string 的名称类型。
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
    /// @tparam Ts 非空的 Semaphore 类型包。
    /// @param semaphores 要借用的信号量；仅保存对象地址，不转移所有权。
    /// @return `*this`。
    /// @throws Exception 任一信号量已经存在于 acquire 列表。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    /// @warning 只要对应 acquire 请求仍然注册且任务尚可能访问它，Semaphore 对象就必须保持存活。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
    AsyncTask& acquire(Ts&... semaphores) &;

    /// @brief 在右值句柄上添加一个或多个执行前信号量获取请求，每个请求 1 个配额。
    /// @tparam Ts 非空的 Semaphore 类型包。
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
    /// @tparam Ts 非空的 Semaphore 类型包。
    /// @param semaphores 要借用的信号量；仅保存对象地址，不转移所有权。
    /// @return `*this`。
    /// @throws Exception 任一信号量已经存在于 release 列表。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    /// @warning 只要对应 release 请求仍然注册且任务尚可能访问它，Semaphore 对象就必须保持存活。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
    AsyncTask& release(Ts&... semaphores) &;

    /// @brief 在右值句柄上添加一个或多个执行后信号量释放请求，每个请求 1 个配额。
    /// @tparam Ts 非空的 Semaphore 类型包。
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
    /// @tparam Ts 非空的 Semaphore 类型包。
    /// @param semaphores 要从 acquire 列表移除的信号量；不存在的项被忽略。
    /// @return `*this`。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
    AsyncTask& remove_acquire(Ts&... semaphores) & noexcept;

    /// @brief 在右值句柄上移除一个或多个执行前信号量获取请求。
    /// @tparam Ts 非空的 Semaphore 类型包。
    /// @param semaphores 要移除的信号量；不存在的项被忽略。
    /// @return 移动后的句柄。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
    AsyncTask remove_acquire(Ts&... semaphores) && noexcept;

    /// @brief 移除一个或多个执行后信号量释放请求。
    /// @tparam Ts 非空的 Semaphore 类型包。
    /// @param semaphores 要从 release 列表移除的信号量；不存在的项被忽略。
    /// @return `*this`。
    /// @pre 句柄有效，且不得与任务启动、执行或其他配置修改并发调用。
    template <typename... Ts>
        requires (sizeof...(Ts) > 0) && (std::same_as<Ts, Semaphore> && ...)
    AsyncTask& remove_release(Ts&... semaphores) & noexcept;

    /// @brief 在右值句柄上移除一个或多个执行后信号量释放请求。
    /// @tparam Ts 非空的 Semaphore 类型包。
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
    /// @tparam F 可接收 (Semaphore&, std::size_t&) 或仅接收 Semaphore& 的 visitor 类型。
    /// @param visitor 对每个 acquire 请求调用一次；接收 count 引用的重载可以原地修改配额。
    /// @pre 句柄有效，且不得与任务启动、执行或配置修改并发调用。
    /// @note 如果 F 同时匹配两种调用形式，优先调用 (Semaphore&, std::size_t&) 形式。
    template <typename F>
        requires std::invocable<F&, Semaphore&, std::size_t&> || std::invocable<F&, Semaphore&>
    void for_each_acquire(F&& visitor) noexcept(std::invocable<F&, Semaphore&, std::size_t&>
                                                    ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
                                                    : std::is_nothrow_invocable_v<F&, Semaphore&>);

    /// @brief 按存储顺序只读遍历全部 acquire 请求。
    /// @tparam F 可接收 (const Semaphore&, std::size_t) 或仅接收 const Semaphore& 的 visitor 类型。
    /// @param visitor 对每个 acquire 请求调用一次。
    /// @pre 句柄有效，且不得与 acquire 配置修改并发调用。
    /// @note 如果 F 同时匹配两种调用形式，优先调用 (const Semaphore&, std::size_t) 形式。
    template <typename F>
        requires std::invocable<F&, const Semaphore&, std::size_t> || std::invocable<F&, const Semaphore&>
    void for_each_acquire(F&& visitor) const noexcept(std::invocable<F&, const Semaphore&, std::size_t>
                                                          ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
                                                          : std::is_nothrow_invocable_v<F&, const Semaphore&>);

    /// @brief 按存储顺序遍历全部 release 请求，并允许修改其配额。
    /// @tparam F 可接收 (Semaphore&, std::size_t&) 或仅接收 Semaphore& 的 visitor 类型。
    /// @param visitor 对每个 release 请求调用一次；接收 count 引用的重载可以原地修改配额。
    /// @pre 句柄有效，且不得与任务启动、执行或配置修改并发调用。
    /// @note 如果 F 同时匹配两种调用形式，优先调用 (Semaphore&, std::size_t&) 形式。
    template <typename F>
        requires std::invocable<F&, Semaphore&, std::size_t&> || std::invocable<F&, Semaphore&>
    void for_each_release(F&& visitor) noexcept(std::invocable<F&, Semaphore&, std::size_t&>
                                                    ? std::is_nothrow_invocable_v<F&, Semaphore&, std::size_t&>
                                                    : std::is_nothrow_invocable_v<F&, Semaphore&>);

    /// @brief 按存储顺序只读遍历全部 release 请求。
    /// @tparam F 可接收 (const Semaphore&, std::size_t) 或仅接收 const Semaphore& 的 visitor 类型。
    /// @param visitor 对每个 release 请求调用一次。
    /// @pre 句柄有效，且不得与 release 配置修改并发调用。
    /// @note 如果 F 同时匹配两种调用形式，优先调用 (const Semaphore&, std::size_t) 形式。
    template <typename F>
        requires std::invocable<F&, const Semaphore&, std::size_t> || std::invocable<F&, const Semaphore&>
    void for_each_release(F&& visitor) const noexcept(std::invocable<F&, const Semaphore&, std::size_t>
                                                          ? std::is_nothrow_invocable_v<F&, const Semaphore&, std::size_t>
                                                          : std::is_nothrow_invocable_v<F&, const Semaphore&>);

    /// @brief 构造一个观察者并注册到当前任务。
    /// @tparam Observer 派生自 TaskObserver 的具体观察者类型。
    /// @tparam Args Observer 构造参数类型。
    /// @param args 完美转发给 Observer 构造函数的参数。
    /// @return 新建观察者的 std::shared_ptr<Observer>；Work 同时保存一个基类共享引用。
    /// @pre 句柄有效，且不得与任务启动、执行或观察者列表修改并发调用。
    /// @throws std::bad_alloc 创建 Observer、ObserverData 或扩展观察者容器失败。
    template <std::derived_from<TaskObserver> Observer, typename... Args>
        requires std::constructible_from<Observer, Args...>
    [[nodiscard]] std::shared_ptr<Observer> register_observer(Args&&... args);

    /// @brief 从当前任务的观察者列表中移除首次匹配的观察者。
    /// @tparam Observer 派生自 TaskObserver 的具体观察者类型。
    /// @param observer 要注销的观察者；空指针或未注册对象被忽略。
    /// @pre 句柄有效，且不得与任务启动、执行或观察者列表修改并发调用。
    /// @note 只释放 Work 持有的对应共享引用，不影响调用方持有的 shared_ptr。
    template <std::derived_from<TaskObserver> Observer>
    void unregister_observer(const std::shared_ptr<Observer>& observer) noexcept;

private:
    using Base::m_work;
    using Base::m_result;

    /// @brief 从工厂返回的状态对构造有效 AsyncTask。
    /// @param state 工厂创建的 (Work*, ResultSlot<R>*) 状态对。
    /// @pre Work 和结果槽均非空且相互匹配，Topology 已绑定创建任务的 Executor。
    /// @pre Work 尚未发布，状态为 Idle，边表为空，父 Work 和父 Topology 均为空。
    /// @note 基类取得一份 Work 强引用；不保存依赖，不启动任务，不增加活动拓扑计数。
    explicit AsyncTask(std::pair<Work*, ResultSlot<R>*> state) noexcept;

};

// ============================================================================
// AsyncTask 构造与赋值
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
AsyncTask<R>::AsyncTask(std::pair<Work*, ResultSlot<R>*> state) noexcept
    : Base{state.first, state.second} {
    TFL_ASSERT(m_work);
    TFL_ASSERT(m_result);
    TFL_ASSERT(m_work->m_topology);
    TFL_ASSERT(!m_work->m_parent);
    TFL_ASSERT(!m_work->m_topology->m_parent);
    TFL_ASSERT(m_work->m_edges.empty() && m_work->m_num_successors == 0);
}

// ============================================================================
// AsyncTask::start
// ============================================================================
template <typename R>
template <async_future... Deps>
AsyncTask<R>& AsyncTask<R>::start(Deps&&... deps) & {
    constexpr std::size_t num_dependencies = sizeof...(Deps);
    static_assert(num_dependencies < std::numeric_limits<std::uint32_t>::max());

    Work* const work = m_work;

    if (!work) [[unlikely]] {
        throw Exception{"AsyncTask: empty task."};
    }

    Topology* const topology = work->m_topology;
    Executor& executor = topology->m_executor;
    auto& control = topology->m_control;
    auto current = control.load(std::memory_order_acquire);

    if (Topology::Control::status(current) != Topology::Control::Status::Idle) [[unlikely]] {
        throw Exception{"AsyncTask: task can only be started once."};
    }

    std::array<Work*, num_dependencies> predecessors{deps.m_work...};
    std::size_t num_predecessors = 0;

    if constexpr (num_dependencies != 0) {
        // 先校验全部非空依赖，校验失败时不修改当前任务。
        for (Work* predecessor : predecessors) {
            if (!predecessor) {
                continue;
            }

            if (predecessor == work) [[unlikely]] {
                throw Exception{"AsyncTask: task cannot depend on itself."};
            }

            const auto state = predecessor->m_topology->m_control.load(std::memory_order_acquire);

            if (Topology::Control::status(state) == Topology::Control::Status::Idle) [[unlikely]] {
                throw Exception{"AsyncTask: dependency has not been started."};
            }

            predecessors[num_predecessors++] = predecessor;
        }
    }

    // 保持 Idle 并取得自身锁，保证同一 Work 只由一个线程完成启动准备。
    for (;;) {
        if (Topology::Control::status(current) != Topology::Control::Status::Idle) [[unlikely]] {
            throw Exception{"AsyncTask: task can only be started once."};
        }

        current &= ~Topology::Control::LOCKED;

        if (control.compare_exchange_weak(current,
                                          current | Topology::Control::LOCKED,
                                          std::memory_order_acquire,
                                          std::memory_order_acquire)) {
            break;
        }
    }

    TFL_ASSERT(!work->m_parent);
    TFL_ASSERT(!topology->m_parent);
    TFL_ASSERT(work->m_edges.empty() && work->m_num_successors == 0);

    if constexpr (num_dependencies != 0) {
        if (num_predecessors != 0) {
            auto& edges = work->m_edges;
            edges.reserve(num_predecessors);

            // 每个条目持有一份前驱强引用，直到当前 Work 销毁。
            for (std::size_t i = 0; i < num_predecessors; ++i) {
                Work* const predecessor = predecessors[i];
                edges.push_back(predecessor);
                predecessor->_increment_ref();
            }
        }

        // 保留一个提交保护计数，依赖登记结束前禁止当前任务就绪。
        work->m_join_counter.store(static_cast<std::uint32_t>(num_predecessors + 1), std::memory_order_relaxed);
    } else {
        work->m_join_counter.store(0, std::memory_order_relaxed);
    }

    // 执行引用和活动拓扑计数均在启动时取得，由异步收尾路径归还。
    work->_increment_ref();
    executor._increment_topology();

    // 持锁从 Idle 发布 Running，再解锁；保留引用计数和停止标志。
    control.fetch_or(Topology::Control::state_bits(Topology::Control::Status::Running), std::memory_order_release);
    control.fetch_and(~Topology::Control::LOCKED, std::memory_order_release);

    if constexpr (num_dependencies != 0) {
        // 发布后自身边表可能被其他后继修改，使用局部数组遍历前驱。
        if (num_predecessors != 0) {
            executor._link_predecessors(work, predecessors.begin(), predecessors.begin() + num_predecessors);
        }

        // 最后释放提交保护计数，取得 1 -> 0 的线程负责调度。
        if (work->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) != 1) {
            return *this;
        }
    }

    if (Worker* worker = executor._this_worker()) {
        executor._schedule(*worker, work);
    } else {
        executor._schedule(work);
    }

    return *this;
}

template <typename R>
template <async_future... Deps>
AsyncTask<R> AsyncTask<R>::start(Deps&&... deps) && {
    static_cast<AsyncTask&>(*this).start(std::forward<Deps>(deps)...);
    return std::move(*this);
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
            std::invoke(visitor, std::as_const(*request.sem), std::size_t{request.count});
        } else {
            std::invoke(visitor, std::as_const(*request.sem));
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
            std::invoke(visitor, std::as_const(*request.sem), std::size_t{request.count});
        } else {
            std::invoke(visitor, std::as_const(*request.sem));
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
// Executor::defer_async
// ============================================================================

template <typename T>
    requires (basic_invocable<T> && capturable<T>)
inline auto Executor::defer_async(T&& task) -> AsyncTask<basic_return_t<T>> {
    return AsyncTask<basic_return_t<T>>{make_async_task_basic(*this, std::forward<T>(task))};
}

template <typename T>
    requires (runtime_invocable<T> && capturable<T>)
inline auto Executor::defer_async(T&& task) -> AsyncTask<runtime_return_t<T>> {
    return AsyncTask<runtime_return_t<T>>{make_async_task_runtime(*this, std::forward<T>(task))};
}

template <typename T>
    requires (subflow_invocable<T> && capturable<T>)
inline auto Executor::defer_async(T&& task) -> AsyncTask<subflow_return_t<T>> {
    return AsyncTask<subflow_return_t<T>>{make_async_task_subflow(*this, std::forward<T>(task))};
}

template <graph_holder Gh, callback C>
    requires capturable<C>
inline AsyncTask<void> Executor::defer_async(Gh&& gh, C&& callback) {
    return defer_async(std::forward<Gh>(gh), std::uint64_t{1}, std::forward<C>(callback));
}

template <graph_holder Gh, callback C>
    requires capturable<C>
inline AsyncTask<void> Executor::defer_async(Gh&& gh, std::uint64_t num, C&& callback) {
    return defer_async(std::forward<Gh>(gh), [num]() mutable noexcept -> bool { return num-- == 0; }, std::forward<C>(callback));
}

template <graph_holder Gh, predicate P, callback C>
    requires capturable<P, C>
inline AsyncTask<void> Executor::defer_async(Gh&& gh, P&& pred, C&& callback) {
    return AsyncTask<void>{make_async_task_module(*this, std::forward<Gh>(gh), std::forward<P>(pred), std::forward<C>(callback))};
}

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

}  // namespace tfl

// ==================== 标准库扩展 ====================
namespace std {

/// @brief 为 tfl::AsyncTask<R> 提供基于底层 Work 身份的标准哈希支持。
///
/// 哈希语义与 AsyncFuture 的任务身份比较保持一致：共享同一个 Work 的 AsyncTask
/// 具有相同哈希值，空句柄按空 Work 指针计算。
///
/// @tparam R 异步任务的结果类型。
template <typename R>
struct hash<tfl::AsyncTask<R>> {
    /// @brief 计算 AsyncTask 所关联底层 Work 的哈希值。
    /// @param task 要计算哈希值的异步任务句柄。
    /// @return task.hash_value()；空句柄按空 Work 指针计算。
    std::size_t operator()(const tfl::AsyncTask<R>& task) const noexcept {
        return task.hash_value();
    }
};

}  // namespace std
