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
#include <memory>
#include <type_traits>
#include <utility>

#include "coro_token.hpp"
#include "macros.hpp"

namespace tfl {

/// @brief TFL 自定义 Coroutine 操作约束。
///
/// 自定义操作负责就绪判断、异步操作注册和结果读取。
/// await_suspend() 接收 CoroToken，不直接访问 Coro 执行上下文。
///
/// await_ready() 返回 true 时直接执行 await_resume()。
/// await_suspend() 返回 true 表示挂起，false 表示继续执行。
/// 三个接口均允许抛异常。
///
/// @warning await_suspend() 返回 false 或抛异常时，不得留下有效的恢复回调，
///          也不得已经提交恢复通知。
template <typename A>
concept CoroOperation = requires(std::remove_reference_t<A>& op, CoroToken token) {
    { op.await_ready() } -> std::same_as<bool>;
    { op.await_suspend(token) } -> std::same_as<bool>;
    op.await_resume();
};

/// @brief 将 TFL 自定义操作转换为标准 Awaiter。
///
/// 框架保存当前 Coroutine 的恢复句柄并维护挂起状态。
/// 自定义操作通过 CoroToken 提交恢复请求，不直接访问 Coro。
///
/// await_suspend() 先登记挂起，再启动自定义操作。
/// 操作返回 false 或抛异常时，撤销挂起登记。
/// 操作返回 true 后完成交接：提前通知已到达时直接继续，否则置位 Invoker 返回标志。
///
/// @tparam A 自定义操作类型，可为值类型或引用类型。
/// @warning 空删除器不延长 Coro 的真实生命周期；外部回调必须遵守恢复和销毁时序。
/// @warning 交接后当前 Awaiter 和 Coro 可能已被其他 Worker 销毁，不得再访问。
template <typename A>
class CoroAwaiter final {
    static_assert(CoroOperation<A>, "CoroAwaiter requires a valid CoroOperation");

public:
    /// @brief 绑定当前 Coroutine 并保存自定义操作。
    explicit CoroAwaiter(Coro& coro, A&& op);

    /// @brief 先使 Token 控制块失效，再销毁自定义操作。
    ~CoroAwaiter() noexcept;

    CoroAwaiter(const CoroAwaiter&) = delete;
    CoroAwaiter& operator=(const CoroAwaiter&) = delete;

    /// @brief 检查操作是否已就绪。
    /// @return 已就绪时返回 true，直接执行 await_resume()。
    [[nodiscard]] constexpr bool await_ready() noexcept(noexcept(m_op.await_ready()));

    /// @brief 保存当前 Coroutine 恢复句柄并启动自定义异步操作。
    /// @param handle 当前正在挂起的 Coroutine 句柄。
    /// @return true 表示挂起，false 表示继续执行。
    /// @throws 自定义操作启动时抛出的异常。
    [[nodiscard]] bool await_suspend(std::coroutine_handle<> handle);

    /// @brief 获取自定义操作的完成结果。
    /// @return 原样转发自定义操作的 await_resume() 返回值。
    constexpr decltype(auto) await_resume() noexcept(noexcept(m_op.await_resume()));

private:
    std::shared_ptr<Coro> m_coro;
    A m_op;
};

// ============================================================================
// CoroAwaiter 实现
// ============================================================================

template <typename A>
inline CoroAwaiter<A>::CoroAwaiter(Coro& coro, A&& op)
    : m_coro{std::addressof(coro), [](Coro*) noexcept {}}
    , m_op{std::forward<A>(op)} {}

template <typename A>
inline CoroAwaiter<A>::~CoroAwaiter() noexcept {
    m_coro.reset();
}

template <typename A>
inline constexpr bool CoroAwaiter<A>::await_ready() noexcept(noexcept(m_op.await_ready())) {
    return m_op.await_ready();
}

template <typename A>
inline bool CoroAwaiter<A>::await_suspend(std::coroutine_handle<> handle) {
    m_coro->_prepare_suspend(handle);
    TFL_TRY {
        if (!m_op.await_suspend(CoroToken{m_coro})) {
            m_coro->_cancel_suspend();
            return false;
        }
    } TFL_CATCH_ALL {
        m_coro->_cancel_suspend();
        TFL_RETHROW();
    }

    return m_coro->_commit_suspend();
}

template <typename A>
inline constexpr decltype(auto) CoroAwaiter<A>::await_resume() noexcept(noexcept(m_op.await_resume())) {
    return m_op.await_resume();
}

} // namespace tfl
