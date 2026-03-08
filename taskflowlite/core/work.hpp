/// @file work.hpp
/// @brief DAG 任务节点基类 - 核心拓扑结构与依赖计数
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include <utility>
#include <expected>
#include <unordered_set>
#include <span>
#include <algorithm>
#include <memory>
#include <future>

#include "enums.hpp"
#include "utility.hpp"
#include "exception.hpp"
#include "traits.hpp"
#include "observer.hpp"
#include "topology.hpp"
#include "semaphore.hpp"

namespace tfl {

/// @brief DAG 任务节点基类
///
/// @details
/// 统一表示普通任务、条件分支、跳转、子流程、异步任务等所有节点类型。
/// 内部统筹管理节点间的依赖连边、信号量约束、异常传播链路和 D2 图形化。
///
/// @par 核心架构设计
/// - 多态分派：子类通过模板包装持有类型擦除后的 callable，利用 invoke() 虚函数统一执行
/// - 冷热数据分离：状态机与依赖计数器紧凑排列，提高 CPU L1 缓存行利用率
/// - 统一边存储：单一 vector 分段存储前驱和后继指针，消灭多重容器开销
///
/// @par 内存布局
/// @code
/// +------------------+  <- 冷数据区（访问频率低）
/// | m_name           |
/// | m_graph          |
/// | m_exception_ptr  |
/// +------------------+  <- 热数据区（访问频率高）
/// | m_state (atomic) |
/// | m_options        |
/// | m_topology       |
/// | m_parent         |
/// | m_join_counter   |
/// | m_num_successors |
/// | m_edges[]        |
/// +------------------+  <- 扩展区（按需分配）
/// | m_semaphores     |
/// | m_observers      |
/// +------------------+
/// @endcode
///
/// @note 此类为框架内部实现，用户通过 Task/AsyncTask/Flow API 间接操作
class Work : public Immovable<Work> {

    friend class Graph;
    friend class Flow;
    friend class Task;
    friend class TaskView;
    friend class AsyncTask;
    friend class Topology;
    friend class Worker;
    friend class Executor;
    friend class Runtime;
    friend class Semaphore;
    friend class Branch;
    friend class MultiBranch;
    friend class Jump;
    friend class MultiJump;

    TFL_WORK_SUBCLASS_FRIENDS;

public:
    /// @brief 静态选项配置位域（构建时确定，执行期只读）。
    ///
    /// 利用位域压缩技术，将布尔标志和构建期累计的入度计数（join count）
    /// 强行塞入单个 32 位无符号整数中，极致节省节点内存开销。
    struct Option {
        using type = std::uint32_t;

        static constexpr unsigned BITS       = sizeof(type) * 8;
        static constexpr unsigned FLAG_BITS  = 8;
        static constexpr unsigned COUNT_BITS = BITS - FLAG_BITS;

        static constexpr type COUNT_MASK = (type{1} << COUNT_BITS) - 1;
        static constexpr type FLAG_MASK  = ~COUNT_MASK;
        static constexpr type NONE      = 0;

        /// @brief 异常捕获锚点标志，指示该节点为异常向上冒泡的终点。
        static constexpr type ANCHORED  = type{1} << (BITS - 1);
        /// @brief 可抢占标志，指示该父级容器节点在子任务完成后应立即重获执行权。
        static constexpr type PREEMPTED = type{1} << (BITS - 2);
    };

protected:
    /// @brief 运行期状态位域（执行期通过原子指令并发修饰）。
    struct State {
        using type = std::uint32_t;

        static constexpr unsigned BITS       = sizeof(type) * 8;
        static constexpr type NONE      = 0;

        /// @brief 标记该节点在执行用户闭包的过程中抛出了异常。
        static constexpr type EXCEPTION = type{1} << (BITS - 1);
        /// @brief 标记抛出的异常已被本节点或上游的 ANCHORED 节点成功捕获归档。
        static constexpr type CAUGHT    = type{1} << (BITS - 2);
    };

    /// @brief 节点绑定的信号量集合（按需延迟分配以省内存）。
    struct SemaphoreData {
        std::vector<Semaphore*> acquires; ///< 执行前必须获取的约束
        std::vector<Semaphore*> releases; ///< 执行后应当释放的约束

        [[nodiscard]] bool empty() const noexcept {
            return acquires.empty() && releases.empty();
        }
    };

    /// @brief 节点挂载的生命周期观察者集合（按需延迟分配）。
    struct ObserverData {
        std::vector<std::shared_ptr<TaskObserver>> observers;

        [[nodiscard]] bool empty() const noexcept {
            return observers.empty();
        }
    };

    /// @brief 默认构造函数（专供占位符如 NullWork 的中间状态使用）。
    explicit Work() = default;

    /// @brief 静态图内节点构造函数。
    /// @param graph 节点归属的任务物理图指针。
    /// @param options 初始选项配置（内部会自动掩除低 24 位的计数值）。
    explicit Work(Graph* graph, Option::type options) noexcept
        : m_graph{graph}
        , m_options{options & Option::FLAG_MASK} {}

    /// @brief 独立异步任务构造函数。
    /// @param topo 该任务绑定的独立执行拓扑上下文。
    /// @param options 初始选项配置。
    explicit Work(Topology* topo, Option::type options) noexcept
        : m_topology{topo}
        , m_options{options & Option::FLAG_MASK} {}

    virtual ~Work() noexcept = default;

    /// @brief 节点的核心执行调度入口，由底层 Worker 线程驱动。
    /// @param exec 驱动当前拓扑的执行器实例。
    /// @param wr 承载当前执行栈的工作线程。
    /// @param cache 零开销调度暂存器，向调用方输出紧接着应被执行的后继节点。
    virtual void invoke(Executor& exec, Worker& wr, Work*& cache) = 0;

    /// @brief 获取该节点的底层逻辑类型枚举。
    virtual TaskType type() const noexcept = 0;

    /// @brief 生成该节点的可视化 D2 声明代码。
    virtual std::string dump() const = 0;

    /// @brief 流式导出该节点的可视化 D2 声明代码。
    virtual void dump(std::ostream& ostream) const = 0;

private:
    // 冷数据区：调试信息、异常（访问频率低）
    std::string m_name;
    const Graph* m_graph{nullptr};
    std::exception_ptr m_exception_ptr{nullptr};

    // 热数据区：调度核心状态（访问频率高，缓存行友好）
    std::atomic<State::type> m_state{State::NONE};
    Option::type m_options{Option::NONE};
    Topology* m_topology{nullptr};
    Work* m_parent{nullptr};
    std::atomic<std::size_t> m_join_counter{0};  // 依赖计数器，并发递减
    std::size_t m_num_successors{0};            // edges 数组分割点
    std::vector<Work*> m_edges;                 // [后继 | 前驱] 统一存储

