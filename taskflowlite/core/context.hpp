/// @file context.hpp
/// @brief 任务执行上下文，提供当前 Work、Worker 和 Executor 的运行期访问入口。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-08-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <memory>
#include <string_view>

#include "utility.hpp"
#include "forward.hpp"
#include "executor.hpp"

namespace tfl {

/// @brief 表示任务执行期间绑定的 Work、Worker 和 Executor 执行上下文。
///
/// Context 是 Runtime、SubFlow、Coro 等执行期上下文对象的公共基类，统一保存
/// 当前 Work、当前实际执行 Worker 和所属 Executor，并提供任务信息及停止状态
/// 访问接口。
///
/// Work 与 Executor 在一次绑定生命周期内保持不变；Worker 表示当前实际执行
/// 该任务的工作线程，Coroutine 挂起后可以由其它 Worker 恢复，因此允许重新绑定。
///
/// Context 支持默认构造和延迟绑定，以便 Coro 直接作为 CoroInvoker 的成员保存。
///
/// Context 本身不拥有任何执行对象。
///
/// @warning worker() 返回的引用只在当前任务实际执行期间有效，不得跨 Coroutine
///          suspension point 保存或传递到其他线程。
class Context : public Immovable<Context> {
    friend class TaskGroup;
    friend class ScopedExceptionAnchor;

public:
    /// @brief 获取当前实际执行任务的 Worker。
    /// @return 当前 Worker 的可变引用。
    [[nodiscard]] Worker& worker() noexcept;

    /// @brief 获取当前实际执行任务的 Worker。
    /// @return 当前 Worker 的只读引用。
    [[nodiscard]] const Worker& worker() const noexcept;

    /// @brief 获取负责当前任务调度的 Executor。
    /// @return 外部拥有的 Executor 可变引用。
    [[nodiscard]] Executor& executor() noexcept;

    /// @brief 获取负责当前任务调度的 Executor。
    /// @return 外部拥有的 Executor 只读引用。
    [[nodiscard]] const Executor& executor() const noexcept;

    /// @brief 查询当前执行上下文是否收到协作式停止请求。
    /// @return 当前 Topology 或其任一父 Topology 已请求停止时返回 true。
    [[nodiscard]] bool stop_requested() const noexcept;

    /// @brief 获取当前执行 Work 的任务类型。
    /// @return 当前 Work 对应的 TaskType。
    [[nodiscard]] TaskType type() const noexcept;

    /// @brief 获取当前执行 Work 的调试名称。
    /// @return 当前 Work 的名称只读视图；未设置名称时返回空视图。
    [[nodiscard]] std::string_view name() const noexcept;

protected:
    Work* m_work{nullptr};
    Worker* m_worker{nullptr};
    Executor* m_executor{nullptr};

    /// @brief 默认构造尚未绑定执行环境的 Context。
    Context() noexcept = default;

    /// @brief 构造并立即绑定完整执行环境。
    ///
    /// 主要用于 Runtime、SubFlow 等同步执行上下文。
    Context(Work& work, Worker& worker, Executor& executor) noexcept
        : m_work{std::addressof(work)}
        , m_worker{std::addressof(worker)}
        , m_executor{std::addressof(executor)} {}

    /// @brief 首次绑定完整执行环境。
    ///
    /// 主要用于默认构造的 Coro，在首次 invoke() 时绑定。
    ///
    /// @pre 当前 Context 尚未绑定任何执行对象。
    void _rebind(Work& work, Worker& worker, Executor& executor) noexcept {
        m_work = std::addressof(work);
        m_worker = std::addressof(worker);
        m_executor = std::addressof(executor);
    }

    /// @brief 获取当前执行上下文所属的 Work。
    /// @return 当前 Work 的可变引用。
    [[nodiscard]] Work& work() noexcept;

    /// @brief 获取当前执行上下文所属的 Work。
    /// @return 当前 Work 的只读引用。
    [[nodiscard]] const Work& work() const noexcept;

};

// ============================================================================
// Context 实现
// ============================================================================

inline Work& Context::work() noexcept {
    TFL_ASSERT(m_work);
    return *m_work;
}

inline const Work& Context::work() const noexcept {
    TFL_ASSERT(m_work);
    return *m_work;
}

inline Worker& Context::worker() noexcept {
    TFL_ASSERT(m_worker);
    return *m_worker;
}

inline const Worker& Context::worker() const noexcept {
    TFL_ASSERT(m_worker);
    return *m_worker;
}

inline Executor& Context::executor() noexcept {
    TFL_ASSERT(m_executor);
    return *m_executor;
}

inline const Executor& Context::executor() const noexcept {
    TFL_ASSERT(m_executor);
    return *m_executor;
}

inline bool Context::stop_requested() const noexcept {
    TFL_ASSERT(m_work);
    return m_work->m_topology->_stop_requested();
}

inline TaskType Context::type() const noexcept {
    TFL_ASSERT(m_work);
    return m_work->type();
}

inline std::string_view Context::name() const noexcept {
    TFL_ASSERT(m_work);
    return m_work->_name();
}

} // namespace tfl
