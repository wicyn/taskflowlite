/// @file work_invokers.hpp
/// @brief Work Payload Invoker 及其调用实现。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-04-20
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn


#pragma once
#include <cmath>
#include <cstring>
#include <functional>

#include "work.hpp"
#include "runtime.hpp"
#include "branch.hpp"
#include "jump.hpp"
#include "subflow.hpp"
#include "executor.hpp"
#include "d2_render.hpp"
#include "work_storage.hpp"
#include "macros.hpp"
namespace tfl {

/// @brief 可选的 Work 单轮执行一致性检查。
///
/// 启用后，BEGIN 在一轮执行首次真正进入 callable 前设置 `Control::EXECUTION`，
/// END 在最终 tear-down 前清除该位；Runtime/SubFlow/Module 的挂起与恢复阶段保持
/// EXECUTION 持续置位。重复 BEGIN 或缺失 BEGIN 的 END 都视为调度协议破坏并终止进程。
///
/// 关闭检查时两个宏退化为空操作，不引入运行期开销。
#if TFL_ENABLE_WORK_EXECUTION_CHECK

#define TFL_WORK_EXECUTION_BEGIN(work)                                                      \
do {                                                                                    \
        auto& _tfl_work = (work);                                                           \
        if (_tfl_work.m_control.fetch_or(Work::Control::EXECUTION, std::memory_order_relaxed) & Work::Control::EXECUTION) [[unlikely]] { \
            std::terminate();                                                               \
    }                                                                                   \
} while (false)

#define TFL_WORK_EXECUTION_END(work)                                                        \
    do {                                                                                    \
        auto& _tfl_work = (work);                                                           \
        if (!(_tfl_work.m_control.fetch_and(~Work::Control::EXECUTION, std::memory_order_relaxed) & Work::Control::EXECUTION)) [[unlikely]] { \
            std::terminate();                                                               \
    }                                                                                   \
} while (false)

#else

#define TFL_WORK_EXECUTION_BEGIN(work) ((void)0)
#define TFL_WORK_EXECUTION_END(work)   ((void)0)

#endif

    // ============================================================================
    // TaskType::Placeholder
    // ============================================================================

    /// @brief 为占位节点提供固定 `TaskType::Placeholder` 和 D2 渲染语义的无状态基类。
    ///
    /// PlaceholderWork 不保存 callable、参数或运行期状态，也不拥有外部 `Work`。
    /// 具体 Invoker 只需定义执行行为；静态依赖传播、父节点计数和生命周期仍由
    /// `Executor` 与外部 `Work` 负责。
    ///
    /// @note 该类型仅作为 Payload 内部实现基类，不表示独立可调度对象。
    class PlaceholderWork {
    public:
        static constexpr TaskType TYPE = TaskType::Placeholder;

        void dump(const Work& w, std::ostream& os) const {
            D2Renderer::render_work(os, w, "circle", "#e5e7eb", "#9ca3af", "#374151", "8", "3");
        }

    protected:
        explicit PlaceholderWork() noexcept = default;
    };

// ============================================================================
// TaskType::Basic 家族
// ============================================================================

/// @brief 为普通同步 callable 节点提供公共存储、固定 `TaskType::Basic` 和 D2 渲染语义。
///
/// BasicWork 按值保存 callable，但不负责停止检查、Semaphore、Observer、
/// 异常归档或依赖传播；这些执行协议由具体 Basic Invoker 与 Executor 完成。
///
/// @tparam F 节点按值拥有的 callable 类型。
template <typename F>
class BasicWork {
public:
    static constexpr TaskType TYPE = TaskType::Basic;

    template <typename U>
        requires std::constructible_from<F, U&&>
    explicit BasicWork(U&& f)
        : m_func{std::forward<U>(f)} {}

    void dump(const Work& w, std::ostream& os) const {
        D2Renderer::render_work(os, w, "rectangle", "#f5f5f5", "#9ca3af", "#1f2937", "8");
    }

protected:
    TFL_NO_UNIQUE_ADDRESS F m_func;
};


/// @brief 为单目标条件分支节点提供 callable 存储、`TaskType::Branch` 和 D2 渲染语义。
///
/// 该内部基类按值保存 callable，不保存本次分支选择结果；具体 Invoker
/// 在执行期创建栈绑定 `Branch`，由用户 callable 选择至多一个后继，再交由
/// Executor 的 Branch tear-down 路径传播依赖。
///
/// @tparam F 节点按值拥有的 callable 类型。
template <typename F>
class BranchWork {
public:
    static constexpr TaskType TYPE = TaskType::Branch;

    template <typename U>
        requires std::constructible_from<F, U&&>
    explicit BranchWork(U&& f)
        : m_func{std::forward<U>(f)} {}

    void dump(const Work& w, std::ostream& os) const {
        D2Renderer::render_work(os, w, "diamond", "#dbeafe", "#3b82f6", "#1e3a5f", "8");
    }

protected:
    TFL_NO_UNIQUE_ADDRESS F m_func;
};


/// @brief 为多目标条件分支节点提供 callable 存储、`TaskType::MultiBranch` 和 D2 渲染语义。
///
/// 该内部基类按值保存 callable；具体 Invoker 在执行期创建栈绑定
/// `MultiBranch`，允许用户选择零个或多个后继，并由专用 tear-down 路径批量传播。
///
/// @tparam F 节点按值拥有的 callable 类型。
template <typename F>
class MultiBranchWork {
public:
    static constexpr TaskType TYPE = TaskType::MultiBranch;

    template <typename U>
        requires std::constructible_from<F, U&&>
    explicit MultiBranchWork(U&& f)
        : m_func{std::forward<U>(f)} {}

    void dump(const Work& w, std::ostream& os) const {
        D2Renderer::render_work(os, w, "hexagon", "#bfdbfe", "#2563eb", "#1e3a5f", "8");
    }

protected:
    TFL_NO_UNIQUE_ADDRESS F m_func;
};


/// @brief 为单目标跳转节点提供 callable 存储、`TaskType::Jump` 和 D2 渲染语义。
///
/// Jump 节点通过专用 tear-down 强制激活至多一个目标，不作为普通 strong predecessor
/// 参与后继 join_counter。该基类只负责 callable 存储和可视化语义。
///
/// @tparam F 节点按值拥有的 callable 类型。
template <typename F>
class JumpWork {
public:
    static constexpr TaskType TYPE = TaskType::Jump;

    template <typename U>
        requires std::constructible_from<F, U&&>
    explicit JumpWork(U&& f)
        : m_func{std::forward<U>(f)} {}

    void dump(const Work& w, std::ostream& os) const {
        D2Renderer::render_work(os, w, "diamond", "#fee2e2", "#ef4444", "#7f1d1d", "8", "5");
    }

protected:
    TFL_NO_UNIQUE_ADDRESS F m_func;
};


/// @brief 为多目标跳转节点提供 callable 存储、`TaskType::MultiJump` 和 D2 渲染语义。
///
/// MultiJump 通过专用 tear-down 强制激活多个目标，不参与普通 strong dependency join。
/// 该基类仅保存 callable，目标集合由执行期栈绑定 `MultiJump` 临时收集。
///
/// @tparam F 节点按值拥有的 callable 类型。
template <typename F>
class MultiJumpWork {
public:
    static constexpr TaskType TYPE = TaskType::MultiJump;

    template <typename U>
        requires std::constructible_from<F, U&&>
    explicit MultiJumpWork(U&& f)
        : m_func{std::forward<U>(f)} {}