    // 扩展区：按需分配（延迟分配优化内存）
    std::unique_ptr<SemaphoreData> m_semaphores;
    std::unique_ptr<ObserverData> m_observers;

    /// @brief 依据节点类型返回其作为前驱时，对后继产生的基础影响权重。
    [[nodiscard]] std::size_t _join_weight() const noexcept {
        switch (type()) {
        case TaskType::Jump:
        case TaskType::MultiJump:
        case TaskType::None:
            return 0;
        case TaskType::Branch:
        case TaskType::MultiBranch:
            return 2;
        default:
            return 1;
        }
    }

    void _set_option(Option::type opt) noexcept { m_options |= (opt & Option::FLAG_MASK); }
    void _clear_option(Option::type opt) noexcept { m_options &= ~(opt & Option::FLAG_MASK); }
    [[nodiscard]] bool _has_option(Option::type opt) const noexcept { return (m_options & opt) & Option::FLAG_MASK; }

    void _set_anchor() noexcept { m_options |= Option::ANCHORED; }
    void _set_preempt() noexcept { m_options |= Option::PREEMPTED; }
    [[nodiscard]] bool _is_anchored() const noexcept { return m_options & Option::ANCHORED; }
    [[nodiscard]] bool _is_preempted() const noexcept { return m_options & Option::PREEMPTED; }

    [[nodiscard]] std::size_t _join_count() const noexcept { return m_options & Option::COUNT_MASK; }
    void _set_join_count(std::size_t c) noexcept {
        m_options = (m_options & Option::FLAG_MASK) | (static_cast<Option::type>(c) & Option::COUNT_MASK);
    }
    void _add_join_count(std::size_t n) noexcept {
        auto cur = m_options & Option::COUNT_MASK;
        TFL_ASSERT(cur + n <= Option::COUNT_MASK && "join count overflow");
        m_options = (m_options & Option::FLAG_MASK) | (static_cast<Option::type>(cur + n) & Option::COUNT_MASK);
    }
    void _sub_join_count(std::size_t n) noexcept {
        auto cur = m_options & Option::COUNT_MASK;
        TFL_ASSERT(cur >= n && "join count underflow");
        m_options = (m_options & Option::FLAG_MASK) | (static_cast<Option::type>(cur - n) & Option::COUNT_MASK);
    }

    void _set_state(State::type s) noexcept { m_state.fetch_or(s, std::memory_order_relaxed); }
    void _clear_state(State::type s) noexcept { m_state.fetch_and(~s, std::memory_order_relaxed); }
    [[nodiscard]] bool _has_state(State::type s) const noexcept { return m_state.load(std::memory_order_relaxed) & s; }
    [[nodiscard]] State::type _load_state() const noexcept { return m_state.load(std::memory_order_relaxed); }
    [[nodiscard]] State::type _fetch_set_state(State::type s) noexcept { return m_state.fetch_or(s, std::memory_order_relaxed); }
    [[nodiscard]] State::type _fetch_clear_state(State::type s) noexcept { return m_state.fetch_and(~s, std::memory_order_relaxed); }

    void _set_exception() noexcept { m_state.fetch_or(State::EXCEPTION, std::memory_order_relaxed); }
    void _set_caught() noexcept { m_state.fetch_or(State::CAUGHT, std::memory_order_relaxed); }
    [[nodiscard]] bool _is_exception() const noexcept { return m_state.load(std::memory_order_relaxed) & State::EXCEPTION; }
    [[nodiscard]] bool _is_caught() const noexcept { return m_state.load(std::memory_order_relaxed) & State::CAUGHT; }

    [[nodiscard]] bool _is_stopped() const noexcept {
        return (m_state.load(std::memory_order_relaxed) & State::EXCEPTION) || m_topology->_is_stopped();
    }

    [[nodiscard]] bool _try_catch_exception() noexcept {
        constexpr auto flags = State::EXCEPTION | State::CAUGHT;
        return (m_state.fetch_or(flags, std::memory_order_relaxed) & State::CAUGHT) == 0;
    }

    void _rethrow_exception() {
        if (m_exception_ptr) {
            auto e = m_exception_ptr;
            m_exception_ptr = nullptr;
            m_state.fetch_and(~(State::EXCEPTION | State::CAUGHT), std::memory_order_relaxed);
            std::rethrow_exception(e);
        }
    }

    [[nodiscard]] std::span<Work*> _successors() noexcept { return {m_edges.data(), m_num_successors}; }
    [[nodiscard]] std::span<Work* const> _successors() const noexcept { return {m_edges.data(), m_num_successors}; }
    [[nodiscard]] std::span<Work*> _predecessors() noexcept { return {m_edges.data() + m_num_successors, m_edges.size() - m_num_successors}; }
    [[nodiscard]] std::span<Work* const> _predecessors() const noexcept { return {m_edges.data() + m_num_successors, m_edges.size() - m_num_successors}; }
    [[nodiscard]] std::size_t _num_predecessors() const noexcept { return m_edges.size() - m_num_successors; }

    [[nodiscard]] SemaphoreData& _ensure_semaphores() {
        if (!m_semaphores) {
            m_semaphores = std::make_unique<SemaphoreData>();
        }
        return *m_semaphores;
    }

    void _try_release_semaphores() noexcept {
        if (m_semaphores && m_semaphores->empty()) {
            m_semaphores.reset();
        }
    }

    [[nodiscard]] std::span<Semaphore*> _acquires() noexcept { return m_semaphores ? std::span<Semaphore*>{m_semaphores->acquires} : std::span<Semaphore*>{}; }
    [[nodiscard]] std::span<Semaphore* const> _acquires() const noexcept { return m_semaphores ? std::span<Semaphore* const>{m_semaphores->acquires} : std::span<Semaphore* const>{}; }
    [[nodiscard]] std::span<Semaphore*> _releases() noexcept { return m_semaphores ? std::span<Semaphore*>{m_semaphores->releases} : std::span<Semaphore*>{}; }
    [[nodiscard]] std::span<Semaphore* const> _releases() const noexcept { return m_semaphores ? std::span<Semaphore* const>{m_semaphores->releases} : std::span<Semaphore* const>{}; }
    [[nodiscard]] std::size_t _num_acquires() const noexcept { return m_semaphores ? m_semaphores->acquires.size() : 0; }
    [[nodiscard]] std::size_t _num_releases() const noexcept { return m_semaphores ? m_semaphores->releases.size() : 0; }

