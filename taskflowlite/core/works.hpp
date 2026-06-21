/// @file works.hpp
/// @brief Work 子类家族 —— 节点类型、提交语义与调用签名的组合实现
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-04-20
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once
#include <cmath>
#include <cstring>
#include <variant>
#include <optional>

#include "work.hpp"
#include "runtime.hpp"
#include "branch.hpp"
#include "jump.hpp"
#include "executor.hpp"
#include "d2_render.hpp"

namespace tfl {

/// @details
/// 本文件实现所有具体 `Work` 派生类。类型数量较多，本质上由三类维度组合而成：
///
/// 维度 A —— 节点类型：
///   - `BasicWork`         普通同步任务
///   - `RuntimeWork`       注入 `Runtime&` 的运行时任务
///   - `BranchWork`        单目标条件分支
///   - `MultiBranchWork`   多目标条件分支
///   - `JumpWork`          单目标强制跳转
///   - `MultiJumpWork`     多目标强制跳转
///   - `GraphWork<Flow>`   嵌套 Flow / 子图节点
///   - `AnchorWork`        内部锚点节点，无用户逻辑
///
/// 维度 B —— 提交语义：
///   - 静态图节点                `*Invoker`
///   - fire-and-forget           `SilentAsync*Invoker`
///   - future 异步任务            `Async*Invoker`
///   - 动态依赖任务              `AsyncHandle*Invoker`
///   - 延迟启动动态依赖任务       `AsyncHandle*Invoker`
///
/// 维度 C —— 调用签名：
///   - basic
///   - runtime
///   - flow
///   - branch / multi_branch
///   - jump / multi_jump
///
/// 每个 Invoker 负责保存用户 callable 与参数，并在 `invoke()` 中执行用户逻辑、
/// 捕获异常，然后转入对应的 Executor tear_down 协议。
///
/// ============================================================================
///  AnchorWork
/// ============================================================================
/// `AnchorWork` 是内部使用的无 body 节点：`TaskType::None`，`invoke()` 为空，
/// `dump()` 为空。它用于依赖计数、异常归档和协作式等待边界，不表示用户任务。
///
/// ============================================================================
///  TopologyHolder
/// ============================================================================
/// `TopologyHolder` 持有局部 `Topology`，用于需要独立执行实例的动态任务。
/// 通过 `private TopologyHolder + public XxxWork` 的继承顺序，先构造
/// `m_local_topology`，再把它传给 `Work` 基类保存。
///
/// ============================================================================
///  PREEMPTED 重入协议
/// ============================================================================
/// `RuntimeWork`、`GraphWork` 以及相关动态任务可能在执行期间派生子任务。
/// 这类节点使用 `PREEMPTED` 两阶段协议：
///
/// 1. 首次进入：设置 `PREEMPTED`，`join_counter += 1` 形成 self-pin，执行 body；
/// 2. 若仍有子任务未完成，则当前 worker 返回；
/// 3. 最后一个子任务完成时通过 `_schedule_parent` 重新调度父节点；
/// 4. 第二次进入：清除 `PREEMPTED`，执行真正的 tear_down。
///
///

/// @brief 持有局部 `Topology` 实例的辅助基类。
///
/// @details
/// 用于需要独立执行上下文的动态任务（异步任务、延迟任务）。
/// 通过 `private TopologyHolder + public XxxWork` 的继承顺序，
/// 确保 `m_local_topology` 先于 `Work` 基类构造，从而 `Work`
/// 可以安全地持有指向该 Topology 的指针。
///
struct TopologyHolder {
    Topology m_local_topology;
    explicit TopologyHolder(Executor* exec) : m_local_topology{exec} {}
};

// ============================================================================
//  TaskType::Noop (dotted circle —— 占位节点)
// ============================================================================
class NoopWork : public Work {
protected:
    template <typename... Xs>
    explicit NoopWork(Xs&&... xs) noexcept
        : Work{TaskType::Noop, std::forward<Xs>(xs)...} {}

    void dump(std::ostream& os) const override final {
        D2Renderer::render_work(os, this, "circle", "#e5e7eb", "#9ca3af", "#374151", "8", "3");
    }
};

// ============================================================================
//  TaskType::Basic 家族
// ============================================================================

/// @brief 普通同步任务的 `Work` 基类 —— `TaskType::Basic`。
///
/// @details
/// 本类不包含用户 callable 或参数；它仅固化节点类型与 D2 可视化样式。
/// 用户逻辑由派生类 `BasicInvoker` 持有并通过 `invoke()` 执行。
///
class BasicWork : public Work {
protected:
    template <typename... Xs>
    explicit BasicWork(Xs&&... xs) noexcept
        : Work{TaskType::Basic, std::forward<Xs>(xs)...} {}

    void dump(std::ostream& os) const override final {
        D2Renderer::render_work(os, this, "rectangle", "#f5f5f5", "#9ca3af", "#1f2937", "8");
    }
};

// ============================================================================
//  TaskType::Branch (diamond)
// ============================================================================

/// @brief 单目标条件分支任务基类 —— `TaskType::Branch`。
///
/// @details
/// 分支节点通过 `Branch` 句柄选择一个后继；未选中的出边在 tear_down
/// 阶段被跳过（依赖计数不传播）。D2 渲染为菱形。
///
class BranchWork : public Work {
protected:
    template <typename... Xs>
    explicit BranchWork(Xs&&... xs) noexcept
        : Work{TaskType::Branch, std::forward<Xs>(xs)...} {}

    void dump(std::ostream& os) const override final {
        D2Renderer::render_work(os, this, "diamond", "#dbeafe", "#3b82f6", "#1e3a5f", "8");
    }
};

// ============================================================================
//  TaskType::MultiBranch (hexagon)
// ============================================================================

/// @brief 多目标广播分支任务基类 —— `TaskType::MultiBranch`。
///
/// @details
/// 与 `BranchWork` 不同，`MultiBranchWork` 允许同时激活**多个**后继。
/// 用户通过 `MultiBranch` 句柄的 `activate()` 逐个挑选目标；
/// tear_down 阶段对每个激活目标独立传播依赖计数。D2 渲染为六边形。
///
class MultiBranchWork : public Work {
protected:
    template <typename... Xs>
    explicit MultiBranchWork(Xs&&... xs) noexcept
        : Work{TaskType::MultiBranch, std::forward<Xs>(xs)...} {}