    void dump(const Work& w, std::ostream& os) const {
        D2Renderer::render_work(os, w, "hexagon", "#fecaca", "#dc2626", "#7f1d1d", "8", "5");
    }

protected:
    TFL_NO_UNIQUE_ADDRESS F m_func;
};


/// @brief 为可动态派生任务的 Runtime 节点提供 callable 存储、`TaskType::Runtime` 和 D2 渲染语义。
///
/// 该基类不处理挂起与恢复；具体 Runtime Invoker 通过 `Properties::PREEMPTED` 和
/// `m_join_counter` 管理 child 生命周期，并向用户 callable 注入栈绑定 `Runtime&`。
///
/// @tparam F 节点按值拥有的 callable 类型。
template <typename F>
class RuntimeWork {
public:
    static constexpr TaskType TYPE = TaskType::Runtime;

    template <typename U>
        requires std::constructible_from<F, U&&>
    explicit RuntimeWork(U&& f)
        : m_func{std::forward<U>(f)} {}

    void dump(const Work& w, std::ostream& os) const {
        D2Renderer::render_work(os, w, "rectangle", "#fce4ec", "#e57373", "#6d1b1b", "30");
    }

protected:
    TFL_NO_UNIQUE_ADDRESS F m_func;
};



/// @brief 为内部持有或借用 Graph 的 Invoker 提供统一 `TaskType::Graph` 与 D2 图渲染语义。
///
/// 采用 CRTP 从 `Derived::graph()` 获取实际 Graph；构造期通过 static_assert 要求派生类
/// 同时提供 noexcept 的 const / non-const `graph()`。Graph 的所有权策略由派生 Invoker
/// 决定，GraphWork 本身不保存图对象。
///
/// @tparam Derived 提供 `graph()` 访问器的具体 Invoker 类型。
template <typename Derived>
class GraphWork {
public:
    static constexpr TaskType TYPE = TaskType::Graph;

    void dump(const Work& w, std::ostream& os) const {
        const auto& derived = static_cast<const Derived&>(*this);
        D2Renderer::render_graph(os, w, to_string(TYPE), derived.graph());
    }

protected:
    explicit GraphWork() noexcept {
        static_assert(requires(Derived& derived, const Derived& const_derived) {
            { derived.graph() } noexcept -> std::same_as<Graph&>;
            { const_derived.graph() } noexcept -> std::same_as<const Graph&>;
        }, "Derived must provide graph() overloads");
    }

};



// ============================================================================
// 派生任务实现
// ============================================================================

/// @brief 静态图中只参与依赖传播、不执行用户 callable 的占位 Invoker。
///
/// 节点固定为 strong predecessor；invoke 不执行任何用户逻辑，直接进入普通静态
/// `_tear_down_task()`，因此仍完整参与 join_counter、后继传播和父节点 slot 守恒。
///
/// @note 不拥有外部资源，也不建立独立 Topology。
class PlaceholderInvoker final : public PlaceholderWork {
public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::STRONG;
    static constexpr Work::Control::type CONTROL = Work::Control::NONE;

    PlaceholderInvoker() noexcept = default;

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) noexcept {
        exe._tear_down_task(w, wr, cache);
    }
};

/// @brief 执行静态图中的普通同步 callable，并完成标准静态节点生命周期。
///
/// 执行前检查继承停止请求并尝试获取 Semaphore；真正进入 callable 后触发 Observer
/// before/after，捕获用户异常并交给 `Work::_process_exception()` 归档，最后释放配置的
/// Semaphore 并进入普通 `_tear_down_task()` 传播 strong dependency。
///
/// @tparam F 节点按值拥有的 callable 类型。
template <typename F>
class BasicInvoker final : public BasicWork<F> {
    using Base = BasicWork<F>;

    using Base::m_func;

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::STRONG;
    static constexpr Work::Control::type CONTROL = Work::Control::NONE;

    template <typename U>
        requires std::constructible_from<F, U&&>
    explicit BasicInvoker(U&& f)
        : Base{std::forward<U>(f)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        if (w._stop_requested()) [[unlikely]] {
            exe._schedule_parent(w.m_parent, wr, cache);
            return;
        }

        // 执行前获取 Semaphore；失败时当前 Work 进入 waiter，本次 invoke 立即让出 Worker。
        if (w.m_semaphores && !w.m_semaphores->acquires.empty()) {
            SmallVector<Work*> waiters;
            if (!w._try_acquire_semaphores(waiters)) {
                exe._schedule_from_semaphore(wr, waiters);
                return;
            }
        }

        TFL_WORK_EXECUTION_BEGIN(w);

        w._notify_before(wr);

        try {
            std::invoke(m_func);
        } catch (...) {
            w._process_exception();
        }

        w._notify_after(wr);

        // 执行完成后释放配置的 Semaphore，并重新调度本次被解冻的 waiter。
        if (w.m_semaphores && !w.m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            w._release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }

        TFL_WORK_EXECUTION_END(w);
        exe._tear_down_task(w, wr, cache);
    }
};


/// @brief 执行静态单目标条件分支，并把用户选择的目标交给 Branch 专用 tear-down。
///
/// callable 执行期间注入栈绑定 `Branch`；用户可选择至多一个后继。节点本身仍是
/// strong predecessor，执行前后遵循停止、Semaphore、Observer 和异常归档协议，
/// 完成后由 `_tear_down_branch_task()` 仅向本次选中的目标传播到达。
///
/// @tparam F 节点按值拥有的 callable 类型。
template <typename F>
class BranchInvoker final : public BranchWork<F> {
    using Base = BranchWork<F>;

    using Base::m_func;

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::STRONG;
    static constexpr Work::Control::type CONTROL = Work::Control::NONE;

    template <typename U>
        requires std::constructible_from<F, U&&>
    explicit BranchInvoker(U&& f)
        : Base{std::forward<U>(f)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        if (w._stop_requested()) [[unlikely]] {
            exe._schedule_parent(w.m_parent, wr, cache);
            return;
        }

        // 执行前获取 Semaphore；失败时当前 Work 进入 waiter，本次 invoke 立即让出 Worker。
        if (w.m_semaphores && !w.m_semaphores->acquires.empty()) {
            SmallVector<Work*> waiters;
            if (!w._try_acquire_semaphores(waiters)) {
                exe._schedule_from_semaphore(wr, waiters);
                return;
            }
        }

        TFL_WORK_EXECUTION_BEGIN(w);

        w._notify_before(wr);

        Branch branch{w, wr, exe};

        try {
            std::invoke(m_func, branch);
        } catch (...) {
            w._process_exception();
        }

        w._notify_after(wr);

        // 执行完成后释放配置的 Semaphore，并重新调度本次被解冻的 waiter。
        if (w.m_semaphores && !w.m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            w._release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }

        TFL_WORK_EXECUTION_END(w);
        exe._tear_down_branch_task(w, wr, cache, branch.m_target);
    }
};

/// @brief 执行静态多目标条件分支，并向本次选择的零个或多个后继传播依赖。
///
/// callable 执行期间注入栈绑定 `MultiBranch` 以收集目标集合。节点作为 strong
/// predecessor 参与普通 join；完成后由 `_tear_down_multi_branch_task()` 处理多个
/// ready target 的 cache 接力、父 slot 扩展和批量调度。
///
/// @tparam F 节点按值拥有的 callable 类型。
template <typename F>
class MultiBranchInvoker final : public MultiBranchWork<F> {
    using Base = MultiBranchWork<F>;

    using Base::m_func;

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::STRONG;
    static constexpr Work::Control::type CONTROL = Work::Control::NONE;