    template <typename T>
    [[nodiscard]] static std::size_t _find_index(std::span<T* const> s, T* target) noexcept {
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == target) return i;
        }
        return static_cast<std::size_t>(-1);
    }

    template <typename T>
    [[nodiscard]] static bool _contains(std::span<T* const> s, T* target) noexcept {
        for (auto* p : s) {
            if (p == target) return true;
        }
        return false;
    }

    void _acquire(Semaphore* sem);
    void _release(Semaphore* sem);
    void _remove_acquire(Semaphore* sem) noexcept;
    void _remove_release(Semaphore* sem) noexcept;
    void _clear_acquires() noexcept;
    void _clear_releases() noexcept;

    template <typename F>
        requires std::invocable<F&, Work*>
    [[nodiscard]] bool _try_acquire_semaphores(F&& on_wake);

    template <typename F>
        requires std::invocable<F&, Work*>
    void _release_semaphores(F&& on_wake);

    void _erase_successor_at(std::size_t idx) noexcept;
    void _erase_predecessor_at(std::size_t idx) noexcept;
    void _precede(Work* target);
    void _remove_successor(Work* target) noexcept;
    void _clear_predecessors() noexcept;
    void _clear_successors() noexcept;

    void _set_up(Work* parent, Topology* t) noexcept;
    void _set_up(std::size_t join_counter) noexcept;
    [[nodiscard]] bool _can_reach(const Work* target) const;
    [[nodiscard]] std::expected<void, std::string_view> _can_precede(Work* target) const;

    [[nodiscard]] static std::string _d2_work(const Work* w,
                                              const char* shape, const char* fill, const char* stroke,
                                              const char* font_color, const char* border_radius,
                                              const char* stroke_dash = "");
    [[nodiscard]] static std::string _d2_escape(const std::string& s);

    static void _d2_work(std::ostream& os, const Work* w,
                         const char* shape, const char* fill, const char* stroke,
                         const char* font_color, const char* border_radius,
                         const char* stroke_dash = "");
    static void _d2_escape(std::ostream& os, const std::string& s);

    template <typename WorkType, typename... Args>
    [[nodiscard]] static Work* _make_topo_work(Executor& exe, Option::type options, Args&&... args);

public:
    // ============================================================================
    //  节点静态工厂族 — Graph 内同步节点
    // ============================================================================

    /// @brief 创建普通顺序推演任务。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    [[nodiscard]] static Work* make_basic(Graph* graph, Option::type options, T&& f, Args&&... args);

    /// @brief 创建单出路互斥分支任务。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && branch_invocable<T, Args...>)
    [[nodiscard]] static Work* make_branch(Graph* graph, Option::type options, T&& f, Args&&... args);

    /// @brief 创建多出路并行分支任务。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && multi_branch_invocable<T, Args...>)
    [[nodiscard]] static Work* make_multi_branch(Graph* graph, Option::type options, T&& f, Args&&... args);

    /// @brief 创建单端特权跳转任务（摧毁常规屏障）。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && jump_invocable<T, Args...>)
    [[nodiscard]] static Work* make_jump(Graph* graph, Option::type options, T&& f, Args&&... args);

    /// @brief 创建多端并行跳转任务。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && multi_jump_invocable<T, Args...>)
    [[nodiscard]] static Work* make_multi_jump(Graph* graph, Option::type options, T&& f, Args&&... args);

    /// @brief 创建基于运行期状态动态扩张的图元任务。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    [[nodiscard]] static Work* make_runtime(Graph* graph, Option::type options, T&& f, Args&&... args);

    /// @brief 创建内嵌静态全图的子流容器节点。
    template <typename F, typename P>
        requires (capturable<P> && flow_type<F> && predicate<P>)
    [[nodiscard]] static Work* make_subflow(Graph* graph, Option::type options, F&& flow, P&& pred);

    // ============================================================================
    //  节点静态工厂族 — 独立异步任务（创建 Topology）
    // ============================================================================

    /// @brief 创建游离于静态图之外、独占生命周期的异步普通任务。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    [[nodiscard]] static Work* make_async_basic(Executor& exe, Option::type options, T&& f, Args&&... args);

    /// @brief 创建游离于静态图之外的异步扩展节点。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    [[nodiscard]] static Work* make_async_runtime(Executor& exe, Option::type options, T&& f, Args&&... args);

    /// @brief 附带 Future/Promise 取值协议的独立异步普通任务。
    template <typename T, typename R, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    [[nodiscard]] static Work* make_async_basic(Executor& exe, Option::type options, T&& f, std::promise<R>&& p, Args&&... args);

    /// @brief 附带 Future/Promise 取值协议的独立异步扩展节点。
    template <typename T, typename R, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    [[nodiscard]] static Work* make_async_runtime(Executor& exe, Option::type options, T&& f, std::promise<R>&& p, Args&&... args);

    // ============================================================================
    //  节点静态工厂族 — 有依赖的异步任务
    // ============================================================================

    /// @brief 可悬挂外部依赖边的高阶独立异步节点。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && basic_invocable<T, Args...>)
    [[nodiscard]] static Work* make_dep_async_basic(Executor& exe, Option::type options, T&& f, Args&&... args);

    /// @brief 可悬挂外部依赖边的高阶异步扩展节点。
    template <typename T, typename... Args>
        requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
    [[nodiscard]] static Work* make_dep_async_runtime(Executor& exe, Option::type options, T&& f, Args&&... args);

    /// @brief 将一整张静态流抛入异步并行宇宙的特殊载体节点。
    template <typename F, typename P, typename C>
        requires (capturable<P, C> && flow_type<F> && predicate<P> && callback<C>)
    [[nodiscard]] static Work* make_dep_flow(Executor& exe, Option::type options, F&& flow, P&& pred, C&& cb);



    static void destroy(Work* work) noexcept;
};

// ============================================================================
//  衍生功能组件定义 (Derived Components)
// ============================================================================

/// @brief 承载 `void()` 标准闭包的基础同步工作元。
template <typename F, typename... Args>
class BasicWork final : public Work {
    [[no_unique_address]] F m_func;
    [[no_unique_address]] std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires std::constructible_from<F, U>
    explicit BasicWork(Graph* g, Option::type opts, U&& f, Us&&... args)
        : Work{g, opts}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor&, Worker&, Work*&) override;

    TaskType type() const noexcept override { return TaskType::Basic; }
    std::string dump() const override { return _d2_work(this, "rectangle", "#f5f5f5", "#9ca3af", "#1f2937", "8"); }
    void dump(std::ostream& os) const override {
        _d2_work(os, this, "rectangle", "#f5f5f5", "#9ca3af", "#1f2937", "8");
    }
};

