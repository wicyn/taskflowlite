/// @file future.hpp
/// @brief Future ── 任务返回值通道 + 协作式停止控制
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-04
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <chrono>
#include <future>
#include <stop_token>

#include "utility.hpp"
namespace tfl {

/// @brief 任务返回值句柄 —— 组合 std::future<R> 并附加协作式停止控制。
///
/// 由 Executor::async / Runtime::async 返回，MoveOnly。组合而非继承 std::future
/// （其析构非虚），以 6 行 forward 换取生命周期可控。
/// m_stop_source 与任务 Topology 共享 control block：request_stop()、
/// 框架内部 _stop_requested() 和用户 stop_token 三者同源，语义一致。
///
/// 提供 native_future() 逃生口以兼容现有 std::future& API。
/// Future 生命周期可独立于 Topology，任务结束后仍可安全查询 stop_requested()。

template <typename R>
class Future : public MoveOnly<Future<R>> {
    friend class Executor;
    friend class Runtime;
public:
    // ========================================================================
    //  Construction / move
    // ========================================================================

    Future() = default;

    Future(Future&&) noexcept = default;
    Future& operator=(Future&&) noexcept = default;

    // Why: std::future 不可拷贝；Future 镜像该约束，避免双方对同一 promise 误读
    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;

    // ========================================================================
    //  std::future 接口转发
    // ========================================================================

    /// @brief 阻塞等待结果并从 std::future 移动取出；仅可调用一次，调用后 valid() 返回 false。
    /// @return callable 的返回值 R（移动语义）。
    R get() { return m_future.get(); }

    /// @brief 是否持有有效的共享状态，move-from 或已 get() 后返回 false。
    [[nodiscard]] bool valid() const noexcept { return m_future.valid(); }

    /// @brief 阻塞当前线程直到关联任务完成并设置结果。
    void wait() const { m_future.wait(); }

    /// @brief 阻塞最多 d；返回 ready / timeout / deferred。
    template <typename Rep, typename Period>
    std::future_status wait_for(const std::chrono::duration<Rep, Period>& d) const {
        return m_future.wait_for(d);
    }

    /// @brief 阻塞至 tp；返回 ready / timeout / deferred。
    template <typename Clock, typename Duration>
    std::future_status wait_until(const std::chrono::time_point<Clock, Duration>& tp) const {
        return m_future.wait_until(tp);
    }

    /// @brief 将内部 std::future 转换为可多次读取的 shared_future。
    /// @return std::shared_future<R>，可被多处 wait/get。
    /// @note 调用后本 Future 不再拥有 valid() 状态；stop 控制能力丢失（m_stop_source 不随 share 传递）。
    [[nodiscard]] std::shared_future<R> share() noexcept { return m_future.share(); }

    /// @brief 取得底层 std::future 的引用——兼容接受 std::future& 的现有 API。
    /// @note 通过此接口操作底层 future 不会触发 stop 控制；仅作为逃生口。
    [[nodiscard]] std::future<R>& native_future() noexcept { return m_future; }
    const std::future<R>& native_future() const noexcept { return m_future; }

    // ========================================================================
    //  停止控制
    // ========================================================================

    /// @brief 获取与此 Future 关联的 stop_token，用户可在 callable 内轮询 stop_requested() 或挂 stop_callback。
    /// @return std::stop_token，与 m_stop_source 共享 control block。
    [[nodiscard]] std::stop_token stop_token() const noexcept {
        return m_stop_source.get_token();
    }

    /// @brief 是否已请求停止；线程安全。
    [[nodiscard]] bool stop_requested() const noexcept {
        return m_stop_source.stop_requested();
    }

    /// @brief stop_source 是否仍可请求停止。
    /// @details 默认构造的 Future（无关联任务）返回 false——与
    ///          `std::stop_source{}` 默认无状态语义一致。
    [[nodiscard]] bool stop_possible() const noexcept {
        return m_stop_source.stop_possible();
    }

    /// @brief 请求任务停止；幂等。
    /// @return 仅首次调用返回 true；之后返回 false。
    /// @note 框架的 `_stop_requested()` 与用户 callable 拿到的 stop_token
    ///       立即可见此次请求——三者共享 control block。
    bool request_stop() noexcept {
        return m_stop_source.request_stop();
    }

private:
    /// @brief 由 Executor::async 内部构造；外部代码不应直接调用。
    /// @param fut    Promise 派生的标准 future
    /// @param ss     与任务 Topology 共享的 stop_source（拷贝即共享 control block）
    Future(std::future<R>&& fut, std::stop_source ss) noexcept
        : m_future{std::move(fut)}, m_stop_source{std::move(ss)} {}

    // Why: 顺序对齐 std::future 与 std::stop_source 的析构链
    //      —— stop_source 后析构，确保 Topology 端 stop_callback 解链发生在
    //      future 端 promise 解绑之后；规避销毁顺序相关的悬挂回调
    std::future<R>    m_future;
    std::stop_source  m_stop_source;
};

} // namespace tfl