    template <typename U>
        requires std::constructible_from<F, U&&>
    explicit MultiBranchInvoker(U&& f)
        : Base{std::forward<U>(f)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        if (w._stop_requested()) [[unlikely]] {
            exe._schedule_parent(w.m_parent, wr, cache);
            return;
        }

        // 执行前获取 Semaphore；失败时当前 Work 进入 waiter，本次 invoke 立即让出 Worker。
        if (w.m_semaphores && !w.m_semaphores->acquires.empty()) {
            SmallVector<Work*> waiters;
            if (!w._try_acquire_semaphores(waiters)) {
                exe._schedule_from_semaphore(wr, waiters);
                return;
            }
        }

        TFL_WORK_EXECUTION_BEGIN(w);

        w._notify_before(wr);

        MultiBranch branch{w, wr, exe};

        try {
            std::invoke(m_func, branch);
        } catch (...) {
            w._process_exception();
        }

        w._notify_after(wr);

        // 执行完成后释放配置的 Semaphore，并重新调度本次被解冻的 waiter。
        if (w.m_semaphores && !w.m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            w._release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }

        TFL_WORK_EXECUTION_END(w);
        exe._tear_down_multi_branch_task(w, wr, cache, branch.m_targets);
    }
};


/// @brief 执行静态单目标 Jump，并通过专用路径强制激活至多一个目标。
///
/// callable 执行期间注入栈绑定 `Jump`。Jump 的 `PROPERTIES` 不含 STRONG，因此不作为
/// 普通 strong predecessor 参与后继 join_counter；完成后 `_tear_down_jump_task()`
/// 直接按照跳转语义激活目标。执行过程仍支持停止、Semaphore、Observer 和异常归档。
///
/// @tparam F 节点按值拥有的 callable 类型。
template <typename F>
class JumpInvoker final : public JumpWork<F> {
    using Base = JumpWork<F>;

    using Base::m_func;

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::NONE;
    static constexpr Work::Control::type CONTROL = Work::Control::NONE;

    template <typename U>
        requires std::constructible_from<F, U&&>
    explicit JumpInvoker(U&& f)
        : Base{std::forward<U>(f)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        if (w._stop_requested()) [[unlikely]] {
            exe._schedule_parent(w.m_parent, wr, cache);
            return;
        }

        // 执行前获取 Semaphore；失败时当前 Work 进入 waiter，本次 invoke 立即让出 Worker。
        if (w.m_semaphores && !w.m_semaphores->acquires.empty()) {
            SmallVector<Work*> waiters;
            if (!w._try_acquire_semaphores(waiters)) {
                exe._schedule_from_semaphore(wr, waiters);
                return;
            }
        }

        TFL_WORK_EXECUTION_BEGIN(w);

        w._notify_before(wr);

        Jump jump{w, wr, exe};

        try {
            std::invoke(m_func, jump);
        } catch (...) {
            w._process_exception();
        }

        w._notify_after(wr);

        // 执行完成后释放配置的 Semaphore，并重新调度本次被解冻的 waiter。
        if (w.m_semaphores && !w.m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            w._release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }

        TFL_WORK_EXECUTION_END(w);
        exe._tear_down_jump_task(w, wr, cache, jump.m_target);
    }
};


/// @brief 执行静态 MultiJump，并通过专用路径强制激活本次选择的多个目标。
///
/// callable 执行期间注入栈绑定 `MultiJump` 收集目标。节点不形成普通 strong dependency；
/// `_tear_down_multi_jump_task()` 负责清零目标运行期 join 状态、cache 接力及额外目标调度。
/// 执行过程仍支持停止、Semaphore、Observer 和异常归档。
///
/// @tparam F 节点按值拥有的 callable 类型。
template <typename F>
class MultiJumpInvoker final : public MultiJumpWork<F> {
    using Base = MultiJumpWork<F>;

    using Base::m_func;

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::NONE;
    static constexpr Work::Control::type CONTROL = Work::Control::NONE;

    template <typename U>
        requires std::constructible_from<F, U&&>
    explicit MultiJumpInvoker(U&& f)
        : Base{std::forward<U>(f)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        if (w._stop_requested()) [[unlikely]] {
            exe._schedule_parent(w.m_parent, wr, cache);
            return;
        }

        // 执行前获取 Semaphore；失败时当前 Work 进入 waiter，本次 invoke 立即让出 Worker。
        if (w.m_semaphores && !w.m_semaphores->acquires.empty()) {
            SmallVector<Work*> waiters;
            if (!w._try_acquire_semaphores(waiters)) {
                exe._schedule_from_semaphore(wr, waiters);
                return;
            }
        }

        TFL_WORK_EXECUTION_BEGIN(w);

        w._notify_before(wr);

        MultiJump jump{w, wr, exe};

        try {
            std::invoke(m_func, jump);
        } catch (...) {
            w._process_exception();
        }

        w._notify_after(wr);

        // 执行完成后释放配置的 Semaphore，并重新调度本次被解冻的 waiter。
        if (w.m_semaphores && !w.m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            w._release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }

        TFL_WORK_EXECUTION_END(w);
        exe._tear_down_multi_jump_task(w, wr, cache, jump.m_targets);
    }
};



/// @brief 执行静态 Runtime callable，并管理动态 child 导致的挂起与恢复。
///
/// 首次进入时完成停止/Semaphore 检查并设置 `PREEMPTED | IMPLICIT_ANCHOR`，随后以一个
/// 基准 join slot 包围用户 callable。若 callable 派生 child，当前 Work 保持 EXECUTION
/// 和 PREEMPTED 状态退出本次 invoke；最后一个 child 归还 slot 后重新调度父 Work，
/// 恢复路径完成 Observer after、Semaphore release 和普通静态 tear-down。
///
/// Runtime 执行窗口中的 IMPLICIT_ANCHOR 用于承接动态子链异常，最终完成时一并清除。
///
/// @tparam F 节点按值拥有的 Runtime callable 类型。
template <typename F>
class RuntimeInvoker final : public RuntimeWork<F> {
    using Base = RuntimeWork<F>;

    using Base::m_func;

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::STRONG;
    static constexpr Work::Control::type CONTROL = Work::Control::NONE;

    template <typename U>
        requires std::constructible_from<F, U&&>
    explicit RuntimeInvoker(U&& f)
        : Base{std::forward<U>(f)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        // 首次进入：尚未因动态 child / 子图进入 PREEMPTED 挂起状态。
        if ((w.m_properties & Work::Properties::PREEMPTED) == 0) {
            if (w._stop_requested()) [[unlikely]] {
                exe._schedule_parent(w.m_parent, wr, cache);
                return;
            }

            // 执行前获取 Semaphore；失败时当前 Work 进入 waiter，本次 invoke 立即让出 Worker。
            if (w.m_semaphores && !w.m_semaphores->acquires.empty()) {
                SmallVector<Work*> waiters;
                if (!w._try_acquire_semaphores(waiters)) {
                    exe._schedule_from_semaphore(wr, waiters);
                    return;
                }
            }

            TFL_WORK_EXECUTION_BEGIN(w);

            w.m_properties |= Work::Properties::PREEMPTED | Work::Properties::IMPLICIT_ANCHOR;
            w.m_join_counter.fetch_add(1, std::memory_order_release);

            w._notify_before(wr);

            Runtime runtime{w, wr, exe};

            try {
                std::invoke(m_func, runtime);
            } catch (...) {
                w._process_exception();
            }

            if (w.m_join_counter.fetch_sub(1, std::memory_order_acq_rel) != 1) {
                return;
            }
        }

        w._notify_after(wr);

        // 最终完成：当前 Work 的所有动态 child / 子图已经归还等待 slot。
        w.m_properties &= ~(Work::Properties::PREEMPTED | Work::Properties::IMPLICIT_ANCHOR);

        // 执行完成后释放配置的 Semaphore，并重新调度本次被解冻的 waiter。
        if (w.m_semaphores && !w.m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            w._release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }

        TFL_WORK_EXECUTION_END(w);
        exe._tear_down_task(w, wr, cache);
    }
};