/// @brief 为闭包注入单一通道挑选权杖 `Branch&` 的条件工作元。
template <typename F, typename... Args>
class BranchWork final : public Work {
    [[no_unique_address]] F m_func;
    [[no_unique_address]] std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires std::constructible_from<F, U>
    explicit BranchWork(Graph* g, Option::type opts, U&& f, Us&&... args)
        : Work{g, opts}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor&, Worker&, Work*&) override;

    TaskType type() const noexcept override { return TaskType::Branch; }
    std::string dump() const override { return _d2_work(this, "diamond", "#dbeafe", "#3b82f6", "#1e3a5f", "8"); }
    void dump(std::ostream& os) const override {
        _d2_work(os, this, "diamond", "#dbeafe", "#3b82f6", "#1e3a5f", "8");
    }
};

/// @brief 为闭包注入多线广播挑选权杖 `MultiBranch&` 的并行激活工作元。
template <typename F, typename... Args>
class MultiBranchWork final : public Work {
    [[no_unique_address]] F m_func;
    [[no_unique_address]] std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires std::constructible_from<F, U>
    explicit MultiBranchWork(Graph* g, Option::type opts, U&& f, Us&&... args)
        : Work{g, opts}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor&, Worker&, Work*&) override;

    TaskType type() const noexcept override { return TaskType::MultiBranch; }
    std::string dump() const override { return _d2_work(this, "hexagon", "#bfdbfe", "#2563eb", "#1e3a5f", "8"); }
    void dump(std::ostream& os) const override {
        _d2_work(os, this, "hexagon", "#bfdbfe", "#2563eb", "#1e3a5f", "8");
    }
};

/// @brief 授权在图内执行强制物理传送跃迁指令 `Jump&` 的中断工作元。
template <typename F, typename... Args>
class JumpWork final : public Work {
    [[no_unique_address]] F m_func;
    [[no_unique_address]] std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires std::constructible_from<F, U>
    explicit JumpWork(Graph* g, Option::type opts, U&& f, Us&&... args)
        : Work{g, opts}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor&, Worker&, Work*&) override;

    TaskType type() const noexcept override { return TaskType::Jump; }
    std::string dump() const override { return _d2_work(this, "diamond", "#fee2e2", "#ef4444", "#7f1d1d", "8", "5"); }
    void dump(std::ostream& os) const override {
        _d2_work(os, this, "diamond", "#fee2e2", "#ef4444", "#7f1d1d", "8", "5");
    }
};

/// @brief 授权同时摧毁并重置多个下游依赖计数网 `MultiJump&` 的广播跃迁元。
template <typename F, typename... Args>
class MultiJumpWork final : public Work {
    [[no_unique_address]] F m_func;
    [[no_unique_address]] std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires std::constructible_from<F, U>
    explicit MultiJumpWork(Graph* g, Option::type opts, U&& f, Us&&... args)
        : Work{g, opts}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor&, Worker&, Work*&) override;

    TaskType type() const noexcept override { return TaskType::MultiJump; }
    std::string dump() const override { return _d2_work(this, "hexagon", "#fecaca", "#dc2626", "#7f1d1d", "8", "5"); }
    void dump(std::ostream& os) const override {
        _d2_work(os, this, "hexagon", "#fecaca", "#dc2626", "#7f1d1d", "8", "5");
    }
};

/// @brief 提供 `Runtime&` 接口，赋予节点在执行期实时操纵所在线程栈与编排图谱的能力。
template <typename F, typename... Args>
class RuntimeWork final : public Work {
    [[no_unique_address]] F m_func;
    [[no_unique_address]] std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires std::constructible_from<F, U>
    explicit RuntimeWork(Graph* g, Option::type opts, U&& f, Us&&... args)
        : Work{g, opts}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor&, Worker&, Work*&) override;

    TaskType type() const noexcept override { return TaskType::Runtime; }
    std::string dump() const override { return _d2_work(this, "rectangle", "#fce4ec", "#e57373", "#6d1b1b", "30"); }
    void dump(std::ostream& os) const override {
        _d2_work(os, this, "rectangle", "#fce4ec", "#e57373", "#6d1b1b", "30");
    }
};

/// @brief 通过持有 `Flow` 图的物理存储权，实现节点内部的深度分形展开与调度收敛。
template <typename FlowStore, typename P>
class SubflowWork final : public Work {
    FlowStore m_flow_store;
    [[no_unique_address]] P m_pred;
    bool m_started{false};
public:
    template <typename U, typename V>
    explicit SubflowWork(Graph* g, Option::type opts, U&& flow_store, V&& pred)
        : Work(g, opts)
        , m_flow_store(std::forward<U>(flow_store))
        , m_pred(std::forward<V>(pred)) {}

    void invoke(Executor&, Worker&, Work*&) override;

    TaskType type() const noexcept override { return TaskType::Graph; }
    std::string dump() const override {
        decltype(auto) flow = detail::unwrap(m_flow_store);

        if (flow.m_graph.empty()) {
            return _d2_work(this, "rectangle", "#e8f5e9", "#10b981", "#065f46", "8");
        }

        char id[24];
        const char* type_name = to_string(type());
        std::snprintf(id, sizeof(id), "p%zx", reinterpret_cast<std::uintptr_t>(this));
        const auto& name = this->m_name.empty() ? std::string(id) : this->m_name;

        std::string out;
        out += id;
        out += ": |md\n  <center>";
        out += _d2_escape(name);

        out += "<br/><span style=\"color: #6b7280;\">[ ";
        out += type_name;
        out += " ]</span></center>\n| {\n";

        out += "  shape: rectangle\n";
        out += "  label.near: top-center\n";
        out += "  style.fill: \"#e8f5e9\"\n";
        out += "  style.stroke: \"#10b981\"\n";
        out += "  style.stroke-width: 2\n";
        out += "  style.border-radius: 14\n\n";
        out += flow.m_graph._dump();
        out += "}";
        return out;
    }

    void dump(std::ostream& os) const override {
        decltype(auto) flow = detail::unwrap(m_flow_store);

        if (flow.m_graph.empty()) {
            _d2_work(os, this, "rectangle", "#e8f5e9", "#10b981", "#065f46", "8");
            return;
        }

        char id[24];
        const char* type_name = to_string(type());
        std::snprintf(id, sizeof(id), "p%zx", reinterpret_cast<std::uintptr_t>(this));
        const auto& name = this->m_name.empty() ? std::string(id) : this->m_name;

        os << id << ": |md\n  <center>";
        _d2_escape(os, name);
        os << "<br/><span style=\"color: #6b7280;\">[ "
           << type_name
           << " ]</span></center>\n| {\n";
        os << "  shape: rectangle\n";
        os << "  label.near: top-center\n";
        os << "  style.fill: \"#e8f5e9\"\n";
        os << "  style.stroke: \"#10b981\"\n";
        os << "  style.stroke-width: 2\n";
        os << "  style.border-radius: 14\n\n";
        flow.m_graph._dump(os);
        os << "}";
    }
};

