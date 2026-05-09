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
///   - 动态依赖任务              `DepAsync*Invoker`
///   - 延迟启动动态依赖任务       `DepDeferredAsync*Invoker`
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
/// @see Work             所有子类的基类
/// @see work_memory.hpp  本文件类型的 make_xxx 工厂
/// @see Executor         调度与 tear_down 协议
///
namespace detail {

template <typename A>
consteval std::pair<Work::Implicit::type, Work::Explicit::type> anchor_bits() noexcept {
    if constexpr (std::same_as<A, anchor::none_t>) {
        return {Work::Implicit::NONE,     Work::Explicit::NONE};
    } else if constexpr (std::same_as<A, anchor::implicit_t>) {
        return {Work::Implicit::ANCHORED, Work::Explicit::NONE};
    } else /* anchor::explicit_t */ {
        return {Work::Implicit::NONE,     Work::Explicit::ANCHORED};
    }
}

} // namespace detail

// 辅助基类，仅负责持有 Topology
struct TopologyHolder {
    Topology m_local_topology;
    explicit TopologyHolder(Executor& exec) : m_local_topology{exec} {}
};

// ============================================================================
//  TaskType::Basic 家族
// ============================================================================
class BasicWork : public Work {
protected:
    template <typename... Xs>
    explicit BasicWork(Xs&&... xs) noexcept
        : Work{TaskType::Basic, std::forward<Xs>(xs)...} {}

    void dump(std::ostream& os) const override final {
        D2Renderer::render_work(os, this, "rectangle",
                                "#f5f5f5", "#9ca3af", "#1f2937", "8");
    }
};

// ============================================================================
//  TaskType::Runtime 家族
// ============================================================================
class RuntimeWork : public Work {
protected:
    template <typename... Xs>
    explicit RuntimeWork(Xs&&... xs) noexcept
        : Work{TaskType::Runtime, std::forward<Xs>(xs)...} {}

    void dump(std::ostream& os) const override final {
        D2Renderer::render_work(os, this, "rectangle",
                                "#fce4ec", "#e57373", "#6d1b1b", "30");
    }
};

// ============================================================================
//  TaskType::Branch (diamond)
// ============================================================================
class BranchWork : public Work {
protected:
    template <typename... Xs>
    explicit BranchWork(Xs&&... xs) noexcept
        : Work{TaskType::Branch, std::forward<Xs>(xs)...} {}

    void dump(std::ostream& os) const override final {
        D2Renderer::render_work(os, this, "diamond",
                                "#dbeafe", "#3b82f6", "#1e3a5f", "8");
    }
};

// ============================================================================
//  TaskType::MultiBranch (hexagon)
// ============================================================================
class MultiBranchWork : public Work {
protected:
    template <typename... Xs>
    explicit MultiBranchWork(Xs&&... xs) noexcept
        : Work{TaskType::MultiBranch, std::forward<Xs>(xs)...} {}

    void dump(std::ostream& os) const override final {
        D2Renderer::render_work(os, this, "hexagon",
                                "#bfdbfe", "#2563eb", "#1e3a5f", "8");
    }
};

// ============================================================================
//  TaskType::Jump (dashed diamond)
// ============================================================================
class JumpWork : public Work {
protected:
    template <typename... Xs>
    explicit JumpWork(Xs&&... xs) noexcept
        : Work{TaskType::Jump, std::forward<Xs>(xs)...} {}

    void dump(std::ostream& os) const override final {
        D2Renderer::render_work(os, this, "diamond",
                                "#fee2e2", "#ef4444", "#7f1d1d", "8", "5");
    }
};

// ============================================================================
//  TaskType::MultiJump (dashed hexagon)
// ============================================================================
class MultiJumpWork : public Work {
protected:
    template <typename... Xs>
    explicit MultiJumpWork(Xs&&... xs) noexcept
        : Work{TaskType::MultiJump, std::forward<Xs>(xs)...} {}

    void dump(std::ostream& os) const override final {
        D2Renderer::render_work(os, this, "hexagon",
                                "#fecaca", "#dc2626", "#7f1d1d", "8", "5");
    }
};

// ============================================================================
//  TaskType::Graph (内嵌 Flow 的容器节点)
// ============================================================================
template <typename GhStore>
class GraphWork : public Work {
protected:
    GhStore m_gh_store;

    template <typename U, typename... Xs>
    explicit GraphWork(U&& gh, Xs&&... xs) noexcept
        : Work{TaskType::Graph, std::forward<Xs>(xs)...}
        , m_gh_store{std::forward<U>(gh)} {}

    void dump(std::ostream& os) const override final {
        const auto& graph = detail::to_graph(detail::unwrap(m_gh_store));
        D2Renderer::render_graph(os, this, to_string(m_type), graph);
    }
};