    void dump(std::ostream& os) const override final {
        D2Renderer::render_work(os, this, "hexagon", "#bfdbfe", "#2563eb", "#1e3a5f", "8");
    }
};

// ============================================================================
//  TaskType::Jump (dashed diamond)
// ============================================================================

/// @brief 单目标强制跳转任务基类 —— `TaskType::Jump`。
///
/// @details
/// 跳转节点**不传**依赖计数给未选中的后继；它直接"跃迁"到指定目标，
/// 断开与原 DAG 拓扑的局部连接。与 `BranchWork` 的关键区别：
/// `JumpWork` 的 `Implicit` 位为 `NONE`（无需 join 汇合），
/// 而 `BranchWork` 默认携带 `JOIN` 位。D2 渲染为虚线菱形。
///
class JumpWork : public Work {
protected:
    template <typename... Xs>
    explicit JumpWork(Xs&&... xs) noexcept
        : Work{TaskType::Jump, std::forward<Xs>(xs)...} {}

    void dump(std::ostream& os) const override final {
        D2Renderer::render_work(os, this, "diamond", "#fee2e2", "#ef4444", "#7f1d1d", "8", "5");
    }
};

// ============================================================================
//  TaskType::MultiJump (dashed hexagon)
// ============================================================================

/// @brief 多目标广播跳转任务基类 —— `TaskType::MultiJump`。
///
/// @details
/// 结合 `MultiBranchWork` 的多目标激活能力和 `JumpWork` 的无 join 语义。
/// 用户通过 `MultiJump` 句柄激活一组目标节点，未被激活的后继
/// 在 tear_down 阶段被跳过。D2 渲染为虚线六边形。
///
class MultiJumpWork : public Work {
protected:
    template <typename... Xs>
    explicit MultiJumpWork(Xs&&... xs) noexcept
        : Work{TaskType::MultiJump, std::forward<Xs>(xs)...} {}

    void dump(std::ostream& os) const override final {
        D2Renderer::render_work(os, this, "hexagon", "#fecaca", "#dc2626", "#7f1d1d", "8", "5");
    }
};

// ============================================================================
//  TaskType::Runtime 家族
// ============================================================================

/// @brief 注入 `Runtime&` 的运行时任务基类 —— `TaskType::Runtime`。
///
/// @details
/// 与 `BasicWork` 的区别仅在于节点类型标签和 D2 渲染配色。
/// 派生类 `RuntimeInvoker` 在 `invoke()` 中构造 `Runtime` 句柄，
/// 授予用户在工作线程上动态创建子任务的能力。
///
class RuntimeWork : public Work {
protected:
    template <typename... Xs>
    explicit RuntimeWork(Xs&&... xs) noexcept
        : Work{TaskType::Runtime, std::forward<Xs>(xs)...} {}

    void dump(std::ostream& os) const override final {
        D2Renderer::render_work(os, this, "rectangle", "#fce4ec", "#e57373", "#6d1b1b", "30");
    }
};

// ============================================================================
//  TaskType::Graph (内嵌 Flow 的容器节点)
// ============================================================================

/// @brief 内嵌 Flow 子图的容器节点基类 —— `TaskType::Graph`。
///
/// @tparam GhStore  Graph holder 类型，决定子图的所有权语义：
///                  - `Graph&`      —— 引用外部 Graph（静态图节点）
///                  - `Graph`       —— 持有 Graph 副本（异步任务节点）
///                  - `std::unique_ptr<Graph>` —— 独占所有权
///
/// @details
/// `GraphWork` 不直接执行用户逻辑；它包装一个完整的子图并在 `invoke()`
/// 中委托给 `Executor` 的子图调度协议。派生类负责在合适的生命周期阶段
/// 调用 `Executor::_set_up_graph()` 初始化子图拓扑。
///
template <typename GhStore>
class GraphWork : public Work {
protected:
    GhStore m_gh_store;

    template <typename U, typename... Xs>
    explicit GraphWork(U&& gh, Xs&&... xs) noexcept
        : Work{TaskType::Graph, std::forward<Xs>(xs)...}
        , m_gh_store{std::forward<U>(gh)} {}

    void dump(std::ostream& os) const override final {
        const auto& graph = detail::to_graph(detail::borrow(m_gh_store));
        D2Renderer::render_graph(os, this, to_string(m_type), graph);
    }
};

// ============================================================================
//  衍生功能组件定义 (Derived Components)
// ============================================================================


/// @brief 占位工作元 —— 无 body，仅执行后继分发。
///
/// 用于 noop task：节点存在于图中、参与依赖计数与边遍历，
/// 但不携带用户逻辑、不触发观察者、不参与信号量与异常归档协议。
class NoopInvoker final : public NoopWork {
public:
    explicit NoopInvoker(const Graph* g)
        : NoopWork{Work::Implicit::JOIN, Work::Explicit::NONE, g} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        exe._tear_down_task(this, wr, cache);
    }
};

/// @brief 承载 `void()` 标准闭包的基础同步工作元。
template <typename F, typename... Args>
class BasicInvoker final : public BasicWork {
    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires (sizeof...(Args) == sizeof...(Us)) && std::constructible_from<F, U&&> && (std::constructible_from<Args, Us&&> && ...)
    explicit BasicInvoker(const Graph* g, U&& f, Us&&... args)
        : BasicWork{Work::Implicit::JOIN, Work::Explicit::NONE, g}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        if (_stop_requested()) [[unlikely]] {
            exe._schedule_parent(m_parent, wr, cache);
            return;
        }

        // acquire 阶段
        if (m_semaphores && !m_semaphores->acquires.empty()) {
            SmallVector<Work*> waiters;
            if (!_try_acquire_semaphores(waiters)) {
                exe._schedule_from_semaphore(wr, waiters);
                return;
            }
        }

        _notify_before(wr);