/// @brief 与独立 Topology 锁死的顶级基础异步任务，触发后自我燃烧并销毁。
template <typename F, typename... Args>
class AsyncBasicWork final : public Work {
    [[no_unique_address]] F m_func;
    [[no_unique_address]] std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires std::constructible_from<F, U>
    explicit AsyncBasicWork(Topology* t, Option::type opts, U&& f, Us&&... args)
        : Work{t, opts}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor&, Worker&, Work*&) override;

    TaskType type() const noexcept override { return TaskType::Basic; }
    std::string dump() const override { return _d2_work(this, "rectangle", "#f5f5f5", "#9ca3af", "#1f2937", "8"); }
    void dump(std::ostream& os) const override {
        _d2_work(os, this, "rectangle", "#f5f5f5", "#9ca3af", "#1f2937", "8");
    }
};

/// @brief 与独立 Topology 锁死的顶级扩展异步任务。
template <typename F, typename... Args>
class AsyncRuntimeWork final : public Work {
    [[no_unique_address]] F m_func;
    [[no_unique_address]] std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires std::constructible_from<F, U>
    explicit AsyncRuntimeWork(Topology* t, Option::type opts, U&& f, Us&&... args)
        : Work{t, opts}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor&, Worker&, Work*&) override;

    TaskType type() const noexcept override { return TaskType::Runtime; }
    std::string dump() const override { return _d2_work(this, "rectangle", "#fce4ec", "#e57373", "#6d1b1b", "30"); }
    void dump(std::ostream& os) const override {
        _d2_work(os, this, "rectangle", "#fce4ec", "#e57373", "#6d1b1b", "30");
    }
};

/// @brief 内部熔接了 promise 的 Promise 同步通道基础异步任务。
template <typename F, typename R, typename... Args>
class AsyncBasicPromiseWork final : public Work {
    [[no_unique_address]] F m_func;
    [[no_unique_address]] std::tuple<Args...> m_args;
    std::promise<R> m_promise;
public:
    template <typename U, typename... Us>
        requires std::constructible_from<F, U>
    explicit AsyncBasicPromiseWork(Topology* t, Option::type opts, U&& f, std::promise<R>&& p, Us&&... args)
        : Work{t, opts}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...}
        , m_promise{std::move(p)} {}

    void invoke(Executor&, Worker&, Work*&) override;

    TaskType type() const noexcept override { return TaskType::Basic; }
    std::string dump() const override { return _d2_work(this, "rectangle", "#f5f5f5", "#9ca3af", "#1f2937", "8"); }
    void dump(std::ostream& os) const override {
        _d2_work(os, this, "rectangle", "#f5f5f5", "#9ca3af", "#1f2937", "8");
    }
};

/// @brief 内部熔接了 promise 的 Promise 同步通道扩展异步任务。
template <typename F, typename R, typename... Args>
class AsyncRuntimePromiseWork final : public Work {
    [[no_unique_address]] F m_func;
    [[no_unique_address]] std::tuple<Args...> m_args;
    std::promise<R> m_promise;
public:
    template <typename U, typename... Us>
        requires std::constructible_from<F, U>
    explicit AsyncRuntimePromiseWork(Topology* t, Option::type opts, U&& f, std::promise<R>&& p, Us&&... args)
        : Work{t, opts}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...}
        , m_promise{std::move(p)} {}

    void invoke(Executor&, Worker&, Work*&) override;

    TaskType type() const noexcept override { return TaskType::Runtime; }
    std::string dump() const override { return _d2_work(this, "rectangle", "#fce4ec", "#e57373", "#6d1b1b", "30"); }
    void dump(std::ostream& os) const override {
        _d2_work(os, this, "rectangle", "#fce4ec", "#e57373", "#6d1b1b", "30");
    }
};

/// @brief 被高阶外部拓扑锁定的依赖型常规任务，需完成 CAS 抢占才能挂载入依赖树。
template <typename F, typename... Args>
class DepAsyncBasicWork final : public Work {
    [[no_unique_address]] F m_func;
    [[no_unique_address]] std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires std::constructible_from<F, U>
    explicit DepAsyncBasicWork(Topology* t, Option::type opts, U&& f, Us&&... args)
        : Work{t, opts}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor&, Worker&, Work*&) override;

    TaskType type() const noexcept override { return TaskType::Basic; }
    std::string dump() const override { return _d2_work(this, "rectangle", "#f5f5f5", "#9ca3af", "#1f2937", "8"); }
    void dump(std::ostream& os) const override {
        _d2_work(os, this, "rectangle", "#f5f5f5", "#9ca3af", "#1f2937", "8");
    }
};

/// @brief 被高阶外部拓扑锁定的依赖型动态挂载任务。
template <typename F, typename... Args>
class DepAsyncRuntimeWork final : public Work {
    [[no_unique_address]] F m_func;
    [[no_unique_address]] std::tuple<Args...> m_args;
public:
    template <typename U, typename... Us>
        requires std::constructible_from<F, U>
    explicit DepAsyncRuntimeWork(Topology* t, Option::type opts, U&& f, Us&&... args)
        : Work{t, opts}
        , m_func{std::forward<U>(f)}
        , m_args{std::forward<Us>(args)...} {}

    void invoke(Executor&, Worker&, Work*&) override;

    TaskType type() const noexcept override { return TaskType::Runtime; }
    std::string dump() const override { return _d2_work(this, "rectangle", "#fce4ec", "#e57373", "#6d1b1b", "30"); }
    void dump(std::ostream& os) const override {
        _d2_work(os, this, "rectangle", "#fce4ec", "#e57373", "#6d1b1b", "30");
    }
};

/// @brief 能够串接任意 Flow 实体的巨无霸容器节点，并在生命周期落幕时点燃专有回调。
template <typename FlowStore, typename P, typename C>
class DepFlowWork final : public Work {
    FlowStore m_flow_store;
    [[no_unique_address]] P m_pred;
    [[no_unique_address]] C m_callback;
    bool m_started{false};
public:
    template <typename U, typename V, typename W>
    explicit DepFlowWork(Topology* t, Option::type opts, U&& flow_store, V&& pred, W&& cb)
        : Work(t, opts)
        , m_flow_store(std::forward<U>(flow_store))
        , m_pred(std::forward<V>(pred))
        , m_callback(std::forward<W>(cb)) {}

    void invoke(Executor&, Worker&, Work*&) override;

