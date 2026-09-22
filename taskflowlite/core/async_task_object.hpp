/// @file async_task_object.hpp
/// @brief AsyncTaskObject —— 带业务对象类型信息的异步任务句柄。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-09-14
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cstddef>
#include <memory>
#include <utility>

#include "async_task.hpp"
#include "macros.hpp"

namespace tfl {

// ============================================================================
// AsyncTaskObject 定义
// ============================================================================

/// @brief 带业务对象类型信息、共享底层任务生命周期的异步任务句柄。
///
/// 基类 AsyncTask<R> 持有底层 Work 的强引用，业务对象由 Work 内部的 Invoker 按值保存。
/// 本类仅保存业务对象指针，不单独分配或销毁业务对象。
/// 复制句柄共享同一个任务和业务对象；移动句柄转移关联，不复制或移动业务对象。
///
/// object() 获取业务对象，继承的 get() 获取异步结果。
/// const 句柄采用指针语义，仍允许通过 object() 修改业务对象。
/// 启动、等待、任务配置、结果访问和停止请求均由基类提供。
///
/// @tparam R 异步任务结果类型，可为值类型、左值引用类型或 void。
/// @tparam F 节点实际保存的业务对象类型。
/// @note 继承的链式接口返回 AsyncTask<R>，不会保留本类的业务对象类型信息。
/// @note 应先保存 defer_async_object() 返回的句柄，再调用配置和启动接口。
/// @warning object() 不执行等待或加锁，业务对象访问必须与任务执行正确同步。
/// @warning 返回的对象引用不持有强引用，底层 Work 销毁后引用失效。
/// @warning 不得通过基类引用重新绑定当前句柄，否则对象指针与任务关联可能不一致。
/// @warning 本类及基类均不提供虚析构，不得通过基类指针删除本类对象。
template <typename R, typename F>
class AsyncTaskObject final : public AsyncTask<R> {

    friend class Executor;

    using Base = AsyncTask<R>;

public:

    using object_type = F;

    /// @brief 构造不关联任务和业务对象的空句柄。
    AsyncTaskObject() noexcept;

    /// @brief 显式构造不关联任务和业务对象的空句柄。
    explicit AsyncTaskObject(std::nullptr_t) noexcept;

    /// @brief 复制句柄，共享同一个任务和业务对象；源句柄允许为空。
    AsyncTaskObject(const AsyncTaskObject&) noexcept;

    /// @brief 释放当前任务强引用后，共享另一句柄的任务和业务对象。
    /// @return `*this`；源句柄允许为空或与当前对象相同。
    AsyncTaskObject& operator=(const AsyncTaskObject&) noexcept;

    /// @brief 接管源句柄的任务强引用和对象指针，源句柄随后变为空。
    AsyncTaskObject(AsyncTaskObject&& other) noexcept;

    /// @brief 释放当前任务强引用后，接管源句柄的任务和业务对象关联。
    /// @return `*this`；自移动不改变当前句柄，否则源句柄变为空。
    AsyncTaskObject& operator=(AsyncTaskObject&& other) noexcept;

    /// @brief 释放当前句柄持有的任务强引用，是否销毁 Work 由引用计数决定。
    ~AsyncTaskObject();

    /// @brief 释放任务关联并置空业务对象指针。
    /// @return `*this`。
    AsyncTaskObject& operator=(std::nullptr_t) noexcept;

    /// @brief 释放当前任务强引用，并把句柄置空。
    /// @note 不请求停止，不等待任务完成，不影响其他共享句柄的关联。
    void reset() noexcept;

    /// @brief 获取节点内部保存的业务对象。
    /// @return 业务对象引用，不复制对象，不增加任务强引用。
    /// @pre 当前句柄有效，且业务对象访问已与任务执行正确同步。
    /// @note const 句柄不限制业务对象的可变性。
    [[nodiscard]] F& object() const noexcept;

private:

    /// @brief 接管已经建立强引用的任务句柄，并关联其内部业务对象。
    /// @param task 已经建立强引用的异步任务句柄。
    /// @param object 该任务内部保存的业务对象。
    /// @pre task 有效，object 属于 task 关联的 Work，且地址在 Work 生命周期内保持稳定。
    AsyncTaskObject(Base task, F& object) noexcept;