        try {
            if constexpr (sizeof...(Args) == 0) {
                if constexpr (basic_invocable_plain<F>) {
                    std::invoke(m_func);
                } else {
                    std::invoke(m_func, _stop_token());
                }
            } else {
                std::apply([this](auto&&... a) {
                    if constexpr (basic_invocable_plain<F, decltype(a)...>) {
                        std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))...);
                    } else {
                        std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., _stop_token());
                    }
                }, m_args);
            }
        } catch (...) {
            _process_exception();
        }

        _notify_after(wr);
        // release 阶段
        if (m_semaphores && !m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            _release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }
        exe._tear_down_task(this, wr, cache);
    }

};


/// @brief 为闭包注入单一通道挑选权杖 `Branch&` 的条件工作元。
template <typename F, typename... Args>
class BranchInvoker final : public BranchWork {
    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires (sizeof...(Args) == sizeof...(Us)) && std::constructible_from<F, U&&> && (std::constructible_from<Args, Us&&> && ...)
    explicit BranchInvoker(const Graph* g, U&& f, Us&&... args)
        : BranchWork{Work::Implicit::JOIN, Work::Explicit::NONE, g}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        if (_stop_requested()) [[unlikely]] {
            exe._schedule_parent(m_parent, wr, cache);
            return;
        }

        // acquire 阶段
        if (m_semaphores && !m_semaphores->acquires.empty()) {
            SmallVector<Work*> waiters;
            if (!_try_acquire_semaphores(waiters)) {
                exe._schedule_from_semaphore(wr, waiters);
                return;
            }
        }

        _notify_before(wr);

        Branch branch(*this);
        try {
            if constexpr (sizeof...(Args) == 0) {
                if constexpr (branch_invocable_plain<F>) {
                    std::invoke(m_func, branch);
                } else {
                    std::invoke(m_func, branch, _stop_token());
                }
            } else {
                std::apply([this, &branch](auto&&... a) {
                    if constexpr (branch_invocable_plain<F, decltype(a)...>) {
                        std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., branch);
                    } else {
                        std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., branch, _stop_token());
                    }
                }, m_args);
            }
        } catch (...) {
            _process_exception();
        }

        _notify_after(wr);
        // release 阶段
        if (m_semaphores && !m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            _release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }
        exe._tear_down_branch_task(this, wr, cache, branch.m_target);
    }

};

/// @brief 为闭包注入多线广播挑选权杖 `MultiBranch&` 的并行激活工作元。
template <typename F, typename... Args>
class MultiBranchInvoker final : public MultiBranchWork {
    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires (sizeof...(Args) == sizeof...(Us)) && std::constructible_from<F, U&&> && (std::constructible_from<Args, Us&&> && ...)
    explicit MultiBranchInvoker(const Graph* g, U&& f, Us&&... args)
        : MultiBranchWork{Work::Implicit::JOIN, Work::Explicit::NONE, g}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        if (_stop_requested()) [[unlikely]] {
            exe._schedule_parent(m_parent, wr, cache);
            return;
        }

        // acquire 阶段
        if (m_semaphores && !m_semaphores->acquires.empty()) {
            SmallVector<Work*> waiters;
            if (!_try_acquire_semaphores(waiters)) {
                exe._schedule_from_semaphore(wr, waiters);
                return;
            }
        }

        _notify_before(wr);

        MultiBranch branch(*this);
        try {
            if constexpr (sizeof...(Args) == 0) {
                if constexpr (multi_branch_invocable_plain<F>) {
                    std::invoke(m_func, branch);
                } else {
                    std::invoke(m_func, branch, _stop_token());
                }
            } else {
                std::apply([this, &branch](auto&&... a) {
                    if constexpr (multi_branch_invocable_plain<F, decltype(a)...>) {
                        std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., branch);
                    } else {
                        std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., branch, _stop_token());
                    }
                }, m_args);
            }
        } catch (...) {
            _process_exception();
        }

        _notify_after(wr);
        // release 阶段
        if (m_semaphores && !m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            _release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }
        exe._tear_down_multi_branch_task(this, wr, cache, branch.m_targets);
    }

};

/// @brief 授权在图内执行强制物理传送跃迁指令 `Jump&` 的中断工作元。
template <typename F, typename... Args>
class JumpInvoker final : public JumpWork {
    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires (sizeof...(Args) == sizeof...(Us)) && std::constructible_from<F, U&&> && (std::constructible_from<Args, Us&&> && ...)
    explicit JumpInvoker(const Graph* g, U&& f, Us&&... args)
        : JumpWork{Work::Implicit::NONE, Work::Explicit::NONE, g}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        if (_stop_requested()) [[unlikely]] {
            exe._schedule_parent(m_parent, wr, cache);
            return;
        }

        // acquire 阶段
        if (m_semaphores && !m_semaphores->acquires.empty()) {
            SmallVector<Work*> waiters;
            if (!_try_acquire_semaphores(waiters)) {
                exe._schedule_from_semaphore(wr, waiters);
                return;
            }
        }

        _notify_before(wr);

        Jump jmp{*this};
        try {
            if constexpr (sizeof...(Args) == 0) {
                if constexpr (jump_invocable_plain<F>) {
                    std::invoke(m_func, jmp);
                } else {
                    std::invoke(m_func, jmp, _stop_token());
                }
            } else {
                std::apply([this, &jmp](auto&&... a) {
                    if constexpr (jump_invocable_plain<F, decltype(a)...>) {
                        std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., jmp);
                    } else {
                        std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., jmp, _stop_token());
                    }
                }, m_args);
            }
        } catch (...) {
            _process_exception();
        }

        _notify_after(wr);
        // release 阶段
        if (m_semaphores && !m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            _release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }
        exe._tear_down_jump_task(this, wr, cache, jmp.m_target);
    }

};

/// @brief 授权同时摧毁并重置多个下游依赖计数网 `MultiJump&` 的广播跃迁元。
template <typename F, typename... Args>
class MultiJumpInvoker final : public MultiJumpWork {
    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires (sizeof...(Args) == sizeof...(Us)) && std::constructible_from<F, U&&> && (std::constructible_from<Args, Us&&> && ...)
    explicit MultiJumpInvoker(const Graph* g, U&& f, Us&&... args)
        : MultiJumpWork{Work::Implicit::NONE, Work::Explicit::NONE, g}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        if (_stop_requested()) [[unlikely]] {
            exe._schedule_parent(m_parent, wr, cache);
            return;
        }