// ============================================================================
//  衍生功能组件定义 (Derived Components)
// ============================================================================

/// @brief 承载 `void()` 标准闭包的基础同步工作元。
template <typename F, typename... Args>
class BasicInvoker final : public BasicWork {
    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
    // requires std::constructible_from<F, U> && (std::constructible_from<Args, Us> && ...)
    explicit BasicInvoker(const Graph* g, U&& f, Us&&... args)
        : BasicWork{Work::Implicit::WEIGHT_1, Work::Explicit::NONE, g}
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
                        std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))...);
                    } else {
                        std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., _stop_token());
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
    // requires std::constructible_from<F, U> && (std::constructible_from<Args, Us> && ...)
    explicit BranchInvoker(const Graph* g, U&& f, Us&&... args)
        : BranchWork{Work::Implicit::WEIGHT_2, Work::Explicit::NONE, g}
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
                        std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., branch);
                    } else {
                        std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., branch, _stop_token());
                    }
                }, m_args);
            }
        } catch (...) {
            _process_exception();
        }

        if (auto target = branch.m_target) {
            target->m_join_counter.fetch_sub(1, std::memory_order_relaxed);
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

/// @brief 为闭包注入多线广播挑选权杖 `MultiBranch&` 的并行激活工作元。
template <typename F, typename... Args>
class MultiBranchInvoker final : public MultiBranchWork {
    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
    // requires std::constructible_from<F, U> && (std::constructible_from<Args, Us> && ...)
    explicit MultiBranchInvoker(const Graph* g, U&& f, Us&&... args)
        : MultiBranchWork{Work::Implicit::WEIGHT_2, Work::Explicit::NONE, g}
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
                        std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., branch);
                    } else {
                        std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., branch, _stop_token());
                    }
                }, m_args);
            }
        } catch (...) {
            _process_exception();
        }

        for (auto* target : branch.m_targets) {
            target->m_join_counter.fetch_sub(1, std::memory_order_relaxed);
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

/// @brief 授权在图内执行强制物理传送跃迁指令 `Jump&` 的中断工作元。
template <typename F, typename... Args>
class JumpInvoker final : public JumpWork {
    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
    // requires std::constructible_from<F, U> && (std::constructible_from<Args, Us> && ...)
    explicit JumpInvoker(const Graph* g, U&& f, Us&&... args)
        : JumpWork{Work::Implicit::WEIGHT_0, Work::Explicit::NONE, g}
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
                        std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., jmp);
                    } else {
                        std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., jmp, _stop_token());
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
    // requires std::constructible_from<F, U> && (std::constructible_from<Args, Us> && ...)
    explicit MultiJumpInvoker(const Graph* g, U&& f, Us&&... args)
        : MultiJumpWork{Work::Implicit::WEIGHT_0, Work::Explicit::NONE, g}
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
                        std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., jmp);
                    } else {
                        std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., jmp, _stop_token());
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
    // requires std::constructible_from<F, U> && (std::constructible_from<Args, Us> && ...)
    explicit RuntimeInvoker(const Graph* g, U&& f, Us&&... args)
        : RuntimeWork{Work::Implicit::WEIGHT_1, Work::Explicit::NONE, g}
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
                            std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt);
                        } else {
                            std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt, _stop_token());
                        }
                    }, m_args);
                }
            } catch (...) {
                _process_exception();
            }

            _notify_after(wr);

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
        : GraphWork<GhStore>{std::forward<Ghs>(ghs), Work::Implicit::WEIGHT_1, Work::Explicit::NONE, g}
        , m_pred{std::forward<V>(pred)} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        auto& graph = detail::to_graph(detail::unwrap(m_gh_store));

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
class SilentAsyncBasicInvoker final : public BasicWork {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;