    TaskType type() const noexcept override { return TaskType::Graph; }
    std::string dump() const override {
        char id[24];
        std::snprintf(id, sizeof(id), "p%zx", reinterpret_cast<std::uintptr_t>(this));

        decltype(auto) flow = detail::unwrap(m_flow_store);

        if (flow.m_graph.empty()) {
            return _d2_work(this, "rectangle", "#e8f5e9", "#10b981", "#065f46", "8");
        }

        const char* type_name = to_string(type());
        const auto& name = this->m_name.empty() ? std::string(id) : this->m_name;

        std::string out;
        out += id;
        out += ": |md\n  <center>";
        out += _d2_escape(name);

        out += "<br/><span style=\"color: #6b7280;\">[ ";
        out += type_name;
        out += " ]</span></center>\n| {\n";

        out += "  shape: rectangle\n";
        out += "  label.near: top-center\n";
        out += "  style.fill: \"#e8f5e9\"\n";
        out += "  style.stroke: \"#10b981\"\n";
        out += "  style.stroke-width: 2\n";
        out += "  style.border-radius: 14\n\n";
        out += flow.m_graph._dump();
        out += "}";
        return out;
    }

    void dump(std::ostream& os) const override {
        decltype(auto) flow = detail::unwrap(m_flow_store);

        if (flow.m_graph.empty()) {
            _d2_work(os, this, "rectangle", "#e8f5e9", "#10b981", "#065f46", "8");
            return;
        }

        char id[24];
        const char* type_name = to_string(type());
        std::snprintf(id, sizeof(id), "p%zx", reinterpret_cast<std::uintptr_t>(this));
        const auto& name = this->m_name.empty() ? std::string(id) : this->m_name;

        os << id << ": |md\n  <center>";
        _d2_escape(os, name);
        os << "<br/><span style=\"color: #6b7280;\">[ "
           << type_name
           << " ]</span></center>\n| {\n";
        os << "  shape: rectangle\n";
        os << "  label.near: top-center\n";
        os << "  style.fill: \"#e8f5e9\"\n";
        os << "  style.stroke: \"#10b981\"\n";
        os << "  style.stroke-width: 2\n";
        os << "  style.border-radius: 14\n\n";
        flow.m_graph._dump(os);
        os << "}";
    }
};

/// @brief 虚拟锚点元。在图解析和异步链排布中充当零损耗的汇聚地。
class NullWork final : public Work {
public:
    explicit NullWork(Topology* t, Option::type opts)
        : Work(t, opts) {}

    void invoke(Executor&, Worker&, Work*&) override {}

    TaskType type() const noexcept override { return TaskType::None; }
    std::string dump() const override { return _d2_work(this, "circle", "#f5f5f5", "#bdbdbd", "#9e9e9e", "8"); }
    void dump(std::ostream& os) const override {
        _d2_work(os, this, "circle", "#f5f5f5", "#bdbdbd", "#9e9e9e", "8");
    }
};

// ============================================================================
//  内联实现 — Graph 内同步节点工厂
// ============================================================================