        // acquire 阶段
        if (m_semaphores && !m_semaphores->acquires.empty()) {
            SmallVector<Work*> waiters;
            if (!_try_acquire_semaphores(waiters)) {
                exe._schedule_from_semaphore(wr, waiters);
                return;
            }
        }

        _notify_before(wr);

        MultiJump jmp{*this};
        try {
            if constexpr (sizeof...(Args) == 0) {
                if constexpr (multi_jump_invocable_plain<F>) {
                    std::invoke(m_func, jmp);
                } else {
                    std::invoke(m_func, jmp, _stop_token());
                }
            } else {
                std::apply([this, &jmp](auto&&... a) {
                    if constexpr (multi_jump_invocable_plain<F, decltype(a)...>) {
                        std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., jmp);
                    } else {
                        std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., jmp, _stop_token());
                    }
                }, m_args);
            }
        } catch (...) {
            _process_exception();
        }

        _notify_after(wr);
        // release 阶段
        if (m_semaphores && !m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            _release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }
        exe._tear_down_multi_jump_task(this, wr, cache, jmp.m_targets);
    }
};

/// @brief 提供 `Runtime&` 接口，赋予节点在执行期实时操纵所在线程栈与编排图谱的能力。
template <typename F, typename... Args>
class RuntimeInvoker final : public RuntimeWork {
    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires (sizeof...(Args) == sizeof...(Us)) && std::constructible_from<F, U&&> && (std::constructible_from<Args, Us&&> && ...)
    explicit RuntimeInvoker(const Graph* g, U&& f, Us&&... args)
        : RuntimeWork{Work::Implicit::JOIN, Work::Explicit::NONE, g}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {

        // ── 首次进入
        if ((m_implicit & Work::Implicit::PREEMPTED) == 0) {

            if (_stop_requested()) [[unlikely]] {
                exe._schedule_parent(m_parent, wr, cache);
                return;
            }

            // acquire 阶段（仅首次进入执行）
            if (m_semaphores && !m_semaphores->acquires.empty()) {
                SmallVector<Work*> waiters;
                if (!_try_acquire_semaphores(waiters)) {
                    exe._schedule_from_semaphore(wr, waiters);
                    return;
                }
            }

            // 进入抢占窗口：置抢占 + 锚点动态位；body 自身占位 counter
            m_implicit |= (Work::Implicit::PREEMPTED | Work::Implicit::ANCHORED);
            m_join_counter.fetch_add(1, std::memory_order_release);

            _notify_before(wr);

            Runtime rt(*this, wr, exe);
            try {
                if constexpr (sizeof...(Args) == 0) {
                    if constexpr (runtime_invocable_plain<F>) {
                        std::invoke(m_func, rt);
                    } else {
                        std::invoke(m_func, rt, _stop_token());
                    }
                } else {
                    std::apply([this, &rt](auto&&... a) {
                        if constexpr (runtime_invocable_plain<F, decltype(a)...>) {
                            std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., rt);
                        } else {
                            std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., rt, _stop_token());
                        }
                    }, m_args);
                }
            } catch (...) {
                _process_exception();
            }

            // Last-arriver 仲裁：
            //   fetch_sub 返回 1 → counter 已归 0，无在飞 child，fallthrough 走 tear_down
            //   否则            → counter > 0，仍有 child，return 让步；最后完成的 child
            //                     负责把本节点重新入队触发第二次进入。
            if (m_join_counter.fetch_sub(1, std::memory_order_acq_rel) != 1) {
                return;   // 抢占退出：不做 tear_down，由最后到达的 child 触发重入
            }
            // Fallthrough：没有在飞 child，继续走到 tear_down
        }

        _notify_after(wr);

        // ── 第二次进入
        m_implicit &= ~(Work::Implicit::PREEMPTED | Work::Implicit::ANCHORED);

        // release 阶段（仅在真正完成时执行一次）
        if (m_semaphores && !m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            _release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }

        exe._tear_down_task(this, wr, cache);
    }
};

/// @brief 静态图中的循环/条件子图执行器。
template <typename GhStore, typename P>
class SubflowInvoker final : public GraphWork<GhStore> {
    using GraphWork<GhStore>::m_gh_store;
    using Work::m_implicit;
    using Work::m_parent;
    using Work::m_semaphores;
    using Work::m_topology;
    using Work::m_join_counter;
    using Work::_stop_requested;
    using Work::_should_abort;
    using Work::_notify_before;
    using Work::_notify_after;
    using Work::_try_acquire_semaphores;
    using Work::_release_semaphores;

    std::size_t m_num_sources{0};
    P m_pred;
public:
    template <typename Ghs, typename V>
    explicit SubflowInvoker(const Graph* g, Ghs&& ghs, V&& pred)
        : GraphWork<GhStore>{std::forward<Ghs>(ghs), Work::Implicit::JOIN, Work::Explicit::NONE, g}
        , m_pred{std::forward<V>(pred)} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        auto& graph = detail::to_graph(detail::borrow(m_gh_store));

        // ── 首次进入：准入检查 + 子图初始化 ──────────────────
        if ((m_implicit & Work::Implicit::PREEMPTED) == 0) {
            if (_stop_requested()) [[unlikely]] {
                exe._schedule_parent(m_parent, wr, cache);
                return;
            }
            // 首次进入：竞争获取信号量，失败则让出执行权等待唤醒
            // acquire 阶段
            if (m_semaphores && !m_semaphores->acquires.empty()) {
                SmallVector<Work*> waiters;
                if (!_try_acquire_semaphores(waiters)) {
                    exe._schedule_from_semaphore(wr, waiters);
                    return;
                }
            }
            // 初始化子图拓扑：重置各节点 join_counter、清除异常残留、
            // 将零入度源节点 swap 到 graph 前端，返回源节点数量
            m_num_sources = exe._set_up_graph(graph, m_topology, this);
            // 标记已进入抢占窗口 + 置隐式锚点（捕获子图内未归档异常）
            m_implicit |= Work::Implicit::PREEMPTED;
        } else {
            // 重入：子图上一轮执行完毕，通知观察者本轮结束
            _notify_after(wr);
        }

