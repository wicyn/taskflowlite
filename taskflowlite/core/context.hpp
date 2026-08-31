/// @file context.hpp
/// @brief 任务执行上下文，提供当前 Worker 和 Executor 访问入口。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-08-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include "utility.hpp"
#include "forward.hpp"
#include "executor.hpp"

namespace tfl {

/// @brief 表示一次任务回调所绑定的 `Work`、`Worker` 和 `Executor` 执行上下文。
///
/// `Context` 是 `Runtime`、动态子图等执行期上下文对象的公共基类，统一保存当前
/// Work、Worker 和 Executor 的非拥有引用，并提供对应执行对象的访问接口。
///
/// Context 本身不拥有任何执行对象，其生命周期严格受当前任务回调约束。
///
/// @warning 该对象及其暴露的引用只在当前任务回调和所属 Worker 线程内有效，
///          不得保存、跨线程传递或延长到当前回调结束之后。
class Context : public Immovable<Context> {
    friend class TaskGroup;
    friend class ScopedExceptionAnchor;
public:
    /// @brief 获取当前执行任务绑定的 Worker。
    /// @return 当前 Worker 的可变引用。
    /// @warning 返回引用不得保存到当前回调之外或传递到其他线程。
    [[nodiscard]] Worker& worker() noexcept;

    /// @brief 获取当前执行任务绑定的 Worker。
    /// @return 当前 Worker 的只读引用。
    /// @warning 返回引用不得保存到当前回调之外或传递到其他线程。
    [[nodiscard]] const Worker& worker() const noexcept;

    /// @brief 获取负责当前任务调度的 Executor。
    /// @return 外部拥有的 Executor 可变引用。
    [[nodiscard]] Executor& executor() noexcept;

    /// @brief 获取负责当前任务调度的 Executor。
    /// @return 外部拥有的 Executor 只读引用。
    [[nodiscard]] const Executor& executor() const noexcept;

protected:
    Work& m_work;
    Worker& m_worker;
    Executor& m_executor;

    /// @brief 构造绑定当前任务执行状态的 Context。
    /// @param work 当前正在执行的 Work。
    /// @param worker 当前执行该 Work 的 Worker。
    /// @param executor 当前 Work 所属 Executor。
    Context(Work& work, Worker& worker, Executor& executor) noexcept
        : m_work{work}
        , m_worker{worker}
        , m_executor{executor} {}
};

// ============================================================================
// Context 实现
// ============================================================================

inline Worker& Context::worker() noexcept {
    return m_worker;
}

inline const Worker& Context::worker() const noexcept {
    return m_worker;
}

inline Executor& Context::executor() noexcept {
    return m_executor;
}

inline const Executor& Context::executor() const noexcept {
    return m_executor;
}

} // namespace tfl
