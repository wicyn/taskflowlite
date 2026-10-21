/// @file coro_awaiter.hpp
/// @brief CoroAwaiter —— 自定义 Coroutine 操作的挂起适配器。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-10-21
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <concepts>
#include <coroutine>
#include <type_traits>
#include <utility>

#include "coro_token.hpp"

namespace tfl {

/// @brief TFL 自定义 Coroutine 操作约束。
///
/// 自定义操作负责就绪判断、异步操作注册和结果读取。
/// await_suspend() 接收 CoroToken，不直接访问 Coro 执行上下文。
///
/// @warning await_suspend() 必须为 noexcept，且返回 void。
template <typename A>
concept CoroOperation = requires(std::remove_reference_t<A>& op, CoroToken token) {
    { op.await_ready() } -> std::convertible_to<bool>;
    { op.await_suspend(token) } noexcept -> std::same_as<void>;
    op.await_resume();
};

/// @brief 将 TFL 自定义操作转换为标准 Awaiter。
///
/// 框架保存当前 Coroutine 的恢复句柄并维护挂起状态。
/// 自定义操作通过 CoroToken 提交恢复请求，不直接访问 Coro。
///
/// @tparam A 自定义操作类型，可为值类型或引用类型。
/// @warning 异步回调使用 CoroToken 时，必须保证所属 Coro 仍然存活。
template <typename A>
class CoroAwaiter final {
    static_assert(CoroOperation<A>, "CoroAwaiter requires a valid CoroOperation");

public:
    /// @brief 绑定当前 Coroutine 并保存自定义操作。
    explicit CoroAwaiter(Coro& coro, A&& op) noexcept(std::is_nothrow_constructible_v<A, A&&>);

    CoroAwaiter(const CoroAwaiter&) = delete;
    CoroAwaiter& operator=(const CoroAwaiter&) = delete;

    /// @brief 检查操作是否已就绪。
    /// @return 已就绪时返回 true，直接执行 await_resume()。
    [[nodiscard]] constexpr bool await_ready() noexcept(noexcept(static_cast<bool>(m_op.await_ready())));

    /// @brief 保存当前 Coroutine 的恢复句柄并启动自定义异步操作。
    /// @param handle 当前正在挂起的 Coroutine 句柄。
    void await_suspend(std::coroutine_handle<> handle) noexcept;

    /// @brief 获取自定义操作的完成结果。
    /// @return 原样转发自定义操作的 await_resume() 返回值。
    constexpr decltype(auto) await_resume() noexcept(noexcept(m_op.await_resume()));

private:
    Coro& m_coro;
    A m_op;
};

// ============================================================================
// CoroAwaiter 实现
// ============================================================================

template <typename A>
inline CoroAwaiter<A>::CoroAwaiter(Coro& coro, A&& op) noexcept(std::is_nothrow_constructible_v<A, A&&>)
    : m_coro{coro}
    , m_op{std::forward<A>(op)} {}

template <typename A>
inline constexpr bool CoroAwaiter<A>::await_ready() noexcept(noexcept(static_cast<bool>(m_op.await_ready()))) {
    return static_cast<bool>(m_op.await_ready());
}

template <typename A>
inline void CoroAwaiter<A>::await_suspend(std::coroutine_handle<> handle) noexcept {
    m_coro._suspend(handle);
    m_op.await_suspend(CoroToken{m_coro});
}

template <typename A>
inline constexpr decltype(auto) CoroAwaiter<A>::await_resume() noexcept(noexcept(m_op.await_resume())) {
    return m_op.await_resume();
}

} // namespace tfl
