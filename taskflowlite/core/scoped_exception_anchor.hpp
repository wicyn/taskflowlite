/// @file scoped_exception_anchor.hpp
/// @brief 词法作用域显式异常锚点。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-08-30
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <memory>

#include "context.hpp"
#include "work.hpp"

namespace tfl {

/// @brief 在当前执行上下文的词法作用域内建立显式异常锚点。
///
/// `ScopedExceptionAnchor` 借用 `Context` 当前绑定的 `Work`。构造时尝试设置
/// `Work::Control::EXPLICIT_ANCHOR`；只有首次设置该标志的对象取得锚点所有权，
/// 析构时也只有该对象负责清除标志。
///
/// 因此同一 `Work` 上严格嵌套的多个 `ScopedExceptionAnchor` 不会提前移除
/// 外层已经建立的异常锚点。
///
/// 显式异常锚点用于截断子任务异常的默认向上传播，并将异常归档到当前 `Work`；
/// 对应等待接口随后负责重新抛出已经归档的异常，由调用方通过普通 try/catch 处理。
///
/// @code
/// try {
///     ScopedExceptionAnchor anchor{subflow};
///
///     subflow.run();
///     subflow.wait();
/// }
/// catch (...) {
///     // 处理当前异常锚点归档并重新抛出的异常。
/// }
/// @endcode
///
/// @warning 必须在启动需要由当前锚点拦截异常的子任务之前构造，并保持存活直到
///          这些子任务全部完成。
/// @warning 同一 `Work` 上的多个锚点必须严格嵌套，不得跨线程或以非嵌套方式重叠。
/// @warning `Context` 及其绑定的 `Work` 必须比当前对象存活更久。
class [[nodiscard("ScopedExceptionAnchor must be bound to a variable and kept alive for the entire exception scope")]]
ScopedExceptionAnchor final : public Immovable<ScopedExceptionAnchor> {
public:
    /// @brief 为指定执行上下文的当前 Work 建立显式异常锚点。
    /// @param context 当前执行上下文。
    explicit ScopedExceptionAnchor(Context& context) noexcept;

    /// @brief 清理由当前对象负责设置的显式异常锚点。
    ~ScopedExceptionAnchor() noexcept;

private:
    /// @brief 尝试在指定 Work 上取得显式异常锚点所有权。
    /// @param work 目标 Work。
    explicit ScopedExceptionAnchor(Work& work) noexcept;

    /// @brief 当前对象拥有锚点清理权的 Work；为空表示锚点已由外层对象持有。
    Work* m_owned_work{nullptr};
};

// ============================================================================
// ScopedExceptionAnchor 实现
// ============================================================================

inline ScopedExceptionAnchor::ScopedExceptionAnchor(Context& context) noexcept
    : ScopedExceptionAnchor{context.m_work} {
}

inline ScopedExceptionAnchor::ScopedExceptionAnchor(Work& work) noexcept {
    const auto previous = work.m_control.fetch_or(Work::Control::EXPLICIT_ANCHOR, std::memory_order_relaxed);

    if (!(previous & Work::Control::EXPLICIT_ANCHOR)) {
        m_owned_work = std::addressof(work);
    }
}

inline ScopedExceptionAnchor::~ScopedExceptionAnchor() noexcept {
    if (m_owned_work) {
        m_owned_work->m_control.fetch_and(~Work::Control::EXPLICIT_ANCHOR, std::memory_order_relaxed);
    }
}

}  // namespace tfl
