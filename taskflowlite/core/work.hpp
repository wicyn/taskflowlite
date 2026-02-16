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

/**
@private
*/
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
public:
    // ========================================================================
    // 静态选项（构建时确定，执行时只读）
    // ========================================================================
    struct Option {
        using type = std::uint32_t;

        static constexpr unsigned BITS       = sizeof(type) * 8;   // 32
        static constexpr unsigned FLAG_BITS  = 8;
        static constexpr unsigned COUNT_BITS = BITS - FLAG_BITS;   // 24

        static constexpr type COUNT_MASK = (type{1} << COUNT_BITS) - 1;  // 0x00FFFFFF
        static constexpr type FLAG_MASK  = ~COUNT_MASK;                   // 0xFF000000

        static constexpr type NONE      = 0;
        static constexpr type ANCHORED  = type{1} << (BITS - 1); // 异常捕获锚点
        static constexpr type PREEMPTED = type{1} << (BITS - 2); // 可抢占
    };
protected:

    // ========================================================================
    // 运行时状态（执行时并发修改）
    // ========================================================================
    struct State {
        using type = std::uint32_t;

        static constexpr unsigned BITS       = sizeof(type) * 8;   // 32

        static constexpr type NONE      = 0;
        static constexpr type EXCEPTION = type{1} << (BITS - 1);  // 发生异常
        static constexpr type CAUGHT    = type{1} << (BITS - 2);  // 异常已捕获
    };

    // ========================================================================
    // 按需分配的附属数据
    // ========================================================================
    struct SemaphoreData {
        std::vector<Semaphore*> acquires;
        std::vector<Semaphore*> releases;

        [[nodiscard]] bool empty() const noexcept {
            return acquires.empty() && releases.empty();
        }
    };

    struct ObserverData {
        std::vector<std::shared_ptr<TaskObserver>> observers;

        [[nodiscard]] bool empty() const noexcept {
            return observers.empty();
        }
    };

    // ============================================================
    // 构造 / 析构
    // ============================================================

    explicit Work() = default;

    explicit Work(Graph* graph, TaskType type, Option::type options) noexcept
        : m_graph{graph}
        , m_type{type}
        , m_options{options & Option::FLAG_MASK} {}

    explicit Work(Topology* topo, TaskType type, Option::type options) noexcept
        : m_topology{topo}
        , m_type{type}
        , m_options{options & Option::FLAG_MASK} {}

    virtual ~Work() noexcept = default;

    virtual void invoke(Executor& exec, Worker& wr, Work*& cache) = 0;