/// @brief 执行静态 SubFlow callable，并等待其动态子图完成后恢复当前节点。
///
/// Invoker 按值拥有 callable 和一份内部 `Graph`。首次进入时获取 Semaphore、设置
/// PREEMPTED 并注入栈绑定 `SubFlow`；若 SubFlow 派生子图，`m_join_counter` 负责等待
/// child 完成。恢复后触发 Observer after、清除 PREEMPTED、释放 Semaphore，并进入
/// 普通静态 tear-down。
///
/// @tparam F 节点按值拥有的 SubFlow callable 类型。
template <typename F>
class SubFlowInvoker final : public GraphWork<SubFlowInvoker<F>> {
    using Self = SubFlowInvoker<F>;
    using Base = GraphWork<Self>;

    friend class GraphWork<Self>;

    TFL_NO_UNIQUE_ADDRESS F m_func;
    Graph m_graph;

    [[nodiscard]] Graph& graph() noexcept {
        return m_graph;
    }

    [[nodiscard]] const Graph& graph() const noexcept {
        return m_graph;
    }

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::STRONG;
    static constexpr Work::Control::type CONTROL = Work::Control::NONE;

    template <typename U>
        requires std::constructible_from<F, U&&>
    explicit SubFlowInvoker(U&& f)
        : m_func{std::forward<U>(f)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        // 首次进入：尚未因动态 child / 子图进入 PREEMPTED 挂起状态。
        if ((w.m_properties & Work::Properties::PREEMPTED) == 0) {
            if (w._stop_requested()) [[unlikely]] {
                exe._schedule_parent(w.m_parent, wr, cache);
                return;
            }

            // 执行前获取 Semaphore；失败时当前 Work 进入 waiter，本次 invoke 立即让出 Worker。
            if (w.m_semaphores && !w.m_semaphores->acquires.empty()) {
                SmallVector<Work*> waiters;
                if (!w._try_acquire_semaphores(waiters)) {
                    exe._schedule_from_semaphore(wr, waiters);
                    return;
                }
            }

            TFL_WORK_EXECUTION_BEGIN(w);

            w.m_properties |= Work::Properties::PREEMPTED;
            w.m_join_counter.fetch_add(1, std::memory_order_release);

            w._notify_before(wr);

            SubFlow flow{m_graph, w, wr, exe};

            try {
                std::invoke(m_func, flow);
            } catch (...) {
                w._process_exception();
            }

            if (w.m_join_counter.fetch_sub(1, std::memory_order_acq_rel) != 1) {
                return;
            }
        }

        w._notify_after(wr);

        // 最终完成：当前 Work 的所有动态 child / 子图已经归还等待 slot。
        w.m_properties &= ~Work::Properties::PREEMPTED;

        // 执行完成后释放配置的 Semaphore，并重新调度本次被解冻的 waiter。
        if (w.m_semaphores && !w.m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            w._release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }
        TFL_WORK_EXECUTION_END(w);
        exe._tear_down_task(w, wr, cache);
    }
};

/// @brief 在静态图节点中重复执行一个子 Graph，直到谓词要求停止或执行链中止。
///
/// Invoker 保存 Graph holder 和终止谓词，并缓存 `_set_up_graph()` 得到的 source 数量。
/// 首次进入时执行停止/Semaphore 检查、设置 PREEMPTED 并建立子图运行期上下文；每轮
/// source 完成后当前 Work 被恢复，再检查无 source、异常/停止及谓词条件。继续执行时
/// 重置非 source join_counter，并让最后一个 source 通过 cache 接力，其余 source 入队。
///
/// Observer before/after 包围整个 Module 生命周期，而不是每一轮子图。
///
/// @tparam GhStore Graph holder 的实际存储类型。
/// @tparam P 无参终止谓词类型；返回 true 时结束 Module。
template <typename GhStore, typename P>
class ModuleInvoker final : public GraphWork<ModuleInvoker<GhStore, P>> {
    using Self = ModuleInvoker<GhStore, P>;
    using Base = GraphWork<Self>;

    friend class GraphWork<Self>;

    std::size_t m_num_sources{0};
    TFL_NO_UNIQUE_ADDRESS GhStore m_gh_store;
    TFL_NO_UNIQUE_ADDRESS P m_pred;

    [[nodiscard]] Graph& graph() noexcept {
        auto& graph_holder = detail::borrow(m_gh_store);
        return detail::to_graph(graph_holder);
    }

    [[nodiscard]] const Graph& graph() const noexcept {
        const auto& graph_holder = detail::borrow(m_gh_store);
        return detail::to_graph(graph_holder);
    }

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::STRONG;
    static constexpr Work::Control::type CONTROL = Work::Control::NONE;

    template <typename Ghs, typename V>
    explicit ModuleInvoker(Ghs&& ghs, V&& pred)
        : m_gh_store{std::forward<Ghs>(ghs)}
        , m_pred{std::forward<V>(pred)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        Graph& graph = this->graph();

        // 首次进入。
        if ((w.m_properties & Work::Properties::PREEMPTED) == 0) {
            if (w._stop_requested()) [[unlikely]] {
                exe._schedule_parent(w.m_parent, wr, cache);
                return;
            }

            // acquire 阶段。
            if (w.m_semaphores && !w.m_semaphores->acquires.empty()) [[unlikely]] {
                SmallVector<Work*> waiters;
                if (!w._try_acquire_semaphores(waiters)) {
                    exe._schedule_from_semaphore(wr, waiters);
                    return;
                }
            }

            TFL_WORK_EXECUTION_BEGIN(w);

            w.m_properties |= Work::Properties::PREEMPTED;
            w._notify_before(wr);
            m_num_sources = exe._set_up_graph(graph, w);
        }

        // 无 source、异常/停止或循环条件满足时结束整个 Module。
        if (m_num_sources == 0 || w._should_abort() || w._invoke_predicate(m_pred)) {
            w._notify_after(wr);

            w.m_properties &= ~Work::Properties::PREEMPTED;

            // release 阶段。
            if (w.m_semaphores && !w.m_semaphores->releases.empty()) [[unlikely]] {
                SmallVector<Work*> waiters;
                w._release_semaphores(waiters);
                exe._schedule_from_semaphore(wr, waiters);
            }

            TFL_WORK_EXECUTION_END(w);
            exe._tear_down_task(w, wr, cache);
            return;
        }

        // 建立本轮子图运行期依赖计数。
        exe._reset_graph_join_counters(graph, m_num_sources);

        // 建立本轮所有 source 的完成等待计数。
        w.m_join_counter.store(m_num_sources, std::memory_order_relaxed);

        // 最后一个 source 通过 cache 接力，其余 source 发布到 Worker 队列。
        Work** const data = graph.m_works.data();
        const std::size_t n = m_num_sources - 1;

        cache = data[n];

        if (n != 0) [[likely]] {
            if (n > 1) [[likely]] {
                exe._schedule(wr, data, n);
            } else {
                exe._schedule(wr, data[0]);
            }
        }
    }
};


// ============================================================================
// SilentAsync Invoker 家族
// ============================================================================
//
// SilentAsync Work 不向调用方暴露结果句柄。各 Invoker 自身拥有独立 Topology，执行结束后
// 由 `_tear_down_silent_async_task()` 直接进入节点销毁路径，并归还父 Work slot 或顶层 topology 计数。

/// @brief 执行无外部结果句柄的顶层/组内普通 SilentAsync callable。
///
/// 通过 `TopologyStorage` 按值拥有独立 Topology，并以 `IMPLICIT_ANCHOR` 允许当前节点
/// 承接执行异常。invoke 捕获 callable 异常后直接进入 `_tear_down_silent_async_task()`；
/// SilentAsync 完成后没有 Future 引用需要保留，Work 可立即进入对应销毁路径。
///
/// @tparam F 按值拥有的 callable 类型。
template <typename F>
class SilentAsyncBasicInvoker final : public TopologyStorage, public BasicWork<F> {
    using Base = BasicWork<F>;