    using Work::m_parent;
public:
    template <typename U, typename... Us>
    // requires std::constructible_from<F, U> && (std::constructible_from<Args, Us> && ...)
    explicit SilentAsyncBasicInvoker(Work* parent, U&& f, Us&&... args)
        : BasicWork{detail::anchor_bits<A>().first,
                    detail::anchor_bits<A>().second,
                    nullptr,
                    parent}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        try {
            if constexpr (sizeof...(Args) == 0) {
                std::invoke(m_func);
            } else {
                std::apply([this](auto&&... a) {
                    std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))...);
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
class SilentAsyncRuntimeInvoker final : public RuntimeWork {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
    // requires std::constructible_from<F, U> && (std::constructible_from<Args, Us> && ...)
    explicit SilentAsyncRuntimeInvoker(Work* parent, U&& f, Us&&... args)
        : RuntimeWork{detail::anchor_bits<A>().first,
                      detail::anchor_bits<A>().second,
                      nullptr,
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
                        std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt);
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


template <typename A, typename GhStore, typename P, typename C>
class SilentAsyncFlowInvoker final : public GraphWork<GhStore> {
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
    explicit SilentAsyncFlowInvoker(Work* parent, Ghs&& ghs, V&& pred, W&& cb)
        : GraphWork<GhStore>{std::forward<Ghs>(ghs),
                               detail::anchor_bits<A>().first,
                               detail::anchor_bits<A>().second,
                               nullptr,
                               parent}
        , m_pred{std::forward<V>(pred)}
        , m_callback{std::forward<W>(cb)} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        auto& graph = detail::to_graph(detail::unwrap(m_gh_store));

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
class AsyncBasicInvoker final : private TopologyHolder, public BasicWork {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
    std::promise<R> m_promise;
public:
    template <typename U, typename... Us>
    // requires std::constructible_from<F, U> && (std::constructible_from<Args, Us> && ...)
    explicit AsyncBasicInvoker(Executor& exec, Work* parent, U&& f, std::promise<R>&& p, Us&&... args)
        : TopologyHolder{exec}
        , BasicWork{detail::anchor_bits<A>().first,
                    detail::anchor_bits<A>().second,
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
                            std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))...);
                        } else {
                            std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., _stop_token());
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
                            return std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))...);
                        } else {
                            return std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., _stop_token());
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
class AsyncRuntimeInvoker final : private TopologyHolder, public RuntimeWork {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
    std::promise<R> m_promise;
    // non-void 时暂存 body 返回值，跨越 "抢占 → 重入" 两次 invoke
    TFL_NO_UNIQUE_ADDRESS std::conditional_t<std::is_void_v<R>, std::monostate, std::optional<R>> m_result;
public:
    template <typename U, typename... Us>
    // requires std::constructible_from<F, U> && (std::constructible_from<Args, Us> && ...)
    explicit AsyncRuntimeInvoker(Executor& exec, Work* parent, U&& f, std::promise<R>&& p, Us&&... args)
        : TopologyHolder{exec}
        , RuntimeWork{detail::anchor_bits<A>().first,
                      detail::anchor_bits<A>().second,
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
                                std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt);
                            } else {
                                std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt, _stop_token());
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
                                return std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt);
                            } else {
                                return std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt, _stop_token());
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
            // Fallthrough：没有在飞 child，继续走到 tear_down
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
class AsyncFlowInvoker final : private TopologyHolder, public GraphWork<GhStore> {
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
    explicit AsyncFlowInvoker(Executor& exec, Work* parent, Ghs&& ghs, V&& pred, W&& cb, std::promise<void>&& p)
        : TopologyHolder{exec}
        , GraphWork<GhStore>{std::forward<Ghs>(ghs),
                               detail::anchor_bits<A>().first,
                               detail::anchor_bits<A>().second,
                               &m_local_topology,
                               parent}
        , m_pred{std::forward<V>(pred)}
        , m_callback{std::forward<W>(cb)}
        , m_promise{std::move(p)} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        auto& graph = detail::to_graph(detail::unwrap(m_gh_store));

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
class DepAsyncBasicInvoker final : private TopologyHolder, public BasicWork {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
    // requires std::constructible_from<F, U> && (std::constructible_from<Args, Us> && ...)
    explicit DepAsyncBasicInvoker(Executor& exec, Work* parent, U&& f, Us&&... args)
        : TopologyHolder{exec}
        , BasicWork{detail::anchor_bits<A>().first,
                    detail::anchor_bits<A>().second,
                    &m_local_topology,
                    parent}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
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
                        std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))...);
                    } else {
                        std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., _stop_token());
                    }
                }, m_args);
            }
        } catch (...) {
            _process_exception();
        }

        exe._tear_down_dep_async_task(this, wr, cache);
    }
};