        // ── 终止判定：三条件任一成立即结束循环 ──
        //   pred == true    → 用户谓词决定停止迭代
        //   join_weight == 0 → 子图为空或所有节点均有前驱（无法启动）
        //   _should_abort()   → 拓扑层面已被异常终止
        if (std::invoke_r<bool>(m_pred) || m_num_sources == 0 || _should_abort()) {
            m_implicit &= ~Work::Implicit::PREEMPTED;
            // release 阶段
            if (m_semaphores && !m_semaphores->releases.empty()) {
                SmallVector<Work*> waiters;
                _release_semaphores(waiters);
                exe._schedule_from_semaphore(wr, waiters);
            }        // 归还信号量配额，唤醒等待者
            exe._tear_down_task(this, wr, cache);                 // 走静态图依赖传播，通知后继节点
        } else {
            // ── 启动子图：设置屏障并批量调度源节点 ──
            _notify_before(wr);                             // 通知观察者"本节点即将开始新一轮"
            m_join_counter.store(m_num_sources, std::memory_order_relaxed); // 倒计时屏障
            exe._schedule(wr, graph.begin(), m_num_sources);// 批量投递源节点进调度队列
        }
    }
};

/// @brief 与独立 Topology 锁死的顶级基础异步任务，触发后自我燃烧并销毁。
template <typename A, typename F, typename... Args>
class DetachedBasicInvoker final : private TopologyHolder, public BasicWork {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;

    using Work::m_parent;
public:
    template <typename U, typename... Us>
        requires (sizeof...(Args) == sizeof...(Us)) && std::constructible_from<F, U&&> && (std::constructible_from<Args, Us&&> && ...)
    explicit DetachedBasicInvoker(Executor* exec, Work* parent, U&& f, Us&&... args)
        : TopologyHolder{exec}
        , BasicWork{detail::anchor_implicit<A>(),
                    detail::anchor_explicit<A>(),
                    &m_local_topology,
                    parent}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        try {
            if constexpr (sizeof...(Args) == 0) {
                std::invoke(m_func);
            } else {
                std::apply([this](auto&&... a) {
                    std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))...);
                }, m_args);
            }
        } catch (...) {
            _process_exception();
        }
        exe._tear_down_async_task(this, wr, cache);
    }
};

/// @brief 与独立 Topology 锁死的顶级扩展异步任务。
template <typename A, typename F, typename... Args>
class DetachedRuntimeInvoker final : private TopologyHolder, public RuntimeWork {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires (sizeof...(Args) == sizeof...(Us)) && std::constructible_from<F, U&&> && (std::constructible_from<Args, Us&&> && ...)
    explicit DetachedRuntimeInvoker(Executor* exec, Work* parent, U&& f, Us&&... args)
        : TopologyHolder{exec}
        , RuntimeWork{detail::anchor_implicit<A>(),
                      detail::anchor_explicit<A>(),
                      &m_local_topology,
                      parent}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        // ── 首次进入
        if ((m_implicit & Work::Implicit::PREEMPTED) == 0) {

            // 进入抢占窗口：置抢占 + 锚点动态位；body 自身占位 counter
            m_implicit |= Work::Implicit::PREEMPTED;
            m_join_counter.fetch_add(1, std::memory_order_release);

            Runtime rt(*this, wr, exe);
            try {
                if constexpr (sizeof...(Args) == 0) {
                    std::invoke(m_func, rt);
                } else {
                    std::apply([this, &rt](auto&&... a) {
                        std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., rt);
                    }, m_args);
                }
            } catch (...) {
                _process_exception();
            }

            // Last-arriver 仲裁：
            //   fetch_sub 返回 1 → counter 已归 0，无在飞 child，fallthrough 走 tear_down
            //   否则            → counter > 0，仍有 child，return 让步；最后完成的 child
            //                     负责把本节点重新入队触发第二次进入。
            if (m_join_counter.fetch_sub(1, std::memory_order_acq_rel) != 1) {
                return;   // 抢占退出：不做 tear_down，由最后到达的 child 触发重入
            }
            // Fallthrough：没有在飞 child，继续走到 tear_down
        }

        // ── 第二次进入
        m_implicit &= ~Work::Implicit::PREEMPTED;
        exe._tear_down_async_task(this, wr, cache);
    }
};


/// @brief fire-and-forget 异步子图执行器 —— 无 promise 通道。
template <typename A, typename GhStore, typename P, typename C>
class DetachedFlowInvoker final : private TopologyHolder, public GraphWork<GhStore> {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

    using GraphWork<GhStore>::m_gh_store;
    using Work::m_implicit;
    using Work::m_topology;
    using Work::m_join_counter;
    using Work::_should_abort;

    std::size_t m_num_sources{0};
    P m_pred;
    C m_callback;
public:
    template <typename Ghs, typename V, typename W>
    explicit DetachedFlowInvoker(Executor* exec, Work* parent, Ghs&& ghs, V&& pred, W&& cb)
        : TopologyHolder{exec}
        , GraphWork<GhStore>{std::forward<Ghs>(ghs),
                             detail::anchor_implicit<A>(),
                             detail::anchor_explicit<A>(),
                             &m_local_topology,
                             parent}
        , m_pred{std::forward<V>(pred)}
        , m_callback{std::forward<W>(cb)} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        auto& graph = detail::to_graph(detail::borrow(m_gh_store));

        // ── 首次进入：准入检查 + 子图初始化 ──────────────────
        if ((m_implicit & Work::Implicit::PREEMPTED) == 0) {
            // 初始化子图拓扑：重置各节点 join_counter、清除异常残留、
            // 将零入度源节点 swap 到 graph 前端，返回源节点数量
            m_num_sources = exe._set_up_graph(graph, m_topology, this);
            // 标记已进入抢占窗口 + 置隐式锚点（捕获子图内未归档异常）
            m_implicit |= Work::Implicit::PREEMPTED;
        }

        // ── 终止判定：三条件任一成立即结束循环 ──
        //   pred == true    → 用户谓词决定停止迭代
        //   join_weight == 0 → 子图为空或所有节点均有前驱（无法启动）
        //   _should_abort()   → 拓扑层面已被异常终止
        if (std::invoke_r<bool>(m_pred) || m_num_sources == 0 || _should_abort()) {
            m_implicit &= ~Work::Implicit::PREEMPTED;
            std::invoke(m_callback);                        // 用户终止回调（生命周期落幕通知）
            exe._tear_down_async_task(this, wr, cache);       // 拆除异步依赖链，传播完成信号给下游
        } else {
            m_join_counter.store(m_num_sources, std::memory_order_relaxed); // 倒计时屏障
            exe._schedule(wr, graph.begin(), m_num_sources);// 批量投递源节点进调度队列
        }
    }
};