    using Base::m_func;

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::IMPLICIT_ANCHOR;
    static constexpr Work::Control::type CONTROL = Work::Control::NONE;

    template <typename U>
        requires std::constructible_from<Base, U&&>
    explicit SilentAsyncBasicInvoker(Topology* parent_topology, Executor* executor, U&& f)
        : TopologyStorage{parent_topology, executor}
        , Base{std::forward<U>(f)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        TFL_WORK_EXECUTION_BEGIN(w);

        try {
            std::invoke(m_func);
        } catch (...) {
            w._process_exception();
        }

        TFL_WORK_EXECUTION_END(w);
        exe._tear_down_silent_async_task(w, wr, cache);
    }
};


/// @brief 执行无结果句柄的 SilentAsync Runtime callable，并等待动态 child 完成。
///
/// Invoker 通过 `TopologyStorage` 拥有独立 Topology，并保存 Runtime callable。
/// 首次进入设置 PREEMPTED 和基准 join slot；child 未全部结束时保持 Work 存活并等待恢复，
/// 最终归零后清除 PREEMPTED 并进入 SilentAsync tear-down。异常由当前隐式锚点归档。
///
/// @tparam F 按值拥有的 Runtime callable 类型。
template <typename F>
class SilentAsyncRuntimeInvoker final : public TopologyStorage, public RuntimeWork<F> {
    using Base = RuntimeWork<F>;

    using Base::m_func;

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::IMPLICIT_ANCHOR;
    static constexpr Work::Control::type CONTROL = Work::Control::NONE;

    template <typename U>
        requires std::constructible_from<Base, U&&>
    explicit SilentAsyncRuntimeInvoker(Topology* parent_topology, Executor* executor, U&& f)
        : TopologyStorage{parent_topology, executor}
        , Base{std::forward<U>(f)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        // 首次进入。
        if ((w.m_properties & Work::Properties::PREEMPTED) == 0) {
            TFL_WORK_EXECUTION_BEGIN(w);

            w.m_properties |= Work::Properties::PREEMPTED;
            w.m_join_counter.fetch_add(1, std::memory_order_release);

            Runtime rt{w, wr, exe};

            try {
                std::invoke(m_func, rt);
            } catch (...) {
                w._process_exception();
            }

            if (w.m_join_counter.fetch_sub(1, std::memory_order_acq_rel) != 1) {
                return;
            }
        }

        // 最终完成。
        w.m_properties &= ~Work::Properties::PREEMPTED;

        TFL_WORK_EXECUTION_END(w);
        exe._tear_down_silent_async_task(w, wr, cache);
    }
};


/// @brief 执行无结果句柄的 SilentAsync SubFlow callable，并等待内部动态子图完成。
///
/// 通过 `TopologyStorage` 拥有独立 Topology，同时按值拥有 callable 和内部 Graph。
/// 首次执行设置 PREEMPTED 与基准 join slot；SubFlow child 未完成时挂起当前 Work，恢复后
/// 清除 PREEMPTED 并进入 SilentAsync tear-down。异常由当前隐式锚点承接。
///
/// @tparam F 按值拥有的 SubFlow callable 类型。
template <typename F>
class SilentAsyncSubFlowInvoker final : public TopologyStorage, public GraphWork<SilentAsyncSubFlowInvoker<F>> {
    using Self = SilentAsyncSubFlowInvoker<F>;
    using Base = GraphWork<Self>;

    friend class GraphWork<Self>;

    TFL_NO_UNIQUE_ADDRESS F m_func;
    Graph m_graph;

    [[nodiscard]] Graph& graph() noexcept {
        return m_graph;
    }

    [[nodiscard]] const Graph& graph() const noexcept {
        return m_graph;
    }

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::IMPLICIT_ANCHOR;
    static constexpr Work::Control::type CONTROL = Work::Control::NONE;

    template <typename U>
        requires std::constructible_from<F, U&&>
    explicit SilentAsyncSubFlowInvoker(Topology* parent_topology, Executor* executor, U&& f)
        : TopologyStorage{parent_topology, executor}
        , m_func{std::forward<U>(f)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        // 首次进入。
        if ((w.m_properties & Work::Properties::PREEMPTED) == 0) {
            TFL_WORK_EXECUTION_BEGIN(w);

            w.m_properties |= Work::Properties::PREEMPTED;
            w.m_join_counter.fetch_add(1, std::memory_order_release);

            SubFlow flow{m_graph, w, wr, exe};

            try {
                std::invoke(m_func, flow);
            } catch (...) {
                w._process_exception();
            }

            if (w.m_join_counter.fetch_sub(1, std::memory_order_acq_rel) != 1) {
                return;
            }
        }

        // 最终完成。
        w.m_properties &= ~Work::Properties::PREEMPTED;
        TFL_WORK_EXECUTION_END(w);
        exe._tear_down_silent_async_task(w, wr, cache);
    }
};

/// @brief 无结果句柄地重复执行一个子 Graph，并在结束时调用完成回调。
///
/// 通过 `TopologyStorage` 拥有独立 Topology；同时保存 Graph holder、终止谓词和 callback。
/// 首次进入初始化子图并设置 PREEMPTED；每轮完成后检查 source 数量、停止/异常和谓词。
/// 结束时调用 callback、清除 PREEMPTED 并进入 SilentAsync tear-down；继续时重置 join 状态，
/// 最后一个 source 通过 cache 接力，其余 source 发布到 Worker 队列。
///
/// @tparam GhStore Graph holder 的实际存储类型。
/// @tparam P 无参终止谓词类型。
/// @tparam C Module 整体结束时调用的无参回调类型。
template <typename GhStore, typename P, typename C>
class SilentAsyncModuleInvoker final : public TopologyStorage, public GraphWork<SilentAsyncModuleInvoker<GhStore, P, C>> {
    using Self = SilentAsyncModuleInvoker<GhStore, P, C>;
    using Base = GraphWork<Self>;

    friend class GraphWork<Self>;

    std::size_t m_num_sources{0};
    TFL_NO_UNIQUE_ADDRESS GhStore m_gh_store;
    TFL_NO_UNIQUE_ADDRESS P m_pred;
    TFL_NO_UNIQUE_ADDRESS C m_callback;

    [[nodiscard]] Graph& graph() noexcept {
        auto& graph_holder = detail::borrow(m_gh_store);
        return detail::to_graph(graph_holder);
    }

    [[nodiscard]] const Graph& graph() const noexcept {
        const auto& graph_holder = detail::borrow(m_gh_store);
        return detail::to_graph(graph_holder);
    }

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::IMPLICIT_ANCHOR;
    static constexpr Work::Control::type CONTROL = Work::Control::NONE;

    template <typename Ghs, typename V, typename W>
        requires std::constructible_from<GhStore, Ghs&&> && std::constructible_from<P, V&&> && std::constructible_from<C, W&&>
    explicit SilentAsyncModuleInvoker(Topology* parent_topology, Executor* executor, Ghs&& ghs, V&& pred, W&& callback)
        : TopologyStorage{parent_topology, executor}
        , m_gh_store{std::forward<Ghs>(ghs)}
        , m_pred{std::forward<V>(pred)}
        , m_callback{std::forward<W>(callback)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        Graph& graph = this->graph();

        // 首次进入：初始化整个子图。
        if ((w.m_properties & Work::Properties::PREEMPTED) == 0) {
            TFL_WORK_EXECUTION_BEGIN(w);

            w.m_properties |= Work::Properties::PREEMPTED;
            m_num_sources = exe._set_up_graph(graph, w);
        }

        // 无 source、异常/停止或循环条件满足时结束整个 Module。
        if (m_num_sources == 0 || w._should_abort() || w._invoke_predicate(m_pred)) {
            w._invoke_callback(m_callback);

            w.m_properties &= ~Work::Properties::PREEMPTED;
            TFL_WORK_EXECUTION_END(w);
            exe._tear_down_silent_async_task(w, wr, cache);
            return;
        }

        // 建立本轮子图运行期依赖计数。
        exe._reset_graph_join_counters(graph, m_num_sources);

        // 建立本轮所有 source 的完成等待计数。
        w.m_join_counter.store(m_num_sources, std::memory_order_relaxed);

        // 最后一个 source 通过 cache 接力，其余 source 发布到 Worker 队列。
        Work** const data = graph.m_works.data();
        const std::size_t n = m_num_sources - 1;

        cache = data[n];

        if (n != 0) [[likely]] {
            if (n > 1) [[likely]] {
                exe._schedule(wr, data, n);
            } else {
                exe._schedule(wr, data[0]);
            }
        }
    }
};


