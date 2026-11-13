/// @file coro_token.hpp
/// @brief CoroToken —— Coroutine 的外部恢复与停止状态控制。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-10-21
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <memory>
#include <utility>

#include "coro.hpp"

namespace tfl {

/// @brief Coroutine 的非拥有型外部控制令牌。
///
/// 自定义 Coroutine 操作通过 CoroToken 提交恢复请求和查询停止状态，
/// 不直接访问 Coro 的内部执行上下文。
///
/// Token 弱引用当前 CoroAwaiter 持有的 shared_ptr 控制块；
/// Awaiter 结束后，若没有其他线程持有已锁定的强引用，Token 自动失效。
/// shared_ptr 使用空删除器，不持有 Coro 对象本身的所有权。
///
/// @warning weak_ptr 只能保护控制块；已经 lock() 的线程仍需遵守 Coro 的生命周期与恢复协议。
/// @warning 外部恢复操作必须管理好回调的取消或完成同步，避免已 lock() 的回调访问已销毁的 Coro。
/// @warning resume() 仅提交恢复请求，不保证 Coroutine 已经执行。
class CoroToken final {
    template <typename>
    friend class CoroAwaiter;

public:
    CoroToken(const CoroToken&) noexcept = default;
    CoroToken(CoroToken&&) noexcept = default;

    CoroToken& operator=(const CoroToken&) noexcept = default;
    CoroToken& operator=(CoroToken&&) noexcept = default;

    /// @brief 请求恢复当前 Coroutine；本次 Awaiter 已失效时返回 false。
    /// @return 成功提交恢复请求时返回 true。
    [[nodiscard]] bool resume() noexcept;

    /// @brief 查询当前 Coroutine 所属任务是否收到停止请求。
    /// @return Awaiter 已失效或所属任务收到停止请求时返回 true。
    [[nodiscard]] bool stop_requested() const noexcept;

private:
    explicit CoroToken(std::weak_ptr<Coro> coro) noexcept;

    std::weak_ptr<Coro> m_coro;
};

// ============================================================================
// CoroToken 实现
// ============================================================================

inline CoroToken::CoroToken(std::weak_ptr<Coro> coro) noexcept
    : m_coro{std::move(coro)} {}

inline bool CoroToken::resume() noexcept {
    if (auto coro = m_coro.lock()) {
        return coro->resume();
    }
    return false;
}

inline bool CoroToken::stop_requested() const noexcept {
    if (auto coro = m_coro.lock()) {
        return coro->stop_requested();
    }
    return true;
}

} // namespace tfl
