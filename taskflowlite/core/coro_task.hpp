/// @file coro_task.hpp
/// @brief CoroTask —— Coroutine frame、结果存储与对称转移。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-10-03
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <concepts>
#include <coroutine>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

#include "coro_awaiter.hpp"
#include "forward.hpp"
#include "result_slot.hpp"
#include "traits.hpp"
#include "macros.hpp"

namespace tfl {

/// @brief 独占 Coroutine frame 的返回对象，支持结果提取及父子对称转移。
///
/// CoroTask 必须由包含 Coro& 参数的协程函数创建，创建时自动绑定第一个 Coro&。
/// 不提供默认构造，普通函数不能通过 `return CoroTask<R>{}` 伪造协程返回对象。
/// 由 Promise 创建的 CoroTask 可移动；移动后的源对象允许处于无 frame 状态。
/// CoroTask 可交给 TFL 的 CoroInvoker 管理，外部唤醒只能通过 Coro::resume() 进入 Executor。
///
/// @tparam R 返回值类型，支持普通对象、左值引用和 void。
template <typename R = void>
class CoroTask final {
    static_assert(!std::is_rvalue_reference_v<R>, "CoroTask does not support rvalue reference results");

    TFL_WORK_SUBCLASS_FRIENDS;

public:
    struct promise_type;
    class Awaiter;

    using result_type = R;
    using handle_type = std::coroutine_handle<promise_type>;

    CoroTask(const CoroTask&) = delete;
    CoroTask& operator=(const CoroTask&) = delete;

    CoroTask(CoroTask&& other) noexcept
        : m_handle{std::exchange(other.m_handle, {})} {}

    CoroTask& operator=(CoroTask&& other) noexcept {
        if (this != std::addressof(other)) {
            reset();
            m_handle = std::exchange(other.m_handle, {});
        }
        return *this;
    }

    ~CoroTask() noexcept {
        reset();
    }

    /// @brief 是否拥有 Coroutine frame。
    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(m_handle);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return valid();
    }

    /// @brief Coroutine 是否到达 final_suspend。
    [[nodiscard]] bool done() const noexcept {
        return m_handle && m_handle.done();
    }

    /// @brief 获取 Coroutine 结果，并传播未处理异常。
    ///
    /// @pre Coroutine 已完成。
    R result() {
        TFL_ASSERT(done());

        if (auto eptr = m_handle.promise()._exception()) [[unlikely]] {
            std::rethrow_exception(eptr);
        }

        return _take_result();
    }

    /// @brief 查看已完成 Coroutine 捕获的异常，不清除异常。
    /// @pre Coroutine 已完成。
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        TFL_ASSERT(done());
        return m_handle.promise()._exception();
    }

    /// @brief 销毁 Coroutine frame。
    /// @pre Coroutine 不在运行，且没有未完成的外部恢复操作。
    void reset() noexcept {
        auto handle = std::exchange(m_handle, handle_type{});
        if (handle) {
            handle.destroy();
        }
    }

    /// @brief 把当前 frame 的所有权转交给子协程 Awaiter。
    /// @warning 调用后当前 CoroTask 变为空。
    [[nodiscard]] Awaiter operator co_await() noexcept;

private:
    /// @brief 仅允许内部 Invoker 构造尚未绑定 Coroutine frame 的空任务。
    /// @note 用户不能直接构造 CoroTask；有效任务只能由 Promise 创建。
    CoroTask() noexcept = default;

    explicit CoroTask(handle_type handle) noexcept : m_handle{handle} {
        TFL_ASSERT(m_handle);
    }

    /// @brief 观察根协程句柄，不转移 frame 所有权。
    [[nodiscard]] handle_type _handle() const noexcept {
        TFL_ASSERT(m_handle);
        return m_handle;
    }

    /// @brief 取出根协程异常，由 CoroInvoker 在完成后调用。
    [[nodiscard]] std::exception_ptr _take_exception() noexcept {
        TFL_ASSERT(done());
        return m_handle.promise()._take_exception();
    }

    /// @brief 直接取出 Coroutine 结果，不检查异常。
    /// @pre Coroutine 已完成，且不存在未处理异常。
    [[nodiscard]] decltype(auto) _take_result() noexcept {
        TFL_ASSERT(done());
        return std::move(m_handle.promise()).take();
    }

private:
    handle_type m_handle{};
};

// ============================================================================
// CoroTask<R>::promise_type
// ============================================================================

template <typename R>
struct CoroTask<R>::promise_type : ResultSlot<R> {
private:
    template <typename>
    friend class CoroTask;

    /// @brief 从协程函数参数中查找第一个非 const Coro&。
    template <typename T, typename... Rest>
    [[nodiscard]] static Coro& _find_coro(T&& value, Rest&&... rest) noexcept {
        if constexpr (std::same_as<std::remove_reference_t<T>, Coro>) {
            return value;
        } else {
            static_assert(sizeof...(Rest) > 0, "CoroTask coroutine requires a Coro& argument");
            if constexpr (sizeof...(Rest) > 0) {
                return _find_coro(std::forward<Rest>(rest)...);
            }
        }
    }

    /// @brief 子协程最终挂起时对称转移到父协程；根协程转移到 noop_coroutine。
    struct FinalAwaiter : std::suspend_always {
        [[nodiscard]] std::coroutine_handle<> await_suspend(handle_type handle) const noexcept {
            auto continuation = handle.promise().m_continuation;
            return continuation ? continuation : std::noop_coroutine();
        }
    };

public:
    promise_type() = delete;