private:

    // ================================================================
    // 冷数据：构建期、调试、异常时才访问
    // ================================================================
    std::string m_name;                                          // 32
    const TaskType m_type{TaskType::None};                       // 4
    const Graph* m_graph{nullptr};                               // 8
    std::exception_ptr m_exception_ptr{nullptr};                 // 16

    // ================================================================
    // 热数据：按执行时序从远到近排列
    // ================================================================
    std::atomic<State::type> m_state{State::NONE};               // 4   ①
    Option::type m_options{Option::NONE};                        // 4   ③⑤
    Topology* m_topology{nullptr};                               // 8   ②
    Work* m_parent{nullptr};                                     // 8   ⑨
    std::atomic<std::size_t> m_join_counter{0};                  // 8   ⑥
    std::size_t m_num_successors{0};                             // 8   ⑦
    std::vector<Work*> m_edges;                                  // 24  ⑧
    std::unique_ptr<SemaphoreData> m_semaphores;                 // 8   nullptr = 无信号量
    std::unique_ptr<ObserverData> m_observers;                   // 8   nullptr = 无观察者
        // 小计 = 80
    // ---- WorkImpl<F>::m_func 紧跟其后 ----

    // ============================================================================
    // join weight：当前节点作为前驱时，对后继 join_counter 的贡献值
    //
    //   普通任务:        1  (完成时递减一次)
    //   Branch/Multi:   2  (完成递减一次 + allow 递减一次)
    //   Jump/Multi:     0  (绕过 counter 直接调度)
    //   None:           0  (占位符，无实际执行)
    // ============================================================================
    [[nodiscard]] std::size_t _join_weight() const noexcept {
        switch (m_type) {
        case TaskType::Jump:
        case TaskType::None:
            return 0;
        case TaskType::Branch:
            return 2;
        default:
            return 1;
        }
    }

    // ============================================================================
    // Option 操作（非原子，仅构建阶段调用）
    // ============================================================================
    void _set_option(Option::type opt) noexcept {
        m_options |= (opt & Option::FLAG_MASK);
    }
    void _clear_option(Option::type opt) noexcept {
        m_options &= ~(opt & Option::FLAG_MASK);
    }
    [[nodiscard]] bool _has_option(Option::type opt) const noexcept {
        return (m_options & opt) & Option::FLAG_MASK;
    }

    void _set_anchor() noexcept { m_options |= Option::ANCHORED; }
    void _set_preempt() noexcept { m_options |= Option::PREEMPTED; }

    [[nodiscard]] bool _is_anchored() const noexcept { return m_options & Option::ANCHORED; }
    [[nodiscard]] bool _is_preempted() const noexcept { return m_options & Option::PREEMPTED; }

    // ============================================================================
    // Option 计数操作（低 24 位，非原子，仅构建阶段调用）
    // ============================================================================
    [[nodiscard]] std::size_t _join_count() const noexcept {
        return m_options & Option::COUNT_MASK;
    }

    void _set_join_count(std::size_t c) noexcept {
        m_options = (m_options & Option::FLAG_MASK)
        | (static_cast<Option::type>(c) & Option::COUNT_MASK);
    }

    void _add_join_count(std::size_t n) noexcept {
        auto cur = m_options & Option::COUNT_MASK;
        TFL_ASSERT(cur + n <= Option::COUNT_MASK && "join count overflow");
        m_options = (m_options & Option::FLAG_MASK)
                    | (static_cast<Option::type>(cur + n) & Option::COUNT_MASK);
    }

    void _sub_join_count(std::size_t n) noexcept {
        auto cur = m_options & Option::COUNT_MASK;
        TFL_ASSERT(cur >= n && "join count underflow");
        m_options = (m_options & Option::FLAG_MASK)
                    | (static_cast<Option::type>(cur - n) & Option::COUNT_MASK);
    }


    // ============================================================================
    // State 操作（原子，执行阶段调用）
    // ============================================================================
    void _set_state(State::type s) noexcept {
        m_state.fetch_or(s, std::memory_order_relaxed);
    }
    void _clear_state(State::type s) noexcept {
        m_state.fetch_and(~s, std::memory_order_relaxed);
    }
    [[nodiscard]] bool _has_state(State::type s) const noexcept {
        return m_state.load(std::memory_order_relaxed) & s;
    }
    [[nodiscard]] State::type _load_state() const noexcept {
        return m_state.load(std::memory_order_relaxed);
    }
    [[nodiscard]] State::type _fetch_set_state(State::type s) noexcept {
        return m_state.fetch_or(s, std::memory_order_relaxed);
    }
    [[nodiscard]] State::type _fetch_clear_state(State::type s) noexcept {
        return m_state.fetch_and(~s, std::memory_order_relaxed);
    }

    void _set_exception() noexcept {
        m_state.fetch_or(State::EXCEPTION, std::memory_order_relaxed);
    }
    void _set_caught() noexcept {
        m_state.fetch_or(State::CAUGHT, std::memory_order_relaxed);
    }

    [[nodiscard]] bool _is_exception() const noexcept {
        return m_state.load(std::memory_order_relaxed) & State::EXCEPTION;
    }
    [[nodiscard]] bool _is_caught() const noexcept {
        return m_state.load(std::memory_order_relaxed) & State::CAUGHT;
    }

    // ============================================================================
    // 组合查询
    // ============================================================================
    [[nodiscard]] bool _is_stopped() const noexcept {
        return (m_state.load(std::memory_order_relaxed) & State::EXCEPTION) || m_topology->_is_stopped();
    }

    // ============================================================================
    // 复合原子操作
    // ============================================================================
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

    // ============================================================
    // 边访问
    // ============================================================
    [[nodiscard]] std::span<Work*> _successors() noexcept {
        return {m_edges.data(), m_num_successors};
    }
    [[nodiscard]] std::span<Work* const> _successors() const noexcept {
        return {m_edges.data(), m_num_successors};
    }
    [[nodiscard]] std::span<Work*> _predecessors() noexcept {
        return {m_edges.data() + m_num_successors, m_edges.size() - m_num_successors};
    }
    [[nodiscard]] std::span<Work* const> _predecessors() const noexcept {
        return {m_edges.data() + m_num_successors, m_edges.size() - m_num_successors};
    }
    [[nodiscard]] std::size_t _num_predecessors() const noexcept {
        return m_edges.size() - m_num_successors;
    }

    // ============================================================
    // 信号量访问
    // ============================================================
    [[nodiscard]] SemaphoreData& _ensure_semaphores() {
        if (!m_semaphores) {
            m_semaphores = std::make_unique<SemaphoreData>();
        }
        return *m_semaphores;
    }

    // 如果数据清空则释放
    void _try_release_semaphores() noexcept {
        if (m_semaphores && m_semaphores->empty()) {
            m_semaphores.reset();
        }
    }
    [[nodiscard]] std::span<Semaphore*> _acquires() noexcept {
        return m_semaphores ? std::span<Semaphore*>{m_semaphores->acquires} : std::span<Semaphore*>{};
    }
    [[nodiscard]] std::span<Semaphore* const> _acquires() const noexcept {
        return m_semaphores ? std::span<Semaphore* const>{m_semaphores->acquires} : std::span<Semaphore* const>{};
    }
    [[nodiscard]] std::span<Semaphore*> _releases() noexcept {
        return m_semaphores ? std::span<Semaphore*>{m_semaphores->releases} : std::span<Semaphore*>{};
    }
    [[nodiscard]] std::span<Semaphore* const> _releases() const noexcept {
        return m_semaphores ? std::span<Semaphore* const>{m_semaphores->releases} : std::span<Semaphore* const>{};
    }
    [[nodiscard]] std::size_t _num_acquires() const noexcept {
        return m_semaphores ? m_semaphores->acquires.size() : 0;
    }
    [[nodiscard]] std::size_t _num_releases() const noexcept {
        return m_semaphores ? m_semaphores->releases.size() : 0;
    }

    // ============================================================
    // 查找辅助
    // ============================================================
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




    // ============================================================
    // 信号量操作
    // ============================================================
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

    // ============================================================
    // 边操作
    // ============================================================
    void _erase_successor_at(std::size_t idx) noexcept;
    void _erase_predecessor_at(std::size_t idx) noexcept;
    void _precede(Work* target);
    void _remove_successor(Work* target) noexcept;
    void _clear_predecessors() noexcept;
    void _clear_successors() noexcept;

    // ============================================================
    // 状态管理
    // ============================================================
    void _set_up(Work* parent, Topology* t) noexcept;
    void _set_up(std::size_t join_counter) noexcept;
    [[nodiscard]] bool _can_reach(const Work* target) const;
    [[nodiscard]] std::expected<void, std::string_view> _can_precede(Work* target) const;

    // ============================================================
    // Handler 工厂（同步任务）
    // ============================================================
    template <typename T>
        requires (capturable<T> && basic_invocable<T>)
    [[nodiscard]] static auto _make_basic(T&&);

    template <typename T>
        requires (capturable<T> && branch_invocable<T>)
    [[nodiscard]] static auto _make_branch(T&&);

    template <typename T>
        requires (capturable<T> && multi_branch_invocable<T>)
    [[nodiscard]] static auto _make_multi_branch(T&&);

    template <typename T>
        requires (capturable<T> && jump_invocable<T>)
    [[nodiscard]] static auto _make_jump(T&&);

    template <typename T>
        requires (capturable<T> && multi_jump_invocable<T>)
    [[nodiscard]] static auto _make_multi_jump(T&&);

    template <typename T>
        requires (capturable<T> && runtime_invocable<T>)
    [[nodiscard]] static auto _make_runtime(T&&);

    template <typename F, typename P>
        requires (capturable<P> && flow_type<F> && predicate<P>)
    [[nodiscard]] static auto _make_subflow(F&&, P&&);

    // ============================================================
    // Handler 工厂（异步任务）
    // ============================================================

    template <typename T>
        requires (capturable<T> && basic_invocable<T>)
    [[nodiscard]] static auto _make_async_basic(T&&);

    template <typename T>
        requires (capturable<T> && runtime_invocable<T>)
    [[nodiscard]] static auto _make_async_runtime(T&&);


    template <typename T, typename R>
        requires (capturable<T> && basic_invocable<T>)
    [[nodiscard]] static auto _make_async_basic(T&&, std::promise<R>&&);

    template <typename T, typename R>
        requires (capturable<T> && runtime_invocable<T>)
    [[nodiscard]] static auto _make_async_runtime(T&&, std::promise<R>&&);


    template <typename T>
        requires (capturable<T> && basic_invocable<T>)
    [[nodiscard]] static auto _make_dep_async_basic(T&&);

    template <typename T>
        requires (capturable<T> && runtime_invocable<T>)
    [[nodiscard]] static auto _make_dep_async_runtime(T&&);

    template <typename F, typename P, typename C>
        requires (capturable<P, C> && flow_type<F> && predicate<P> && callback<C>)
    [[nodiscard]] static auto _make_dep_flow(F&&, P&&, C&&);




    // ============================================================
    // 核心工厂函数声明
    // ============================================================
    template <typename F>
        requires std::invocable<F, Executor&, Worker&, Work*, Work*&>
    [[nodiscard]] static Work* make(Graph* graph, TaskType type, Option::type options, F&& f);

    template <typename F>
        requires std::invocable<F, Executor&, Worker&, Work*, Work*&>
    [[nodiscard]] static Work* make(Executor& exe, TaskType type, Option::type options, F&& f);

    // ============================================================
    // 静态资源释放
    // ============================================================
    // 对应 Work::make 的销毁函数。
    // 如果 work 是 Topology 的根节点，释放 Topology；否则仅释放 work 自身。
    static void destroy(Work* work) noexcept;
};