/// @brief 内部熔接了 promise 的 Promise 同步通道基础异步任务。
template <typename A, typename F, typename R, typename... Args>
class PromisedBasicInvoker final : private TopologyHolder, public BasicWork {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
    std::promise<R> m_promise;
public:
    template <typename U, typename... Us>
        requires (sizeof...(Args) == sizeof...(Us)) && std::constructible_from<F, U&&> && (std::constructible_from<Args, Us&&> && ...)
    explicit PromisedBasicInvoker(Executor* exec, Work* parent, U&& f, std::promise<R>&& p, Us&&... args)
        : TopologyHolder{exec}
        , BasicWork{detail::anchor_implicit<A>(),
                    detail::anchor_explicit<A>(),
                    &m_local_topology,
                    parent}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...}
        , m_promise{std::move(p)} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        try {
            if constexpr (std::is_void_v<R>) {
                if constexpr (sizeof...(Args) == 0) {
                    if constexpr (basic_invocable_plain<F>) {
                        std::invoke(m_func);
                    } else {
                        std::invoke(m_func, _stop_token());
                    }
                } else {
                    std::apply([this](auto&&... a) {
                        if constexpr (basic_invocable_plain<F, decltype(a)...>) {
                            std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))...);
                        } else {
                            std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., _stop_token());
                        }
                    }, m_args);
                }
                m_promise.set_value();
            } else {
                if constexpr (sizeof...(Args) == 0) {
                    if constexpr (basic_invocable_plain<F>) {
                        m_promise.set_value(std::invoke(m_func));
                    } else {
                        m_promise.set_value(std::invoke(m_func, _stop_token()));
                    }
                } else {
                    m_promise.set_value(std::apply([this](auto&&... a) {
                        if constexpr (basic_invocable_plain<F, decltype(a)...>) {
                            return std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))...);
                        } else {
                            return std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., _stop_token());
                        }
                    }, m_args));
                }
            }
        } catch (...) {
            _process_exception();
        }

        if (m_exception_ptr) {
            m_promise.set_exception(m_exception_ptr);
        }

        exe._tear_down_async_task(this, wr, cache);
    }
};

/// @brief 内部熔接了 promise 的 Promise 同步通道扩展异步任务。
template <typename A, typename F, typename R, typename... Args>
class PromisedRuntimeInvoker final : private TopologyHolder, public RuntimeWork {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
    std::promise<R> m_promise;
    // non-void 时暂存 body 返回值，跨越 "抢占 → 重入" 两次 invoke
    TFL_NO_UNIQUE_ADDRESS std::conditional_t<std::is_void_v<R>, std::monostate, std::optional<R>> m_result;
public:
    template <typename U, typename... Us>
        requires (sizeof...(Args) == sizeof...(Us)) && std::constructible_from<F, U&&> && (std::constructible_from<Args, Us&&> && ...)
    explicit PromisedRuntimeInvoker(Executor* exec, Work* parent, U&& f, std::promise<R>&& p, Us&&... args)
        : TopologyHolder{exec}
        , RuntimeWork{detail::anchor_implicit<A>(),
                      detail::anchor_explicit<A>(),
                      &m_local_topology,
                      parent}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...}
        , m_promise{std::move(p)} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        // ── 首次进入
        if ((m_implicit & Work::Implicit::PREEMPTED) == 0) {

            // 进入抢占窗口：置抢占 + 锚点动态位；body 自身占位 counter
            m_implicit |= Work::Implicit::PREEMPTED;
            m_join_counter.fetch_add(1, std::memory_order_release);

            Runtime rt(*this, wr, exe);
            try {
                if constexpr (std::is_void_v<R>) {
                    if constexpr (sizeof...(Args) == 0) {
                        if constexpr (runtime_invocable_plain<F>) {
                            std::invoke(m_func, rt);
                        } else {
                            std::invoke(m_func, rt, _stop_token());
                        }
                    } else {
                        std::apply([this, &rt](auto&&... a) {
                            if constexpr (runtime_invocable_plain<F, decltype(a)...>) {
                                std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., rt);
                            } else {
                                std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., rt, _stop_token());
                            }
                        }, m_args);
                    }
                } else {
                    if constexpr (sizeof...(Args) == 0) {
                        if constexpr (runtime_invocable_plain<F>) {
                            m_result.emplace(std::invoke(m_func, rt));
                        } else {
                            m_result.emplace(std::invoke(m_func, rt, _stop_token()));
                        }
                    } else {
                        m_result.emplace(std::apply([this, &rt](auto&&... a) {
                            if constexpr (runtime_invocable_plain<F, decltype(a)...>) {
                                return std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., rt);
                            } else {
                                return std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., rt, _stop_token());
                            }
                        }, m_args));
                    }
                }
            } catch (...) {
                //走归档协议：异常写入 m_exception_ptr + CAUGHT 位 CAS 胜出者留底
                // body 异常和可能并发的 child 异常以"第一个到达者"为准
                _process_exception();
            }

            // Last-arriver 仲裁：
            //   fetch_sub 返回 1 → counter 已归 0，无在飞 child，fallthrough 走 tear_down
            //   否则            → counter > 0，仍有 child，return 让步；最后完成的 child
            //                     负责把本节点重新入队触发第二次进入。
            if (m_join_counter.fetch_sub(1, std::memory_order_acq_rel) != 1) {
                return;   // 抢占退出：不做 tear_down，由最后到达的 child 触发重入
            }
        }

        // ── 第二次进入（或首次即完成）：根据 m_exception_ptr 决定 promise ─
        m_implicit &= ~Work::Implicit::PREEMPTED;

        if (m_exception_ptr) {
            // body 或某个 child 归档了异常
            m_promise.set_exception(m_exception_ptr);
        } else {
            if constexpr (std::is_void_v<R>) {
                m_promise.set_value();
            } else {
                // m_result 此刻一定有值——body 无异常完成的前提下
                m_promise.set_value(std::move(*m_result));
            }
        }

        exe._tear_down_async_task(this, wr, cache);
    }
};



