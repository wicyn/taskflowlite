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

/// @brief 任务返回值句柄 ── 组合 std::future 并附加 stop_source 协作停止能力。
///
/// @details
/// 由 `Executor::async` 返回，是用户拿到任务结果的唯一通道。内部组合（而非
/// 继承）`std::future<R>`：`std::future` 的析构函数非虚，公有继承会在
/// 切片场景（拷贝/移动到基类、基类指针 delete）下静默泄漏 stop_source 的
/// control block。组合方案以 6 行 forward 换取生命周期可控。
///
/// `m_stop_source` 与任务所属 Topology 共享同一份 control block
/// （`std::stop_source` 是 shared_ptr 语义）：用户调用 `request_stop()`、
/// 框架内部 `_stop_requested()`、用户 callable 拿到的 `stop_token` 三者
/// 看到的是同一个状态，不会出现"用户停了但框架不知道"的语义割裂。
///
/// | 维度          | tfl::Future          | std::future          | std::shared_future   |
/// |---------------|----------------------|----------------------|----------------------|
/// | 拷贝          | ✗                    | ✗                    | ✓                    |
/// | 一次性 get    | ✓                    | ✓                    | 多次可读             |
/// | 协作停止      | ✓ (request_stop)     | ✗                    | ✗                    |
/// | 与 topology   | 共享 stop_source     | 无                   | 无                   |
/// | 逃生口        | native_future()      | —                    | —                    |
///
/// ============================================================================
///  Ownership and lifetime
/// ============================================================================
/// `Future` 是 move-only，与 `std::future` 一致。`m_stop_source` 持有的
/// control block 让 Future 的生命周期可以独立于 Topology——即使任务已结束、
/// Topology 已析构，Future 上的 `stop_requested()` 仍可安全查询（永远反映
/// 最后一次状态），`request_stop()` 也是安全的（无副作用）。

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

    /// @brief 阻塞获取结果；仅可调用一次。
    R get() { return m_future.get(); }

    /// @brief 是否持有有效的共享状态（move-from 后为 false）。
    [[nodiscard]] bool valid() const noexcept { return m_future.valid(); }

    /// @brief 阻塞至结果就绪。
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

    /// @brief 转换为可多次读取的 shared_future（stop 控制能力丢失）。
    std::shared_future<R> share() noexcept { return m_future.share(); }

    /// @brief 取得底层 std::future 的引用——兼容接受 std::future& 的现有 API。
    /// @note 通过此接口操作底层 future 不会触发 stop 控制；仅作为逃生口。
    std::future<R>& native_future() noexcept { return m_future; }
    const std::future<R>& native_future() const noexcept { return m_future; }

    // ========================================================================
    //  停止控制
    // ========================================================================

    /// @brief 取得对应 stop_token；用户可在 callable 内查询或挂 stop_callback。
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