    F* m_object{nullptr};
};


// ============================================================================
// AsyncTaskObject 实现
// ============================================================================

template <typename R, typename F>
AsyncTaskObject<R, F>::AsyncTaskObject() noexcept = default;

template <typename R, typename F>
AsyncTaskObject<R, F>::AsyncTaskObject(std::nullptr_t) noexcept
    : Base{nullptr} {
}

template <typename R, typename F>
AsyncTaskObject<R, F>::AsyncTaskObject(const AsyncTaskObject&) noexcept = default;

template <typename R, typename F>
AsyncTaskObject<R, F>& AsyncTaskObject<R, F>::operator=(const AsyncTaskObject&) noexcept = default;

template <typename R, typename F>
AsyncTaskObject<R, F>::AsyncTaskObject(AsyncTaskObject&& other) noexcept
    : Base{std::move(other)}
    , m_object{std::exchange(other.m_object, nullptr)} {
}

template <typename R, typename F>
AsyncTaskObject<R, F>& AsyncTaskObject<R, F>::operator=(AsyncTaskObject&& other) noexcept {
    if (this != std::addressof(other)) {
        Base::operator=(std::move(other));
        m_object = std::exchange(other.m_object, nullptr);
    }

    return *this;
}

template <typename R, typename F>
AsyncTaskObject<R, F>::~AsyncTaskObject() = default;

template <typename R, typename F>
AsyncTaskObject<R, F>& AsyncTaskObject<R, F>::operator=(std::nullptr_t) noexcept {
    reset();
    return *this;
}

template <typename R, typename F>
void AsyncTaskObject<R, F>::reset() noexcept {
    m_object = nullptr;
    Base::reset();
}

template <typename R, typename F>
F& AsyncTaskObject<R, F>::object() const noexcept {
    TFL_ASSERT(Base::valid() && m_object);
    return *m_object;
}

template <typename R, typename F>
AsyncTaskObject<R, F>::AsyncTaskObject(Base task, F& object) noexcept
    : Base{std::move(task)}
    , m_object{std::addressof(object)} {
}

// ============================================================================
// Executor::defer_async_object
// ============================================================================

template <typename T, typename... Args>
    requires (std::same_as<T, std::decay_t<T>> && basic_invocable<T> && std::constructible_from<T, Args&&...>)
inline auto Executor::defer_async_object(Args&&... args) -> AsyncTaskObject<basic_return_t<T>, T> {
    auto [work, result, object] = make_async_task_basic_object<T>(*this, std::forward<Args>(args)...);

    return AsyncTaskObject<basic_return_t<T>, T>{AsyncTask<basic_return_t<T>>{work, result}, object};
}

template <typename T, typename... Args>
    requires (std::same_as<T, std::decay_t<T>> && runtime_invocable<T> && std::constructible_from<T, Args&&...>)
inline auto Executor::defer_async_object(Args&&... args) -> AsyncTaskObject<runtime_return_t<T>, T> {
    auto [work, result, object] = make_async_task_runtime_object<T>(*this, std::forward<Args>(args)...);

    return AsyncTaskObject<runtime_return_t<T>, T>{AsyncTask<runtime_return_t<T>>{work, result}, object};
}

template <typename T, typename... Args>
    requires (std::same_as<T, std::decay_t<T>> && subflow_invocable<T> && std::constructible_from<T, Args&&...>)
inline auto Executor::defer_async_object(Args&&... args) -> AsyncTaskObject<subflow_return_t<T>, T> {
    auto [work, result, object] = make_async_task_subflow_object<T>(*this, std::forward<Args>(args)...);

    return AsyncTaskObject<subflow_return_t<T>, T>{AsyncTask<subflow_return_t<T>>{work, result}, object};
}

// ============================================================================
// Executor::defer_async_object —— 默认构造
// ============================================================================

template <graph_holder Gh, callback C>
    requires (std::same_as<Gh, std::decay_t<Gh>> && capturable<C> && std::constructible_from<Gh>)
inline auto Executor::defer_async_object(C&& callback) -> AsyncTaskObject<void, Gh> {
    return defer_async_object<Gh>(std::tuple{}, std::forward<C>(callback));
}

template <graph_holder Gh, callback C>
    requires (std::same_as<Gh, std::decay_t<Gh>> && capturable<C> && std::constructible_from<Gh>)
inline auto Executor::defer_async_object(std::uint64_t num, C&& callback) -> AsyncTaskObject<void, Gh> {
    return defer_async_object<Gh>(std::tuple{}, num, std::forward<C>(callback));
}

template <graph_holder Gh, predicate P, callback C>
    requires (std::same_as<Gh, std::decay_t<Gh>> && capturable<P, C> && std::constructible_from<Gh>)
inline auto Executor::defer_async_object(P&& pred, C&& callback) -> AsyncTaskObject<void, Gh> {
    return defer_async_object<Gh>(std::tuple{}, std::forward<P>(pred), std::forward<C>(callback));
}

// ============================================================================
// Executor::defer_async_object —— Tuple 参数构造
// ============================================================================

template <graph_holder Gh, typename Tuple, callback C>
    requires (std::same_as<Gh, std::decay_t<Gh>> && capturable<C> && tuple_constructible_from<Gh, Tuple&&>)
inline auto Executor::defer_async_object(Tuple&& args, C&& callback) -> AsyncTaskObject<void, Gh> {
    return defer_async_object<Gh>(std::forward<Tuple>(args), std::uint64_t{1}, std::forward<C>(callback));
}

template <graph_holder Gh, typename Tuple, callback C>
    requires (std::same_as<Gh, std::decay_t<Gh>> && capturable<C> && tuple_constructible_from<Gh, Tuple&&>)
inline auto Executor::defer_async_object(Tuple&& args, std::uint64_t num, C&& callback) -> AsyncTaskObject<void, Gh> {
    auto pred = [num]() mutable noexcept -> bool {
        return num-- == 0;
    };

    return defer_async_object<Gh>(std::forward<Tuple>(args), std::move(pred), std::forward<C>(callback));
}

template <graph_holder Gh, typename Tuple, predicate P, callback C>
    requires (std::same_as<Gh, std::decay_t<Gh>> && capturable<P, C> && tuple_constructible_from<Gh, Tuple&&>)
inline auto Executor::defer_async_object(Tuple&& args, P&& pred, C&& callback) -> AsyncTaskObject<void, Gh> {
    return std::apply([this, &pred, &callback](auto&&... values) -> AsyncTaskObject<void, Gh> {
        auto [work, result, object] = make_async_task_module_object<Gh>(
            *this,
            std::forward<P>(pred),
            std::forward<C>(callback),
            std::forward<decltype(values)>(values)...
            );

        return AsyncTaskObject<void, Gh>{AsyncTask<void>{work, result}, object};
    }, std::forward<Tuple>(args));
}

}  // namespace tfl