// ============================================================================
// Async Invoker 家族
// ============================================================================
//
// Async Work 由 AsyncFuture 观察结果和完成状态。ResultStorage 提供独立 Topology 与结果槽，
// 完成路径发布 Finished 并释放执行强引用，但 Work 可继续由外部 Future 强引用保持存活。

/// @brief 执行可由 `AsyncFuture<R>` 等待并读取结果的普通 Async callable。
///
/// 继承 `ResultStorage<R>`，按值拥有独立 Topology 和结果槽；当前 Work 固定为显式异常锚点。
/// invoke 通过 `set_result_from()` 保存返回值或 void 完成状态，用户异常进入 Work 归档链，
/// 最后 `_tear_down_async_task()` 发布 Finished、唤醒等待者并释放执行生命周期引用。
///
/// Async 不参与运行期动态后继插入协议，其 Work 可在 Future 强引用释放前继续存活。
///
/// @tparam F 按值拥有的 callable 类型。
template <typename F>
class AsyncBasicInvoker final : public ResultStorage<basic_return_t<F>>, public BasicWork<F> {
    using R = basic_return_t<F>;
    using Storage = ResultStorage<R>;
    using Base = BasicWork<F>;

    using Base::m_func;

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::NONE;
    static constexpr Work::Control::type CONTROL = Work::Control::EXPLICIT_ANCHOR;

    template <typename U>
        requires std::constructible_from<Base, U&&>
    explicit AsyncBasicInvoker(Topology* parent_topology, Executor* executor, U&& f)
        : Storage{parent_topology, executor}
        , Base{std::forward<U>(f)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        TFL_WORK_EXECUTION_BEGIN(w);

        try {
            Storage::set_result_from(m_func);
        } catch (...) {
            w._process_exception();
        }

        TFL_WORK_EXECUTION_END(w);
        exe._tear_down_async_task(w, wr, cache);
    }
};


/// @brief 执行可返回结果的 Async Runtime callable，并等待动态 child 完成。
///
/// `ResultStorage<R>` 提供独立 Topology 与结果槽，Work 本身是显式异常锚点。首次进入设置
/// PREEMPTED 和基准 join slot，并通过 `set_result_from()` 调用用户 Runtime callable；
/// 若派生 child，则保持挂起直到最后一个 child 恢复当前 Work，最终清除 PREEMPTED 并进入
/// Async tear-down 发布完成状态。
///
/// @tparam F 按值拥有的 Runtime callable 类型。
template <typename F>
class AsyncRuntimeInvoker final : public ResultStorage<runtime_return_t<F>>, public RuntimeWork<F> {
    using R = runtime_return_t<F>;
    using Storage = ResultStorage<R>;
    using Base = RuntimeWork<F>;

    using Base::m_func;

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::NONE;
    static constexpr Work::Control::type CONTROL = Work::Control::EXPLICIT_ANCHOR;

    template <typename U>
        requires std::constructible_from<Base, U&&>
    explicit AsyncRuntimeInvoker(Topology* parent_topology, Executor* executor, U&& f)
        : Storage{parent_topology, executor}
        , Base{std::forward<U>(f)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        // 首次进入。
        if ((w.m_properties & Work::Properties::PREEMPTED) == 0) {
            TFL_WORK_EXECUTION_BEGIN(w);

            w.m_properties |= Work::Properties::PREEMPTED;
            w.m_join_counter.fetch_add(1, std::memory_order_release);

            Runtime rt{w, wr, exe};

            try {
                Storage::set_result_from(m_func, rt);
            } catch (...) {
                w._process_exception();
            }

            if (w.m_join_counter.fetch_sub(1, std::memory_order_acq_rel) != 1) {
                return;
            }
        }

        // 最终完成。
        w.m_properties &= ~Work::Properties::PREEMPTED;

        TFL_WORK_EXECUTION_END(w);
        exe._tear_down_async_task(w, wr, cache);
    }
};

/// @brief 执行可返回结果的 Async SubFlow callable，并等待内部动态子图完成。
///
/// Invoker 同时拥有 `ResultStorage<R>`、callable 和内部 Graph。首次进入设置 PREEMPTED
/// 与基准 join slot，并将栈绑定 `SubFlow` 注入用户 callable；child 未结束时挂起当前 Work。
/// 恢复后清除 PREEMPTED，并通过 Async tear-down 发布 Finished 和结果可见性。
///
/// @tparam F 按值拥有的 SubFlow callable 类型。
template <typename F>
class AsyncSubFlowInvoker final : public ResultStorage<subflow_return_t<F>>, public GraphWork<AsyncSubFlowInvoker<F>> {
    using R = subflow_return_t<F>;
    using Self = AsyncSubFlowInvoker<F>;
    using Storage = ResultStorage<R>;
    using Base = GraphWork<Self>;

    friend class GraphWork<Self>;

    TFL_NO_UNIQUE_ADDRESS F m_func;
    Graph m_graph;

    [[nodiscard]] Graph& graph() noexcept {
        return m_graph;
    }

    [[nodiscard]] const Graph& graph() const noexcept {
        return m_graph;
    }

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::NONE;
    static constexpr Work::Control::type CONTROL = Work::Control::EXPLICIT_ANCHOR;

    template <typename U>
        requires std::constructible_from<F, U&&>
    explicit AsyncSubFlowInvoker(Topology* parent_topology, Executor* executor, U&& f)
        : Storage{parent_topology, executor}
        , m_func{std::forward<U>(f)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        // 首次进入。
        if ((w.m_properties & Work::Properties::PREEMPTED) == 0) {
            TFL_WORK_EXECUTION_BEGIN(w);

            w.m_properties |= Work::Properties::PREEMPTED;
            w.m_join_counter.fetch_add(1, std::memory_order_release);

            SubFlow flow{m_graph, w, wr, exe};

            try {
                Storage::set_result_from(m_func, flow);
            } catch (...) {
                w._process_exception();
            }

            if (w.m_join_counter.fetch_sub(1, std::memory_order_acq_rel) != 1) {
                return;
            }
        }

        // 最终完成。
        w.m_properties &= ~Work::Properties::PREEMPTED;

        TFL_WORK_EXECUTION_END(w);
        exe._tear_down_async_task(w, wr, cache);
    }
};

/// @brief 可由 Future 等待完成的 Async Module，重复执行子 Graph 并在结束时调用 callback。
///
/// `ResultStorage<void>` 提供独立 Topology 与完成结果槽，Work 固定为显式异常锚点。
/// Invoker 保存 Graph holder、终止谓词和完成回调；通过 PREEMPTED 跨多轮子图保持一次
/// Module 执行，终止后进入 Async tear-down 发布 Finished 并唤醒 Future 等待者。
///
/// @tparam GhStore Graph holder 的实际存储类型。
/// @tparam P 无参终止谓词类型。
/// @tparam C Module 整体结束时调用的无参回调类型。
template <typename GhStore, typename P, typename C>
class AsyncModuleInvoker final : public ResultStorage<void>, public GraphWork<AsyncModuleInvoker<GhStore, P, C>> {
    using Self = AsyncModuleInvoker<GhStore, P, C>;
    using Storage = ResultStorage<void>;
    using Base = GraphWork<Self>;