/// @brief 被高阶外部拓扑锁定的依赖型动态挂载任务。
template <typename A, typename F, typename... Args>
class DepAsyncRuntimeInvoker final : private TopologyHolder, public RuntimeWork {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
    // requires std::constructible_from<F, U> && (std::constructible_from<Args, Us> && ...)
    explicit DepAsyncRuntimeInvoker(Executor& exec, Work* parent, U&& f, Us&&... args)
        : TopologyHolder{exec}
        , RuntimeWork{detail::anchor_bits<A>().first,
                      detail::anchor_bits<A>().second,
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
                    if constexpr (runtime_invocable_plain<F>) {
                        std::invoke(m_func, rt);
                    } else {
                        std::invoke(m_func, rt, _stop_token());
                    }
                } else {
                    std::apply([this, &rt](auto&&... a) {
                        if constexpr (runtime_invocable_plain<F, decltype(a)...>) {
                            std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt);
                        } else {
                            std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt, _stop_token());
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

        // ── 第二次进入
        m_implicit &= ~Work::Implicit::PREEMPTED;
        exe._tear_down_dep_async_task(this, wr, cache);
    }
};

/// @brief 能够串接任意 Flow 实体的巨无霸容器节点，并在生命周期落幕时点燃专有回调。
template <typename A, typename GhStore, typename P, typename C>
class DepAsyncFlowInvoker final : private TopologyHolder, public GraphWork<GhStore> {
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
    explicit DepAsyncFlowInvoker(Executor& exec, Work* parent, Ghs&& ghs, V&& pred, W&& cb)
        : TopologyHolder{exec}
        , GraphWork<GhStore>{std::forward<Ghs>(ghs),
                               detail::anchor_bits<A>().first,
                               detail::anchor_bits<A>().second,
                               &m_local_topology,
                               parent}
        , m_pred{std::forward<V>(pred)}
        , m_callback{std::forward<W>(cb)} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        auto& graph = detail::to_graph(detail::unwrap(m_gh_store));

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
            exe._tear_down_dep_async_task(this, wr, cache);       // 拆除异步依赖链，传播完成信号给下游
        } else {
            m_join_counter.store(m_num_sources, std::memory_order_relaxed); // 倒计时屏障
            exe._schedule(wr, graph.begin(), m_num_sources);// 批量投递源节点进调度队列
        }
    }
};


/// @brief 被高阶外部拓扑锁定的依赖型常规任务，需完成 CAS 抢占才能挂载入依赖树。
template <typename A, typename F, typename... Args>
class DepDeferredAsyncBasicInvoker final : private TopologyHolder, public BasicWork {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
    // requires std::constructible_from<F, U> && (std::constructible_from<Args, Us> && ...)
    explicit DepDeferredAsyncBasicInvoker(Executor& exec, Work* parent, U&& f, Us&&... args)
        : TopologyHolder{exec}
        , BasicWork{detail::anchor_bits<A>().first,
                    detail::anchor_bits<A>().second,
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
                        std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))...);
                    } else {
                        std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., _stop_token());
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
class DepDeferredAsyncRuntimeInvoker final : private TopologyHolder, public RuntimeWork {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

    F m_func;
    TFL_NO_UNIQUE_ADDRESS std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
    // requires std::constructible_from<F, U> && (std::constructible_from<Args, Us> && ...)
    explicit DepDeferredAsyncRuntimeInvoker(Executor& exec, Work* parent, U&& f, Us&&... args)
        : TopologyHolder{exec}
        , RuntimeWork{detail::anchor_bits<A>().first,
                      detail::anchor_bits<A>().second,
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
                            std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt);
                        } else {
                            std::invoke(m_func, detail::unwrap(std::forward<decltype(a)>(a))..., rt, _stop_token());
                        }
                    }, m_args);
                }
            } catch (...) {
                _process_exception();
            }

            _notify_after(wr);

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
class DepDeferredAsyncFlowInvoker final : private TopologyHolder, public GraphWork<GhStore> {
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
    explicit DepDeferredAsyncFlowInvoker(Executor& exec, Work* parent, Ghs&& ghs, V&& pred, W&& cb)
        : TopologyHolder{exec}
        , GraphWork<GhStore>{std::forward<Ghs>(ghs),
                               detail::anchor_bits<A>().first,
                               detail::anchor_bits<A>().second,
                               &m_local_topology, parent}
        , m_pred{std::forward<V>(pred)}
        , m_callback{std::forward<W>(cb)} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {
        auto& graph = detail::to_graph(detail::unwrap(m_gh_store));

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
class AnchorWork final : private TopologyHolder, public Work {
    static_assert(anchor_tag<A>, "A must be tfl::anchor::{none_t, implicit_t, explicit_t}");

public:
    explicit AnchorWork(Executor& exec, Work* parent)
        : TopologyHolder{exec}
        , Work{TaskType::None,
               detail::anchor_bits<A>().first,
               detail::anchor_bits<A>().second,
               &m_local_topology,
               parent} {}

    void invoke(Executor& exe, Worker& wr, Work*& cache) override final {}
    void dump(std::ostream& os) const override final {}
};


template <typename Gh>
    requires graph_holder<Gh>
inline void Runtime::cowait(Gh& gh) {
    auto& graph = detail::to_graph(detail::unwrap(gh));
    if (graph.empty()) {
        return;
    }

    // 一次性栈锚点 —— 隔离本次 corun 的 join 计数与异常归档,
    // 避免污染外层 m_work 的状态(m_work 当前正处于自锚定基线 1)。
    AnchorWork<anchor::explicit_t> anchor{m_executor, std::addressof(m_work)};

    {
        // 把锚点临时置为显式锚定,使本作用域内派生的子链异常归档到此节点。
        ExplicitAnchorGuard guard{std::addressof(anchor)};
        m_executor._cowait_graph(m_worker, graph, std::addressof(anchor));
    }

    anchor._rethrow_exception();
}

}  // namespace tfl

