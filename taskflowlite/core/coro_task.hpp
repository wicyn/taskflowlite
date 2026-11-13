/// @file coro_task.hpp
/// @brief CoroTask —— Coroutine Frame、结果存储与 Promise 管理。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-10-03
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

#include "coro.hpp"
#include "coro_awaiter.hpp"
#include "forward.hpp"
#include "result_slot.hpp"
#include "traits.hpp"
#include "macros.hpp"

namespace tfl {

/// @brief 独占 Coroutine frame 的返回对象；子协程由 Coro::call() 对称转移。
///
/// CoroTask 必须由包含非 const Coro& 参数的协程函数创建，并自动绑定第一个 Coro&。
/// 默认构造、移动、显式回收和结果访问仅供 Coro 与内部 Invoker 使用。
/// 用户通过协程函数返回 CoroTask；不可手动移动或 _reset() 改变 Frame 的回收顺序。
/// 子协程只能通过 Coro::call() 对称转移；CoroTask 不提供 operator co_await。
/// Frame 存储由所属 Coro 的 Stacklet 管理，销毁必须遵循同一 Coro 的 LIFO 顺序。
/// Frame 的创建和销毁必须在所属 Coro 当前绑定的 Worker 上进行。
/// CoroTask 可交给 TFL 的 CoroInvoker 管理，外部唤醒只能通过 Coro::resume() 进入 Executor。
/// @warning 析构保持公开以支持协程返回对象；直接保存返回对象仍须遵守 Frame 的 LIFO 生命周期。
///
/// @tparam R 返回值类型，支持普通对象、左值引用和 void。
template <typename R = void>
class CoroTask final {
    static_assert(!std::is_rvalue_reference_v<R>, "CoroTask does not support rvalue reference results");

    TFL_WORK_SUBCLASS_FRIENDS;

    friend class Coro;

public:
    /// @brief 编译器通过 coroutine_traits 查找的 Promise 类型，必须公开可访问。
    struct promise_type;

    using result_type = R;

    CoroTask(const CoroTask&) = delete;
    CoroTask& operator=(const CoroTask&) = delete;

    ~CoroTask() noexcept {
        _reset();
    }

private:
    using handle_type = std::coroutine_handle<promise_type>;

    /// @brief 仅允许内部 Invoker 构造尚未绑定 Coroutine frame 的空任务。
    /// @note 用户不能直接构造 CoroTask；有效任务只能由 Promise 创建。
    CoroTask() noexcept = default;

    CoroTask(CoroTask&& other) noexcept
        : m_handle{std::exchange(other.m_handle, {})} {}

    CoroTask& operator=(CoroTask&& other) noexcept {
        if (this != std::addressof(other)) {
            _reset();
            m_handle = std::exchange(other.m_handle, {});
        }
        return *this;
    }

    explicit CoroTask(handle_type handle) noexcept : m_handle{handle} {
        TFL_ASSERT(m_handle);
    }

    /// @brief 是否拥有 Coroutine frame。
    [[nodiscard]] bool _valid() const noexcept {
        return static_cast<bool>(m_handle);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return _valid();
    }

    /// @brief Coroutine 是否到达 final_suspend。
    [[nodiscard]] bool _done() const noexcept {
        return m_handle && m_handle.done();
    }

    /// @brief 获取 Coroutine 结果，并传播未处理异常。
    ///
    /// @pre Coroutine 已完成。
    R _result() {
        TFL_ASSERT(_done());

        if (auto eptr = m_handle.promise()._exception()) [[unlikely]] {
            std::rethrow_exception(eptr);
        }

        return _take_result();
    }

    /// @brief 查看已完成 Coroutine 捕获的异常，不清除异常。
    /// @pre Coroutine 已完成。
    [[nodiscard]] std::exception_ptr _exception() const noexcept {
        TFL_ASSERT(_done());
        return m_handle.promise()._exception();
    }

    /// @brief 销毁 Coroutine frame。
    /// @pre Coroutine 不在运行，且没有未完成的外部恢复操作。
    void _reset() noexcept {
        auto handle = std::exchange(m_handle, handle_type{});
        if (handle) {
            handle.destroy();
        }
    }

    /// @brief 观察根协程句柄，不转移 frame 所有权。
    [[nodiscard]] handle_type _handle() const noexcept {
        TFL_ASSERT(m_handle);
        return m_handle;
    }

    /// @brief 取出根协程异常，由 CoroInvoker 在完成后调用。
    [[nodiscard]] std::exception_ptr _take_exception() noexcept {
        TFL_ASSERT(_done());
        return m_handle.promise()._take_exception();
    }