/// @brief 能够串接任意 Flow 实体的巨无霸容器节点，并在生命周期落幕时点燃专有回调。
template <typename A, typename GhStore, typename P, typename C>
class PromisedFlowInvoker final : private TopologyHolder, public GraphWork<GhStore> {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

    using GraphWork<GhStore>::m_gh_store;
    using Work::m_implicit;
    using Work::m_topology;
    using Work::m_join_counter;
    using Work::m_exception_ptr;
    using Work::_should_abort;

    std::size_t          m_num_sources{0};
    P                    m_pred;
    C                    m_callback;
    std::promise<void>   m_promise;

public:
    template <typename Ghs, typename V, typename W>
    explicit PromisedFlowInvoker(Executor* exec, Work* parent, Ghs&& ghs, V&& pred, W&& cb, std::promise<void>&& p)
        : TopologyHolder{exec}
        , GraphWork<GhStore>{std::forward<Ghs>(ghs),
                             detail::anchor_implicit<A>(),
                             detail::anchor_explicit<A>(),
                             &m_local_topology,
                             parent}
        , m_pred{std::forward<V>(pred)}
        , m_callback{std::forward<W>(cb)}
        , m_promise{std::move(p)} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        auto& graph = detail::to_graph(detail::borrow(m_gh_store));

        // ── 首次进入：准入检查 + 子图初始化 ─────────────────────────────
        if ((m_implicit & Work::Implicit::PREEMPTED) == 0) {
            // Why: _set_up_graph 重置子图各节点 join_counter、清除异常残留位、
            //      将零入度源节点 swap 到 graph 前端，返回源节点数量
            m_num_sources = exe._set_up_graph(graph, m_topology, this);
            // 标记进入抢占窗口（隐式锚点已由 anchor::A 静态决定）
            m_implicit |= Work::Implicit::PREEMPTED;
        }

        if (std::invoke_r<bool>(m_pred) || m_num_sources == 0 || _should_abort()) {
            m_implicit &= ~Work::Implicit::PREEMPTED;

            std::invoke(m_callback);

            if (m_exception_ptr) {
                m_promise.set_exception(m_exception_ptr);
            } else {
                m_promise.set_value();
            }

            exe._tear_down_async_task(this, wr, cache);
        } else {
            m_join_counter.store(m_num_sources, std::memory_order_relaxed); // 倒计时屏障
            exe._schedule(wr, graph.begin(), m_num_sources);                // 批量投递源节点
        }
    }
};

/// @brief 被高阶外部拓扑锁定的依赖型常规任务，需完成 CAS 抢占才能挂载入依赖树。
template <typename A, typename F, typename... Args>
class AttachedBasicInvoker final : private TopologyHolder, public BasicWork {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires (sizeof...(Args) == sizeof...(Us)) && std::constructible_from<F, U&&> && (std::constructible_from<Args, Us&&> && ...)
    explicit AttachedBasicInvoker(Executor* exec, Work* parent, U&& f, Us&&... args)
        : TopologyHolder{exec}
        , BasicWork{detail::anchor_implicit<A>(),
                    detail::anchor_explicit<A>(),
                    &m_local_topology,
                    parent}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        // acquire 阶段
        if (m_semaphores && !m_semaphores->acquires.empty()) {
            SmallVector<Work*> waiters;
            if (!_try_acquire_semaphores(waiters)) {
                exe._schedule_from_semaphore(wr, waiters);
                return;
            }
        }

        _notify_before(wr);

        try {
            if constexpr (sizeof...(Args) == 0) {
                if constexpr (basic_invocable_plain<F>) {
                    std::invoke(m_func);
                } else {
                    std::invoke(m_func, _stop_token());
                }
            } else {
                std::apply([this](auto&&... a) {
                    if constexpr (basic_invocable_plain<F, decltype(a)...>) {
                        std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))...);
                    } else {
                        std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., _stop_token());
                    }
                }, m_args);
            }
        } catch (...) {
            _process_exception();
        }

        _notify_after(wr);
        // release 阶段
        if (m_semaphores && !m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            _release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }
        exe._tear_down_dep_async_task(this, wr, cache);
    }
};

/// @brief 被高阶外部拓扑锁定的依赖型动态挂载任务。
template <typename A, typename F, typename... Args>
class AttachedRuntimeInvoker final : private TopologyHolder, public RuntimeWork {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires (sizeof...(Args) == sizeof...(Us)) && std::constructible_from<F, U&&> && (std::constructible_from<Args, Us&&> && ...)
    explicit AttachedRuntimeInvoker(Executor* exec, Work* parent, U&& f, Us&&... args)
        : TopologyHolder{exec}
        , RuntimeWork{detail::anchor_implicit<A>(),
                      detail::anchor_explicit<A>(),
                      &m_local_topology,
                      parent}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        // ── 首次进入
        if ((m_implicit & Work::Implicit::PREEMPTED) == 0) {

            // acquire 阶段（仅首次进入执行）
            if (m_semaphores && !m_semaphores->acquires.empty()) {
                SmallVector<Work*> waiters;
                if (!_try_acquire_semaphores(waiters)) {
                    exe._schedule_from_semaphore(wr, waiters);
                    return;
                }
            }

            // 进入抢占窗口：置抢占 + 锚点动态位；body 自身占位 counter
            m_implicit |= Work::Implicit::PREEMPTED;
            m_join_counter.fetch_add(1, std::memory_order_release);

            _notify_before(wr);

            Runtime rt(*this, wr, exe);
            try {
                if constexpr (sizeof...(Args) == 0) {
                    if constexpr (runtime_invocable_plain<F>) {
                        std::invoke(m_func, rt);
                    } else {
                        std::invoke(m_func, rt, _stop_token());
                    }
                } else {
                    std::apply([this, &rt](auto&&... a) {
                        if constexpr (runtime_invocable_plain<F, decltype(a)...>) {
                            std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., rt);
                        } else {
                            std::invoke(m_func, detail::borrow(std::forward<decltype(a)>(a))..., rt, _stop_token());
                        }
                    }, m_args);
                }
            } catch (...) {
                _process_exception();
            }