template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
Work* Work::make_basic(Graph* graph, Option::type options, T&& f, Args&&... args) {
    return new BasicWork<std::decay_t<T>, std::decay_t<Args>...>(
        graph, options, std::forward<T>(f), std::forward<Args>(args)...);
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && branch_invocable<T, Args...>)
Work* Work::make_branch(Graph* graph, Option::type options, T&& f, Args&&... args) {
    return new BranchWork<std::decay_t<T>, std::decay_t<Args>...>(
        graph, options, std::forward<T>(f), std::forward<Args>(args)...);
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && multi_branch_invocable<T, Args...>)
Work* Work::make_multi_branch(Graph* graph, Option::type options, T&& f, Args&&... args) {
    return new MultiBranchWork<std::decay_t<T>, std::decay_t<Args>...>(
        graph, options, std::forward<T>(f), std::forward<Args>(args)...);
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && jump_invocable<T, Args...>)
Work* Work::make_jump(Graph* graph, Option::type options, T&& f, Args&&... args) {
    return new JumpWork<std::decay_t<T>, std::decay_t<Args>...>(
        graph, options, std::forward<T>(f), std::forward<Args>(args)...);
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && multi_jump_invocable<T, Args...>)
Work* Work::make_multi_jump(Graph* graph, Option::type options, T&& f, Args&&... args) {
    return new MultiJumpWork<std::decay_t<T>, std::decay_t<Args>...>(
        graph, options, std::forward<T>(f), std::forward<Args>(args)...);
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
Work* Work::make_runtime(Graph* graph, Option::type options, T&& f, Args&&... args) {
    return new RuntimeWork<std::decay_t<T>, std::decay_t<Args>...>(
        graph, options, std::forward<T>(f), std::forward<Args>(args)...);
}

template <typename F, typename P>
    requires (capturable<P> && flow_type<F> && predicate<P>)
Work* Work::make_subflow(Graph* graph, Option::type options, F&& flow, P&& pred) {
    return new SubflowWork<detail::wrap_t<F>, std::decay_t<P>>(
        graph, options,
        detail::wrap(std::forward<F>(flow)),
        std::forward<P>(pred));
}

// ============================================================================
//  内联实现 — 独立异步任务工厂
// ============================================================================

template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
Work* Work::make_async_basic(Executor& exe, Option::type options, T&& f, Args&&... args) {
    return _make_topo_work<AsyncBasicWork<std::decay_t<T>, std::decay_t<Args>...>>(
        exe, options, std::forward<T>(f), std::forward<Args>(args)...);
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
Work* Work::make_async_runtime(Executor& exe, Option::type options, T&& f, Args&&... args) {
    return _make_topo_work<AsyncRuntimeWork<std::decay_t<T>, std::decay_t<Args>...>>(
        exe, options, std::forward<T>(f), std::forward<Args>(args)...);
}

template <typename T, typename R, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
Work* Work::make_async_basic(Executor& exe, Option::type options, T&& f, std::promise<R>&& p, Args&&... args) {
    return _make_topo_work<AsyncBasicPromiseWork<std::decay_t<T>, R, std::decay_t<Args>...>>(
        exe, options, std::forward<T>(f), std::move(p), std::forward<Args>(args)...);
}

template <typename T, typename R, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
Work* Work::make_async_runtime(Executor& exe, Option::type options, T&& f, std::promise<R>&& p, Args&&... args) {
    return _make_topo_work<AsyncRuntimePromiseWork<std::decay_t<T>, R, std::decay_t<Args>...>>(
        exe, options, std::forward<T>(f), std::move(p), std::forward<Args>(args)...);
}

// ============================================================================
//  内联实现 — 有依赖的异步任务工厂
// ============================================================================

template <typename T, typename... Args>
    requires (capturable<T, Args...> && basic_invocable<T, Args...>)
Work* Work::make_dep_async_basic(Executor& exe, Option::type options, T&& f, Args&&... args) {
    return _make_topo_work<DepAsyncBasicWork<std::decay_t<T>, std::decay_t<Args>...>>(
        exe, options, std::forward<T>(f), std::forward<Args>(args)...);
}

template <typename T, typename... Args>
    requires (capturable<T, Args...> && runtime_invocable<T, Args...>)
Work* Work::make_dep_async_runtime(Executor& exe, Option::type options, T&& f, Args&&... args) {
    return _make_topo_work<DepAsyncRuntimeWork<std::decay_t<T>, std::decay_t<Args>...>>(
        exe, options, std::forward<T>(f), std::forward<Args>(args)...);
}

template <typename F, typename P, typename C>
    requires (capturable<P, C> && flow_type<F> && predicate<P> && callback<C>)
Work* Work::make_dep_flow(Executor& exe, Option::type options, F&& flow, P&& pred, C&& cb) {
    return _make_topo_work<DepFlowWork<detail::wrap_t<F>, std::decay_t<P>, std::decay_t<C>>>(
        exe, options,
        detail::wrap(std::forward<F>(flow)),
        std::forward<P>(pred),
        std::forward<C>(cb));
}

inline void Work::destroy(Work* work) noexcept {
    // Why: 统一处理关联 Topology 的双向解绑并实施资源销毁。
    // 若这是独立下发的异步任务节点，则需同步将其宿主 Topology 伴随释放，避免堆内存孤岛。
    if (!work) {
        return;
    }

    Topology* topo = work->m_topology;
    if (topo && topo->m_work == work) {
        delete work;
        delete topo;
    } else {
        delete work;
    }
}

inline void Work::_set_up(Work* const parent, Topology* const t) noexcept {
    // Why: 执行前的彻底清洗。清除因上一轮异常残留的标识痕迹，
    // 并将初始静态阈值（预设好的前驱依赖权重）下发至工作区的并发原子倒数器，为本轮推进建立壁垒。
    m_parent = parent;
    m_topology = t;
    m_exception_ptr = nullptr;
    m_state.store(State::NONE, std::memory_order_relaxed);
    m_join_counter.store(_join_count(), std::memory_order_relaxed);
}

inline void Work::_set_up(const std::size_t join_counter) noexcept {
    m_exception_ptr = nullptr;
    m_state.store(State::NONE, std::memory_order_relaxed);
    m_join_counter.store(join_counter, std::memory_order_relaxed);
}


inline void Work::_erase_successor_at(std::size_t idx) noexcept {
    TFL_ASSERT(idx < m_num_successors);
    // Why: 不动如山。抛弃传统 vector 擦除动作带来的尾流数据 O(N) 挪步大腾挪。
    // 使用游标边缘的末尾元素进行 Swap 覆盖，实现绝对冷酷的 O(1) 斩杀线清理（副作用是丧失遍历保序性，但这在 DAG 中无关紧要）。
    const std::size_t last_succ = m_num_successors - 1;
    const std::size_t num_preds = _num_predecessors();

    if (idx != last_succ) {
        m_edges[idx] = m_edges[last_succ];
    }
    if (num_preds > 0) {
        m_edges[last_succ] = m_edges.back();
    }
    m_edges.pop_back();
    --m_num_successors;
}

inline void Work::_erase_predecessor_at(std::size_t idx) noexcept {
    TFL_ASSERT(idx < _num_predecessors());
    const std::size_t abs_idx = m_num_successors + idx;
    m_edges[abs_idx] = m_edges.back();
    m_edges.pop_back();
}

inline void Work::_precede(Work* const target) {
    if (auto result = _can_precede(target); !result) {
        throw Exception("cannot precede: {}.", result.error());
    }

    // Why: 构造期单向压入并相互引注。
    // 在这同步的拓扑描绘阶段，将即将建立的前后级从属关系写入目标节点的前端负重配额（join_count）中。
    // 这是将运行期判断成本彻底转嫁到构建期的核心秘诀。
    m_edges.push_back(target);
    if (m_num_successors < m_edges.size() - 1) {
        std::swap(m_edges[m_num_successors], m_edges.back());
    }
    ++m_num_successors;
    target->m_edges.push_back(this);

    if (std::size_t w = _join_weight(); w > 0) {
        target->_add_join_count(w);
    }
}

inline void Work::_remove_successor(Work* const target) noexcept {
    if (!target) return;

    const std::size_t idx = _find_index<Work>(_successors(), target);
    if (idx == static_cast<std::size_t>(-1)) return;

    _erase_successor_at(idx);

    const std::size_t pidx = _find_index<Work>(target->_predecessors(), this);
    TFL_ASSERT(pidx != static_cast<std::size_t>(-1) && "predecessor must exist");
    target->_erase_predecessor_at(pidx);

    if (std::size_t w = _join_weight(); w > 0) {
        target->_sub_join_count(w);
    }
}

inline void Work::_clear_predecessors() noexcept {
    for (Work* pred : _predecessors()) {
        const std::size_t idx = _find_index<Work>(pred->_successors(), this);
        TFL_ASSERT(idx != static_cast<std::size_t>(-1) && "successor must exist");
        pred->_erase_successor_at(idx);
    }

    _set_join_count(0);
    while (m_edges.size() > m_num_successors) {
        m_edges.pop_back();
    }
}

inline void Work::_clear_successors() noexcept {
    if (std::size_t w = _join_weight(); w > 0) {
        for (Work* succ : _successors()) {
            succ->_sub_join_count(w);
            const std::size_t idx = _find_index<Work>(succ->_predecessors(), this);
            TFL_ASSERT(idx != static_cast<std::size_t>(-1) && "predecessor must exist");
            succ->_erase_predecessor_at(idx);
        }
    } else {
        for (Work* succ : _successors()) {
            const std::size_t idx = _find_index<Work>(succ->_predecessors(), this);
            TFL_ASSERT(idx != static_cast<std::size_t>(-1) && "predecessor must exist");
            succ->_erase_predecessor_at(idx);
        }
    }

    const std::size_t num_preds = _num_predecessors();
    for (std::size_t i = 0; i < num_preds; ++i) {
        m_edges[i] = m_edges[m_num_successors + i];
    }
    while (m_edges.size() > num_preds) {
        m_edges.pop_back();
    }
    m_num_successors = 0;
}

inline std::expected<void, std::string_view> Work::_can_precede(Work* const target) const {
    if (!target) return std::unexpected{"target is null"};
    if (target == this) return std::unexpected{"self-loop detected"};
    if (!m_graph) return std::unexpected{"work not attached to graph"};
    if (m_graph != target->m_graph) return std::unexpected{"works belong to different graphs"};
    if (_contains<Work>(_successors(), target)) return std::unexpected{"edge already exists"};
    return {};
}

inline bool Work::_can_reach(const Work* target) const {
    if (!target) return false;
    if (target == this) return true;

    std::vector<const Work*> queue;
    std::unordered_set<const Work*> visited;
    queue.reserve(_successors().size());

    for (const auto* succ : _successors()) {
        if (succ == target) return true;
        queue.push_back(succ);
        visited.insert(succ);
    }

    for (std::size_t i = 0; i < queue.size(); ++i) {
        for (const auto* succ : queue[i]->_successors()) {
            if (succ == target) return true;
            if (visited.insert(succ).second) {
                queue.push_back(succ);
            }
        }
    }
    return false;
}

inline void Work::_acquire(Semaphore* sem) {
    if (!sem) throw Exception("cannot acquire null semaphore.");
    auto& sd = _ensure_semaphores();
    if (_contains<Semaphore>(std::span<Semaphore* const>{sd.acquires}, sem))
        throw Exception("semaphore already in acquire list.");
    sd.acquires.push_back(sem);
}

inline void Work::_release(Semaphore* sem) {
    if (!sem) throw Exception("cannot release null semaphore.");
    auto& sd = _ensure_semaphores();
    if (_contains<Semaphore>(std::span<Semaphore* const>{sd.releases}, sem))
        throw Exception("semaphore already in release list.");
    sd.releases.push_back(sem);
}

inline void Work::_remove_acquire(Semaphore* sem) noexcept {
    if (!m_semaphores) return;
    auto& acqs = m_semaphores->acquires;
    const std::size_t idx = _find_index<Semaphore>(std::span<Semaphore* const>{acqs}, sem);
    if (idx == static_cast<std::size_t>(-1)) return;

    acqs[idx] = acqs.back();
    acqs.pop_back();
    _try_release_semaphores();
}

inline void Work::_remove_release(Semaphore* sem) noexcept {
    if (!m_semaphores) return;
    auto& rels = m_semaphores->releases;
    const std::size_t idx = _find_index<Semaphore>(std::span<Semaphore* const>{rels}, sem);
    if (idx == static_cast<std::size_t>(-1)) return;

    rels[idx] = rels.back();
    rels.pop_back();
    _try_release_semaphores();
}

inline void Work::_clear_acquires() noexcept {
    if (!m_semaphores) return;
    m_semaphores->acquires.clear();
    _try_release_semaphores();
}

inline void Work::_clear_releases() noexcept {
    if (!m_semaphores) return;
    m_semaphores->releases.clear();
    _try_release_semaphores();
}


template <typename F>
    requires std::invocable<F&, Work*>
inline bool Work::_try_acquire_semaphores(F&& on_wake) {
    if (!m_semaphores) return true;
    auto& acqs = m_semaphores->acquires;
    for (std::size_t i = 0; i < acqs.size(); ++i) {
        if (!acqs[i]->_try_acquire(this)) {
            // Why: 如果任务在连续索要多重配额的半途中受挫，必须对前方所有已取得的名额执行撤销与无条件偿还。
            // 这种主动的自我回滚（Rollback）是彻底打破“循环等待”以消灭死锁发生可能性的终极防线。
            for (std::size_t j = i; j > 0; --j) {
                acqs[j - 1]->_release(on_wake);
            }
            return false;
        }
    }
    return true;
}

template <typename F>
    requires std::invocable<F&, Work*>
inline void Work::_release_semaphores(F&& on_wake) {
    if (!m_semaphores) return;
    for (auto* sem : m_semaphores->releases) {
        sem->_release(on_wake);
    }
}

std::string Work::_d2_work(const Work* w,
                           const char* shape, const char* fill, const char* stroke,
                           const char* font_color, const char* border_radius,
                           const char* stroke_dash)
{
    char id[24];
    std::snprintf(id, sizeof(id), "p%zx", reinterpret_cast<std::uintptr_t>(w));

    const char* type_name = to_string(w->type());
    const auto& name = w->m_name.empty() ? std::string(id) : w->m_name;

    std::string out;
    out += id;
    out += ": |md\n  <center>";
    out += _d2_escape(name);
    out += "<br/><span style=\"color: #6b7280;\">[ ";
    out += type_name;
    out += " ]</span></center>\n| {\n";
    out += "  shape: ";
    out += shape;
    out += "\n";
    out += "  style.fill: \"";
    out += fill;
    out += "\"\n";
    out += "  style.stroke: \"";
    out += stroke;
    out += "\"\n";
    out += "  style.font-color: \"";
    out += font_color;
    out += "\"\n";
    out += "  style.border-radius: ";
    out += border_radius;
    out += "\n";

    if (stroke_dash && stroke_dash[0] != '\0') {
        out += "  style.stroke-dash: ";
        out += stroke_dash;
        out += "\n";
    }

    out += "  style.font-size: 14\n";
    out += "}";
    return out;
}

std::string Work::_d2_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '"':  out += "&quot;"; break;
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '\\': out += "\\\\";   break;
        default:   out += c;
        }
    }
    return out;
}