// ============================================================================
// WorkImpl - 唯一的模板子类
// ============================================================================
template <typename F>
    requires std::invocable<F, Executor&, Worker&, Work*, Work*&>
class WorkImpl final : public Work {
    F m_func;

public:
    template <typename U>
        requires std::constructible_from<F, U>
    explicit WorkImpl(Graph* graph, TaskType type, Option::type options, U&& f)
        : Work{graph, type, options}
        , m_func{std::forward<U>(f)} {}

    template <typename U>
        requires std::constructible_from<F, U>
    explicit WorkImpl(Topology* topo, TaskType type, Option::type options, U&& f)
        : Work{topo, type, options}
        , m_func{std::forward<U>(f)} {}

    void invoke(Executor& exec, Worker& wr, Work*& cache) override final {
        m_func(exec, wr, this, cache);
    }
};
// ============================================================================
// 推导指引！
// ============================================================================
template <typename U>
WorkImpl(Graph*, TaskType, Work::Option::type, U) -> WorkImpl<std::decay_t<U>>;

template <typename U>
WorkImpl(Topology*, TaskType, Work::Option::type, U) -> WorkImpl<std::decay_t<U>>;

// ============================================================================
// 工厂函数
// ============================================================================
template <typename F>
    requires std::invocable<F, Executor&, Worker&, Work*, Work*&>