    friend class GraphWork<Self>;

    std::size_t m_num_sources{0};
    TFL_NO_UNIQUE_ADDRESS GhStore m_gh_store;
    TFL_NO_UNIQUE_ADDRESS P m_pred;
    TFL_NO_UNIQUE_ADDRESS C m_callback;

    [[nodiscard]] Graph& graph() noexcept {
        auto& graph_holder = detail::borrow(m_gh_store);
        return detail::to_graph(graph_holder);
    }

    [[nodiscard]] const Graph& graph() const noexcept {
        const auto& graph_holder = detail::borrow(m_gh_store);
        return detail::to_graph(graph_holder);
    }

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::NONE;
    static constexpr Work::Control::type CONTROL = Work::Control::EXPLICIT_ANCHOR;

    template <typename Ghs, typename V, typename W>
        requires std::constructible_from<GhStore, Ghs&&> && std::constructible_from<P, V&&> && std::constructible_from<C, W&&>
    explicit AsyncModuleInvoker(Topology* parent_topology, Executor* executor, Ghs&& ghs, V&& pred, W&& callback)
        : Storage{parent_topology, executor}
        , m_gh_store{std::forward<Ghs>(ghs)}
        , m_pred{std::forward<V>(pred)}
        , m_callback{std::forward<W>(callback)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        Graph& graph = this->graph();

        // 首次进入：初始化整个子图。
        if ((w.m_properties & Work::Properties::PREEMPTED) == 0) {
            TFL_WORK_EXECUTION_BEGIN(w);

            w.m_properties |= Work::Properties::PREEMPTED;
            m_num_sources = exe._set_up_graph(graph, w);
        }

        // 无 source、异常/停止或循环条件满足时结束整个 Module。
        if (m_num_sources == 0 || w._should_abort() || w._invoke_predicate(m_pred)) {
            w._invoke_callback(m_callback);

            w.m_properties &= ~Work::Properties::PREEMPTED;
            TFL_WORK_EXECUTION_END(w);
            exe._tear_down_async_task(w, wr, cache);
            return;
        }

        // 建立本轮子图运行期依赖计数。
        exe._reset_graph_join_counters(graph, m_num_sources);

        // 建立本轮所有 source 的完成等待计数。
        w.m_join_counter.store(m_num_sources, std::memory_order_relaxed);

        // 最后一个 source 通过 cache 接力，其余 source 发布到 Worker 队列。
        Work** const data = graph.m_works.data();
        const std::size_t n = m_num_sources - 1;

        cache = data[n];

        if (n != 0) [[likely]] {
            if (n > 1) [[likely]] {
                exe._schedule(wr, data, n);
            } else {
                exe._schedule(wr, data[0]);
            }
        }
    }
};


// ============================================================================
// AsyncTask Invoker 家族
// ============================================================================
//
// AsyncTask Work 对应可在启动前建立动态前置依赖、运行期被其他 AsyncTask Work 注册为动态后继的
// 异步任务。完成路径必须与 Topology LOCKED 插边协议协调，发布 Finished 后冻结并传播动态后继。

/// @brief 执行可建立运行期前置依赖和动态后继的 AsyncTask 普通异步 callable。
///
/// `ResultStorage<R>` 保存独立 Topology 与结果槽，Work 固定为显式异常锚点。执行前支持
/// Semaphore 获取和 Observer before，结果通过 `set_result_from()` 写入；完成后释放 Semaphore、
/// 触发 Observer after，并由 `_tear_down_async_task()` 与动态插边 LOCKED 协议竞争 Finished，
/// 冻结并传播运行期动态后继后释放执行引用。
///
/// @tparam F 按值拥有的 callable 类型。
template <typename F>
class AsyncTaskBasicInvoker final : public ResultStorage<basic_return_t<F>>, public BasicWork<F> {
    using R = basic_return_t<F>;
    using Storage = ResultStorage<R>;
    using Base = BasicWork<F>;

    using Base::m_func;

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::NONE;
    static constexpr Work::Control::type CONTROL = Work::Control::EXPLICIT_ANCHOR;

    template <typename U>
        requires std::constructible_from<Base, U&&>
    explicit AsyncTaskBasicInvoker(Topology* parent_topology, Executor* executor, U&& f)
        : Storage{parent_topology, executor}
        , Base{std::forward<U>(f)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        // 执行前获取 Semaphore；失败时当前 Work 进入 waiter，本次 invoke 立即让出 Worker。
        if (w.m_semaphores && !w.m_semaphores->acquires.empty()) {
            SmallVector<Work*> waiters;
            if (!w._try_acquire_semaphores(waiters)) {
                exe._schedule_from_semaphore(wr, waiters);
                return;
            }
        }

        TFL_WORK_EXECUTION_BEGIN(w);

        w._notify_before(wr);

        try {
            Storage::set_result_from(m_func);
        } catch (...) {
            w._process_exception();
        }

        w._notify_after(wr);

        // 执行完成后释放配置的 Semaphore，并重新调度本次被解冻的 waiter。
        if (w.m_semaphores && !w.m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            w._release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }

        TFL_WORK_EXECUTION_END(w);
        exe._tear_down_async_task(w, wr, cache);
    }
};


/// @brief 执行 AsyncTask Runtime callable，并同时支持动态依赖、结果和运行期 child。
///
/// 首次进入先获取 Semaphore，再设置 PREEMPTED 与基准 join slot，并通过 `ResultStorage<R>`
/// 保存 Runtime callable 的返回值。child 未完成时 Work 保持挂起；恢复后触发 Observer after、
/// 清除 PREEMPTED、释放 Semaphore，最终由 AsyncTask tear-down 发布 Finished 并传播动态后继。
///
/// @tparam F 按值拥有的 Runtime callable 类型。
template <typename F>
class AsyncTaskRuntimeInvoker final : public ResultStorage<runtime_return_t<F>>, public RuntimeWork<F> {
    using R = runtime_return_t<F>;
    using Storage = ResultStorage<R>;
    using Base = RuntimeWork<F>;

    using Base::m_func;

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::NONE;
    static constexpr Work::Control::type CONTROL = Work::Control::EXPLICIT_ANCHOR;

    template <typename U>
        requires std::constructible_from<Base, U&&>
    explicit AsyncTaskRuntimeInvoker(Topology* parent_topology, Executor* executor, U&& f)
        : Storage{parent_topology, executor}
        , Base{std::forward<U>(f)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        // 首次进入。
        if ((w.m_properties & Work::Properties::PREEMPTED) == 0) {
            // acquire 阶段。
            if (w.m_semaphores && !w.m_semaphores->acquires.empty()) {
                SmallVector<Work*> waiters;
                if (!w._try_acquire_semaphores(waiters)) {
                    exe._schedule_from_semaphore(wr, waiters);
                    return;
                }
            }

            TFL_WORK_EXECUTION_BEGIN(w);

            w.m_properties |= Work::Properties::PREEMPTED;
            w.m_join_counter.fetch_add(1, std::memory_order_release);

            w._notify_before(wr);

            Runtime rt{w, wr, exe};

            try {
                Storage::set_result_from(m_func, rt);
            } catch (...) {
                w._process_exception();
            }

            if (w.m_join_counter.fetch_sub(1, std::memory_order_acq_rel) != 1) {
                return;
            }
        }

        // 最终完成。
        w._notify_after(wr);

        w.m_properties &= ~Work::Properties::PREEMPTED;

        // release 阶段。
        if (w.m_semaphores && !w.m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            w._release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }

        TFL_WORK_EXECUTION_END(w);
        exe._tear_down_async_task(w, wr, cache);
    }
};