    /// @brief 直接取出 Coroutine 结果，不检查异常；供根任务 Invoker 使用。
    /// @pre Coroutine 已完成，且不存在未处理异常。
    [[nodiscard]] decltype(auto) _take_result() noexcept {
        TFL_ASSERT(_done());
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

    friend class Coro;

    struct alignas(Stacklet::alignment) FrameHeader {
        Coro* coro;
    };

    static_assert(sizeof(FrameHeader) % Stacklet::alignment == 0);

    /// @brief 从协程函数参数中查找第一个非 const Coro&。
    template <typename T, typename... Rest>
    [[nodiscard]] static Coro& _find_coro(T&& value, Rest&&... rest) noexcept {
        if constexpr (std::same_as<T, Coro&>) {
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
    /// @brief 从所属 Coro 的活动 Stacklet 分配 Frame；空间不足时获取新的栈段。
    /// @pre 所属 Coro 已绑定当前 Worker；根 Frame 也通过该 Worker 获取栈段。
    template <typename... Args>
    [[nodiscard]] static void* operator new(std::size_t size, Args&&... args) {
        if (size > std::numeric_limits<std::size_t>::max() - sizeof(FrameHeader)) [[unlikely]] {
            TFL_THROW(std::bad_alloc{});
        }

        Coro& coro = _find_coro(std::forward<Args>(args)...);

        auto* memory = static_cast<std::byte*>(coro._allocate_frame(size + sizeof(FrameHeader)));
        ::new (memory) FrameHeader{std::addressof(coro)};
        return memory + sizeof(FrameHeader);
    }

    /// @brief 根据 FrameHeader 找到所属 Coro，并按 LIFO 回退其活动 Stacklet。
    static void operator delete(void* pointer) noexcept {
        if (!pointer) {
            return;
        }

        auto* header = reinterpret_cast<FrameHeader*>(
            static_cast<std::byte*>(pointer) - sizeof(FrameHeader));

        header->coro->_deallocate_frame(header);
    }

    static void operator delete(void* pointer, std::size_t) noexcept {
        operator delete(pointer);
    }

    promise_type() = delete;

    /// @brief 创建 Promise 时自动绑定协程函数参数中的第一个非 const Coro&。
    /// @note 结果槽构造可能抛异常，此时由 Coroutine 创建路径回收已申请的 Frame。
    template <typename... Args>
    explicit promise_type(Args&&... args) noexcept(std::is_nothrow_default_constructible_v<ResultSlot<R>>)
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

    /// @brief 把 TFL 自定义操作转换为 CoroAwaiter。
    template <CoroOperation A>
    [[nodiscard]] auto await_transform(A&& op) {
        return CoroAwaiter<A>{m_coro, std::forward<A>(op)};
    }

    /// @brief 校验并透传同一 Coro 的延迟协程调用。
    template <typename T>
    [[nodiscard]] Coro::CallOperation<T>& await_transform(Coro::CallOperation<T>& op) {
        if (std::addressof(op.m_coro) != std::addressof(m_coro)) [[unlikely]] {
            TFL_THROW(Exception{"CoroTask: CallOperation belongs to another Coro."});
        }

        return op;
    }

    /// @brief 校验并透传右值 CallOperation，不移动操作对象。
    template <typename T>
    [[nodiscard]] Coro::CallOperation<T>&& await_transform(Coro::CallOperation<T>&& op) {
        if (std::addressof(op.m_coro) != std::addressof(m_coro)) [[unlikely]] {
            TFL_THROW(Exception{"CoroTask: CallOperation belongs to another Coro."});
        }

        return std::move(op);
    }

    /// @brief 透传 JoinOperation，并校验 Coroutine 上下文。
    template <typename A>
        requires (std::same_as<std::remove_cvref_t<A>, Coro::JoinOperation> && !std::is_const_v<std::remove_reference_t<A>>)
    [[nodiscard]] decltype(auto) await_transform(A&& op) {
        if (std::addressof(op.m_coro) != std::addressof(m_coro)) [[unlikely]] {
            TFL_THROW(Exception{"CoroTask: JoinOperation belongs to another Coro."});
        }

        return std::forward<A>(op);
    }

    /// @brief 校验并透传 SilentForkOperation，支持左值和右值。
    /// @note 不复制或移动 Operation，对象仍由原作用域管理。
    template <typename T>
        requires (std::same_as<std::remove_cvref_t<T>, Coro::SilentForkOperation> && !std::is_const_v<std::remove_reference_t<T>>)
    [[nodiscard]] decltype(auto) await_transform(T&& op) {
        if (std::addressof(op.m_coro) != std::addressof(m_coro)) [[unlikely]] {
            TFL_THROW(Exception{"CoroTask: SilentForkOperation belongs to another Coro."});
        }

        return std::forward<T>(op);
    }

    /// @brief 校验并透传左值 ForkOperation，不复制操作对象。
    template <typename T, async_future... Deps>
    [[nodiscard]] Coro::ForkOperation<T, Deps...>& await_transform(Coro::ForkOperation<T, Deps...>& op) {
        if (std::addressof(op.m_coro) != std::addressof(m_coro)) [[unlikely]] {
            TFL_THROW(Exception{"CoroTask: ForkOperation belongs to another Coro."});
        }

        return op;
    }

    /// @brief 校验并透传右值 ForkOperation，不移动操作对象。
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

} // namespace tfl