            // Last-arriver 仲裁：
            //   fetch_sub 返回 1 → counter 已归 0，无在飞 child，fallthrough 走 tear_down
            //   否则            → counter > 0，仍有 child，return 让步；最后完成的 child
            //                     负责把本节点重新入队触发第二次进入。
            if (m_join_counter.fetch_sub(1, std::memory_order_acq_rel) != 1) {
                return;   // 抢占退出：不做 tear_down，由最后到达的 child 触发重入
            }
            // Fallthrough：没有在飞 child，继续走到 tear_down
        }

        _notify_after(wr);

        // ── 第二次进入
        m_implicit &= ~Work::Implicit::PREEMPTED;

        // release 阶段
        if (m_semaphores && !m_semaphores->releases.empty()) {
            SmallVector<Work*> waiters;
            _release_semaphores(waiters);
            exe._schedule_from_semaphore(wr, waiters);
        }

        exe._tear_down_dep_async_task(this, wr, cache);
    }
};

/// @brief 能够串接任意 Flow 实体的巨无霸容器节点，并在生命周期落幕时点燃专有回调。
template <typename A, typename GhStore, typename P, typename C>
class AttachedFlowInvoker final : private TopologyHolder, public GraphWork<GhStore> {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

    using GraphWork<GhStore>::m_gh_store;
    using Work::m_implicit;
    using Work::m_semaphores;
    using Work::m_topology;
    using Work::m_join_counter;
    using Work::_should_abort;
    using Work::_notify_before;
    using Work::_notify_after;
    using Work::_try_acquire_semaphores;
    using Work::_release_semaphores;

    std::size_t m_num_sources{0};
    P m_pred;
    C m_callback;
public:
    template <typename Ghs, typename V, typename W>
    explicit AttachedFlowInvoker(Executor* exec, Work* parent, Ghs&& ghs, V&& pred, W&& cb)
        : TopologyHolder{exec}
        , GraphWork<GhStore>{std::forward<Ghs>(ghs),
                             detail::anchor_implicit<A>(),
                             detail::anchor_explicit<A>(),
                             &m_local_topology, parent}
        , m_pred{std::forward<V>(pred)}
        , m_callback{std::forward<W>(cb)} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        auto& graph = detail::to_graph(detail::borrow(m_gh_store));

        // ── 首次进入：准入检查 + 子图初始化 ──────────────────
        if ((m_implicit & Work::Implicit::PREEMPTED) == 0) {
            // 首次进入：竞争获取信号量，失败则让出执行权等待唤醒
            // acquire 阶段
            if (m_semaphores && !m_semaphores->acquires.empty()) {
                SmallVector<Work*> waiters;
                if (!_try_acquire_semaphores(waiters)) {
                    exe._schedule_from_semaphore(wr, waiters);
                    return;
                }
            }
            // 初始化子图拓扑：重置各节点 join_counter、清除异常残留、
            // 将零入度源节点 swap 到 graph 前端，返回源节点数量
            m_num_sources = exe._set_up_graph(graph, m_topology, this);
            // 标记已进入抢占窗口 + 置隐式锚点（捕获子图内未归档异常）
            m_implicit |= Work::Implicit::PREEMPTED;
        } else {
            // 重入：子图上一轮执行完毕，通知观察者本轮结束
            _notify_after(wr);
        }

        // ── 终止判定：三条件任一成立即结束循环 ──
        //   pred == true    → 用户谓词决定停止迭代
        //   join_weight == 0 → 子图为空或所有节点均有前驱（无法启动）
        //   _should_abort()   → 拓扑层面已被异常终止
        if (std::invoke_r<bool>(m_pred) || m_num_sources == 0 || _should_abort()) {
            m_implicit &= ~Work::Implicit::PREEMPTED;
            // release 阶段
            if (m_semaphores && !m_semaphores->releases.empty()) {
                SmallVector<Work*> waiters;
                _release_semaphores(waiters);
                exe._schedule_from_semaphore(wr, waiters);
            }        // 归还信号量配额，唤醒等待者
            std::invoke(m_callback);                        // 用户终止回调（生命周期落幕通知）
            exe._tear_down_dep_async_task(this, wr, cache);       // 拆除异步依赖链，传播完成信号给下游
        } else {
            // ── 启动子图：设置屏障并批量调度源节点 ──
            _notify_before(wr);                             // 通知观察者"本节点即将开始新一轮"
            m_join_counter.store(m_num_sources, std::memory_order_relaxed); // 倒计时屏障
            exe._schedule(wr, graph.begin(), m_num_sources);// 批量投递源节点进调度队列
        }
    }
};

/// @brief 虚拟锚点元。在图解析和异步链排布中充当零损耗的汇聚地。
template <typename A>
class AnchorWork final : private TopologyHolder, public NoopWork {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

public:
    explicit AnchorWork(Executor* exec, Work* parent)
        : TopologyHolder{exec}
        , Work{detail::anchor_implicit<A>(),
               detail::anchor_explicit<A>(),
               &m_local_topology,
               parent} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {}
};


/// @brief 协作式等待子图完成 —— 在当前 `Runtime` 上下文中执行 `gh`。
///
/// @tparam Gh  满足 `graph_holder` 的 Graph 持有者类型。
/// @param gh   待执行的子图。必须存活到本调用返回。
template <typename Gh>
    requires graph_holder<Gh>
inline void Runtime::cowait(Gh& gh) {
    _assert_owner();
    auto& graph = detail::to_graph(gh);
    if (graph.empty()) {
        return;
    }

    // 一次性栈锚点 —— 隔离本次 corun 的 join 计数与异常归档,
    // 避免污染外层 m_work 的状态(m_work 当前正处于自锚定基线 1)。
    AnchorWork<anchor::explicit_t> anchor{std::addressof(m_executor), std::addressof(m_work)};

    {
        // 把锚点临时置为显式锚定,使本作用域内派生的子链异常归档到此节点。
        ExplicitAnchorGuard guard{std::addressof(anchor)};
        m_executor._cowait_graph(m_worker, graph, std::addressof(anchor));
    }

    anchor._rethrow_exception();
}

}  // namespace tfl