    /// @brief 创建 Promise 时自动绑定协程函数参数中的第一个 Coro&。
    template <typename... Args>
    explicit promise_type(Args&&... args) noexcept
        : m_coro{_find_coro(std::forward<Args>(args)...)} {}

    [[nodiscard]] CoroTask get_return_object() noexcept {
        return CoroTask{handle_type::from_promise(*this)};
    }

    [[nodiscard]] std::suspend_always initial_suspend() const noexcept {
        return {};
    }

    [[nodiscard]] FinalAwaiter final_suspend() const noexcept {
        return {};
    }

    void unhandled_exception() noexcept {
        m_exception = std::current_exception();
    }

    /// @brief 仅允许等待 CoroTask，保留左值与右值语义。
    template <typename T>
        requires coro_task<T> && (!std::is_const_v<std::remove_reference_t<T>>)
    [[nodiscard]] decltype(auto) await_transform(T&& task) noexcept {
        return std::forward<T>(task);
    }

    /// @brief 把 TFL 自定义操作转换为 CoroAwaiter。
    template <CoroOperation A>
    [[nodiscard]] auto await_transform(A&& op) {
        return CoroAwaiter<A>{m_coro, std::forward<A>(op)};
    }

    /// @brief 透传 JoinOperation，并校验 Coroutine 上下文。
    template <typename A>
        requires std::same_as<std::remove_cvref_t<A>, Coro::JoinOperation>
    [[nodiscard]] decltype(auto) await_transform(A&& op) {
        if (std::addressof(op.m_coro) != std::addressof(m_coro)) [[unlikely]] {
            TFL_THROW(Exception{"CoroTask: JoinOperation belongs to another Coro."});
        }

        return std::forward<A>(op);
    }

    /// @brief 校验并透传 SilentForkOperation；不转移其子 Work 所有权。
    /// @param op 当前 Coro 创建的 SilentForkOperation 临时对象。
    /// @return 原操作的右值引用，由协程等待表达式管理操作对象生命周期。
    [[nodiscard]] Coro::SilentForkOperation&& await_transform(Coro::SilentForkOperation&& op) {
        if (std::addressof(op.m_coro) != std::addressof(m_coro)) [[unlikely]] {
            TFL_THROW(Exception{"CoroTask: SilentForkOperation belongs to another Coro."});
        }

        return std::move(op);
    }

    /// @brief 校验并透传任意返回值和依赖组合的 ForkOperation。
    /// @tparam T 子 Work 的结果类型。
    /// @tparam Deps 子 Work 的前置 AsyncFuture / AsyncTask 类型包。
    /// @param op 当前 Coro 创建的 ForkOperation 临时对象。
    /// @return 原操作的右值引用，不复制或移动 ForkOperation。
    template <typename T, async_future... Deps>
    [[nodiscard]] Coro::ForkOperation<T, Deps...>&& await_transform(Coro::ForkOperation<T, Deps...>&& op) {
        if (std::addressof(op.m_coro) != std::addressof(m_coro)) [[unlikely]] {
            TFL_THROW(Exception{"CoroTask: ForkOperation belongs to another Coro."});
        }

        return std::move(op);
    }

private:
    void _set_continuation(std::coroutine_handle<> parent) noexcept {
        TFL_ASSERT(parent);
        TFL_ASSERT(!m_continuation);
        m_continuation = parent;
    }

    [[nodiscard]] std::exception_ptr _exception() const noexcept {
        return m_exception;
    }

    [[nodiscard]] std::exception_ptr _take_exception() noexcept {
        return std::exchange(m_exception, {});
    }

private:
    Coro& m_coro;
    std::coroutine_handle<> m_continuation{};
    std::exception_ptr m_exception{};
};

// ============================================================================
// CoroTask<R>::Awaiter
// ============================================================================

template <typename R>
class CoroTask<R>::Awaiter final : public std::suspend_always {
    friend class CoroTask<R>;

public:
    Awaiter(const Awaiter&) = delete;
    Awaiter& operator=(const Awaiter&) = delete;
    Awaiter(Awaiter&&) = delete;
    Awaiter& operator=(Awaiter&&) = delete;

    ~Awaiter() noexcept {
        if (m_handle) {
            m_handle.destroy();
        }
    }

    /// @brief 记录 continuation，并返回子协程句柄实现对称转移。
    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) noexcept {
        m_handle.promise()._set_continuation(parent);
        return m_handle;
    }

    /// @brief 获取子 Coroutine 的执行结果，并传播异常。
    R await_resume() {
        TFL_ASSERT(m_handle && m_handle.done());

        auto& promise = m_handle.promise();

        if (auto eptr = promise._take_exception()) [[unlikely]] {
            std::rethrow_exception(eptr);
        }

        return std::move(promise).take();
    }

private:
    explicit Awaiter(handle_type handle) noexcept : m_handle{handle} {
        TFL_ASSERT(m_handle);
    }

    handle_type m_handle{};
};

// ============================================================================
// CoroTask<R>::operator co_await
// ============================================================================

template <typename R>
auto CoroTask<R>::operator co_await() noexcept -> Awaiter {
    TFL_ASSERT(m_handle);
    return Awaiter{std::exchange(m_handle, handle_type{})};
}

} // namespace tfl