Work* Work::make(Graph* graph, TaskType type, Work::Option::type options, F&& f) {
    return new WorkImpl<std::decay_t<F>>(graph, type, options, std::forward<F>(f));
}

template <typename F>
    requires std::invocable<F, Executor&, Worker&, Work*, Work*&>
Work* Work::make(Executor& exe, TaskType type, Work::Option::type options, F&& f) {
    Topology* topo = new Topology(exe);
    topo->m_work = new WorkImpl<std::decay_t<F>>(topo, type, options, std::forward<F>(f));
    return topo->m_work;
}
// ============================================================================
// Work 静态销毁函数实现
// ============================================================================
inline void Work::destroy(Work* work) noexcept {
    // 1. 安全检查
    if (!work) {
        return;
    }

    // 2. 检查该 Work 是否是一个 Topology 的持有者/根节点
    //    (通常由 Work::make(Executor*, ...) 创建的情况)
    //    注意：需要先保存 topology 指针，因为 work 可能在删除 topology 后被访问
    Topology* topo = work->m_topology;
    if (topo && topo->m_work == work) {
        delete work;
        delete topo;
    } else {
        // 3. 普通节点情况
        //    (由 Work::make(Graph*, ...) 创建，或者作为子任务)
        //    直接删除该节点。因为 destroy 是 Work 的静态成员，
        //    它有权限调用 protected/private 的析构函数。
        delete work;
    }
}
// ============================================================================
// 状态管理
// ============================================================================
inline void Work::_set_up(Work* const parent, Topology* const t) noexcept {
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

// ----------------------------------------------------------------------------
// 边操作
// ----------------------------------------------------------------------------
inline void Work::_erase_successor_at(std::size_t idx) noexcept {
    TFL_ASSERT(idx < m_num_successors);
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

    m_edges.push_back(target);
    if (m_num_successors < m_edges.size() - 1) {
        std::swap(m_edges[m_num_successors], m_edges.back());
    }
    ++m_num_successors;
    target->m_edges.push_back(this);

    // 构建期累加 target 的 join count
    if (auto w = _join_weight()) {
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

    // 构建期扣减 target 的 join count
    if (auto w = _join_weight()) {
        target->_sub_join_count(w);
    }
}

inline void Work::_clear_predecessors() noexcept {
    for (Work* pred : _predecessors()) {
        const std::size_t idx = _find_index<Work>(pred->_successors(), this);
        TFL_ASSERT(idx != static_cast<std::size_t>(-1) && "successor must exist");
        pred->_erase_successor_at(idx);
    }

    // 重算 join count: 清掉所有前驱贡献
    _set_join_count(0);
    while (m_edges.size() > m_num_successors) {
        m_edges.pop_back();
    }
}


inline void Work::_clear_successors() noexcept {
    // 扣减每个后继的 join count
    if (auto w = _join_weight()) {
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
    //if (target->_can_reach(this)) return std::unexpected{"would create cycle"};
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
// ----------------------------------------------------------------------------
// 信号量操作
// ----------------------------------------------------------------------------
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

}  // namespace tfl
