/// @file coro_token.hpp
/// @brief CoroToken —— Coroutine 的外部恢复与停止状态控制。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-10-21
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <memory>

#include "coro.hpp"

namespace tfl {

/// @brief Coroutine 的非拥有型外部控制令牌。
///
/// 自定义 Coroutine 操作通过 CoroToken 提交恢复请求和查询停止状态，
/// 不直接访问 Coro 的内部执行上下文。
///
/// @warning 不持有 Coro 所有权，使用期间必须保证 Coro 仍然存活。
/// @warning resume() 仅提交恢复请求，不保证 Coroutine 已经执行。
class CoroToken final {
    template <typename>
    friend class CoroAwaiter;

public:
    CoroToken(const CoroToken&) noexcept = default;
    CoroToken(CoroToken&&) noexcept = default;

    CoroToken& operator=(const CoroToken&) noexcept = default;
    CoroToken& operator=(CoroToken&&) noexcept = default;

    /// @brief 请求恢复当前 Coroutine。
    /// @return 成功提交恢复请求时返回 true。
    [[nodiscard]] bool resume() const;

    /// @brief 查询当前 Coroutine 所属任务是否收到停止请求。
    /// @return 收到停止请求时返回 true。
    [[nodiscard]] bool stop_requested() const noexcept;

private:
    explicit CoroToken(Coro& coro) noexcept;

    Coro* m_coro;
};

// ============================================================================
// CoroToken 实现
// ============================================================================

inline CoroToken::CoroToken(Coro& coro) noexcept
    : m_coro{std::addressof(coro)} {}

inline bool CoroToken::resume() const {
    return m_coro->resume();
}

inline bool CoroToken::stop_requested() const noexcept {
    return m_coro->stop_requested();
}

} // namespace tfl