void Work::_d2_work(std::ostream& os, const Work* w,
                    const char* shape, const char* fill, const char* stroke,
                    const char* font_color, const char* border_radius,
                    const char* stroke_dash)
{
    char id[24];
    std::snprintf(id, sizeof(id), "p%zx", reinterpret_cast<std::uintptr_t>(w));

    const char* type_name = to_string(w->type());
    const auto& name = w->m_name.empty() ? std::string(id) : w->m_name;

    os << id << ": |md\n  <center>";
    _d2_escape(os, name);
    os << "<br/><span style=\"color: #6b7280;\">[ "
       << type_name
       << " ]</span></center>\n| {\n";
    os << "  shape: " << shape << "\n";
    os << "  style.fill: \"" << fill << "\"\n";
    os << "  style.stroke: \"" << stroke << "\"\n";
    os << "  style.font-color: \"" << font_color << "\"\n";
    os << "  style.border-radius: " << border_radius << "\n";

    if (stroke_dash && stroke_dash[0] != '\0') {
        os << "  style.stroke-dash: " << stroke_dash << "\n";
    }

    os << "  style.font-size: 14\n";
    os << "}";
}

void Work::_d2_escape(std::ostream& os, const std::string& s) {
    for (char c : s) {
        switch (c) {
        case '"':  os << "&quot;"; break;
        case '&':  os << "&amp;";  break;
        case '<':  os << "&lt;";   break;
        case '>':  os << "&gt;";   break;
        case '\\': os << "\\\\";   break;
        default:   os << c;
        }
    }
}

template <typename WorkType, typename... Args>
[[nodiscard]] Work* Work::_make_topo_work(Executor& exe, Option::type options, Args&&... args) {
    auto* topo = new Topology(exe);
    auto* w = new WorkType(topo, options, std::forward<Args>(args)...);
    topo->m_work = w;
    return w;
}

}  // namespace tfl