/// @brief 执行 AsyncTask SubFlow callable，并同时支持动态依赖、结果和内部动态子图。
///
/// Invoker 按值拥有 callable 与内部 Graph，并通过 `ResultStorage<R>` 保存独立 Topology
/// 和结果。首次进入完成 Semaphore/Observer 前置流程、设置 PREEMPTED 与基准 join slot；
/// SubFlow child 完成后恢复当前 Work，最终清理 PREEMPTED、释放 Semaphore并进入 AsyncTask
/// tear-down，与动态后继插入协议协调完成发布。
///
/// @tparam F 按值拥有的 SubFlow callable 类型。
template <typename F>
class AsyncTaskSubFlowInvoker final : public ResultStorage<subflow_return_t<F>>, public GraphWork<AsyncTaskSubFlowInvoker<F>> {
    using R = subflow_return_t<F>;
    using Self = AsyncTaskSubFlowInvoker<F>;
    using Storage = ResultStorage<R>;
    using Base = GraphWork<Self>;

    friend class GraphWork<Self>;

    TFL_NO_UNIQUE_ADDRESS F m_func;
    Graph m_graph;

    [[nodiscard]] Graph& graph() noexcept {
        return m_graph;
    }

    [[nodiscard]] const Graph& graph() const noexcept {
        return m_graph;
    }

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::NONE;
    static constexpr Work::Control::type CONTROL = Work::Control::EXPLICIT_ANCHOR;

    template <typename U>
        requires std::constructible_from<F, U&&>
    explicit AsyncTaskSubFlowInvoker(Topology* parent_topology, Executor* executor, U&& f)
        : Storage{parent_topology, executor}
        , m_func{std::forward<U>(f)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        // 首次进入。
        if ((w.m_properties & Work::Properties::PREEMPTED) == 0) {
            // acquire 阶段。
            if (w.m_semaphores && !w.m_semaphores->acquires.empty()) {
                SmallVector<Work*> waiters;
                if (!w._try_acquire_semaphores(waiters)) {
                    exe._schedule_from_semaphore(wr, waiters);
                    return;
                }
            }

            TFL_WORK_EXECUTION_BEGIN(w);

            w.m_properties |= Work::Properties::PREEMPTED;
            w.m_join_counter.fetch_add(1, std::memory_order_release);

            w._notify_before(wr);

            SubFlow flow{m_graph, w, wr, exe};

            try {
                Storage::set_result_from(m_func, flow);
            } catch (...) {
                w._process_exception();
            }

            if (w.m_join_counter.fetch_sub(1, std::memory_order_acq_rel) != 1) {
                return;
            }
        }

        // 最终完成。
        w._notify_after(wr);

        w.m_properties &= ~Work::Properties::PREEMPTED;

        // release 阶段。
        if (w.m_semaphores && !w.m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            w._release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }

        TFL_WORK_EXECUTION_END(w);
        exe._tear_down_async_task(w, wr, cache);
    }
};

/// @brief 执行 AsyncTask Module，并同时提供动态依赖、Semaphore/Observer 和 Future 完成状态。
///
/// `ResultStorage<void>` 提供独立 Topology 与完成槽；Invoker 保存 Graph holder、终止谓词和
/// callback。首次进入先获取 Semaphore、触发 Observer before、设置 PREEMPTED 并初始化子图；
/// 多轮 source 执行完成后恢复当前 Work继续检查终止条件。结束时触发 Observer after 和 callback，
/// 释放 Semaphore，并由 AsyncTask tear-down 发布 Finished、冻结并传播动态后继。
///
/// @tparam GhStore Graph holder 的实际存储类型。
/// @tparam P 无参终止谓词类型。
/// @tparam C Module 整体结束时调用的无参回调类型。
template <typename GhStore, typename P, typename C>
class AsyncTaskModuleInvoker final : public ResultStorage<void>, public GraphWork<AsyncTaskModuleInvoker<GhStore, P, C>> {
    using Self = AsyncTaskModuleInvoker<GhStore, P, C>;
    using Storage = ResultStorage<void>;
    using Base = GraphWork<Self>;

    friend class GraphWork<Self>;

    std::size_t m_num_sources{0};
    TFL_NO_UNIQUE_ADDRESS GhStore m_gh_store;
    TFL_NO_UNIQUE_ADDRESS P m_pred;
    TFL_NO_UNIQUE_ADDRESS C m_callback;

    [[nodiscard]] Graph& graph() noexcept {
        auto& graph_holder = detail::borrow(m_gh_store);
        return detail::to_graph(graph_holder);
    }

    [[nodiscard]] const Graph& graph() const noexcept {
        const auto& graph_holder = detail::borrow(m_gh_store);
        return detail::to_graph(graph_holder);
    }

public:
    static constexpr Work::Properties::type PROPERTIES = Work::Properties::NONE;
    static constexpr Work::Control::type CONTROL = Work::Control::EXPLICIT_ANCHOR;

    template <typename Ghs, typename V, typename W>
        requires std::constructible_from<GhStore, Ghs&&> && std::constructible_from<P, V&&> && std::constructible_from<C, W&&>
    explicit AsyncTaskModuleInvoker(Topology* parent_topology, Executor* executor, Ghs&& ghs, V&& pred, W&& callback)
        : Storage{parent_topology, executor}
        , m_gh_store{std::forward<Ghs>(ghs)}
        , m_pred{std::forward<V>(pred)}
        , m_callback{std::forward<W>(callback)} {}

    void invoke(Work& w, Worker& wr, Executor& exe, Work*& cache) {
        Graph& graph = this->graph();

        if ((w.m_properties & Work::Properties::PREEMPTED) == 0) {
            // 首次进入：获取信号量并初始化整个子图。
            if (w.m_semaphores && !w.m_semaphores->acquires.empty()) [[unlikely]] {
                SmallVector<Work*> waiters;
                if (!w._try_acquire_semaphores(waiters)) {
                    exe._schedule_from_semaphore(wr, waiters);
                    return;
                }
            }
            TFL_WORK_EXECUTION_BEGIN(w);

            w.m_properties |= Work::Properties::PREEMPTED;
            w._notify_before(wr);
            m_num_sources = exe._set_up_graph(graph, w);
        }

        // 无 source、异常/停止或循环条件满足时结束整个 Module。
        if (m_num_sources == 0 || w._should_abort() || w._invoke_predicate(m_pred)) {
            w._notify_after(wr);

            w._invoke_callback(m_callback);
            w.m_properties &= ~Work::Properties::PREEMPTED;
            if (w.m_semaphores && !w.m_semaphores->releases.empty()) [[unlikely]] {
                SmallVector<Work*> waiters;
                w._release_semaphores(waiters);
                exe._schedule_from_semaphore(wr, waiters);
            }

            TFL_WORK_EXECUTION_END(w);
            exe._tear_down_async_task(w, wr, cache);
            return;
        }

        exe._reset_graph_join_counters(graph, m_num_sources);

        // 建立本轮所有 source 的完成等待计数。
        w.m_join_counter.store(m_num_sources, std::memory_order_relaxed);

        // 最后一个 source 通过 cache 接力，其余 source 发布到 Worker 队列。
        Work** const data = graph.m_works.data();
        const std::size_t n = m_num_sources - 1;

        cache = data[n];

        if (n != 0) [[likely]] {
            if (n > 1) [[likely]] {
                exe._schedule(wr, data, n);
            } else {
                exe._schedule(wr, data[0]);
            }
        }
    }

};

}  // namespace tfl
