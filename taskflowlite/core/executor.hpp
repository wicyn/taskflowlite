#pragma once

#include <cassert>

#include "flow.hpp"
#include "async_task.hpp"
#include "runtime.hpp"
#include "branch.hpp"
#include "jump.hpp"
#include "unbounded_queue_bucket.hpp"
#include "graph.hpp"
#include "worker.hpp"
#include "unordered_dense.hpp"
namespace tfl {

class Executor : public Immovable<Executor> {
    friend class Work;
    friend class Flow;
    friend class AsyncTask;
    friend class Runtime;

    // ---- 子类友元 ----
    TFL_WORK_SUBCLASS_FRIENDS;

public:
    explicit Executor(WorkerHandler& handler, std::size_t num_workers = std::thread::hardware_concurrency());

    ~Executor() noexcept;

    template <typename F>
        requires flow_type<F>
    AsyncTask submit(F&& flow);

    template <typename F, typename C>
        requires (capturable<C> && flow_type<F> && callback<C>)
    AsyncTask submit(F&& flow, C&& callback);

    template <typename F>
        requires flow_type<F>
    AsyncTask submit(F&& flow, std::uint64_t num);

    template <typename F, typename C>
        requires (capturable<C> && flow_type<F> && callback<C>)
    AsyncTask submit(F&& flow, std::uint64_t num, C&& callback);

    template <typename F, typename P>
        requires (capturable<P> && flow_type<F> && predicate<P>)
    AsyncTask submit(F&& flow, P&& pred);

    template <typename F, typename P, typename C>
        requires (capturable<P, C> && flow_type<F> && predicate<P> && callback<C>)
    AsyncTask submit(F&& flow, P&& pred, C&& callback);

    template <typename T>
        requires (capturable<T> && basic_invocable<T>)
    AsyncTask submit(T&& task);

    template <typename T>
        requires (capturable<T> && runtime_invocable<T>)
    AsyncTask submit(T&& task);

    template <typename T>
        requires (capturable<T> && basic_invocable<T>)
    void silent_async(T&& task);

    template <typename T>
        requires (capturable<T> && runtime_invocable<T>)
    void silent_async(T&& task);

    template <typename T>
        requires (capturable<T> && basic_invocable<T>)
    auto async(T&& task) -> std::future<basic_return_t<T>>;

    template <typename T>
        requires (capturable<T> && runtime_invocable<T>)
    auto async(T&& task) -> std::future<runtime_return_t<T>>;

    void wait_for_all() const noexcept;

    [[nodiscard]] std::size_t num_workers() const noexcept;
    [[nodiscard]] std::size_t num_waiters() const noexcept;
    [[nodiscard]] std::size_t num_queues() const noexcept;
    [[nodiscard]] std::size_t num_topologies() const noexcept;

private:
    alignas(2 * std::hardware_destructive_interference_size)
        std::atomic<std::size_t> m_num_topologies{0};
    std::vector<Worker> m_workers;
    UnboundedQueueBucket<Work*> m_shared_queues;
    Notifier m_notifier;
    WorkerHandler& m_handler;

    unordered_dense::map<std::thread::id, Worker*> m_thread_worker_map;

    void _spawn(std::size_t);
    void _shutdown() noexcept;
    void _invoke(Worker&, Work*);
    [[nodiscard]] Work* _wait_for_work(Worker&) noexcept;

    void _set_up_graph(Graph&, Topology*, Work*);
    void _set_up_graph(Graph&, Topology*, Worker&, Work*);
    void _tear_down_task(Work*, Worker&, Work*&);
    void _tear_down_jump_task(Work*, Worker&, Work*&, Work*);
    void _tear_down_multi_jump_task(Work*, Worker&, Work*&, const SmallVector<Work*>&);

    template <typename I, typename S>
        requires std::sentinel_for<S, I>
    void _set_up_dep_async_task(Work*, I , S , std::size_t&);
    void _tear_down_dep_async_task(Work*, Worker&, Work*&);

    void _process_exception(Work*);

    template <std::random_access_iterator Iterator>
    void _schedule(Worker& wr, Iterator first, std::size_t n);
    template <std::random_access_iterator Iterator>
    void _schedule(Iterator first, std::size_t n);
    void _schedule(Worker&, Work*);
    void _schedule(Work*);
    void _schedule_parent(Work* parent, Worker& wr, Work*& cache);

    template <predicate Pred>
    void _corun_until(Worker& wr, Pred&& pred);
    void _corun_graph(Worker&, Graph&, Work*);

    void _increment_topology() noexcept;
    void _decrement_topology() noexcept;

    [[nodiscard]] Worker* _this_worker();

    //[[nodiscard]] int _this_worker_id() const;

};


inline Executor::Executor(WorkerHandler& handler, std::size_t num_workers)
    : m_workers{num_workers}
    , m_notifier{num_workers}
    , m_shared_queues{num_workers}
    , m_handler{handler} {
    if (num_workers == 0) {
        TFL_THROW("executor must define at least one worker");
    }
    _spawn(num_workers);
}

// Destructor
inline Executor::~Executor() noexcept {
    _shutdown();
}

inline void Executor::wait_for_all() const noexcept {
    size_t n = m_num_topologies.load(std::memory_order_acquire);
    while(n != 0) {
        m_num_topologies.wait(n, std::memory_order_acquire);
        n = m_num_topologies.load(std::memory_order_acquire);
    }
}

inline std::size_t Executor::num_workers() const noexcept {
    return m_workers.size();
}

inline std::size_t Executor::num_waiters() const noexcept {
    return m_notifier.num_waiters();
}

inline std::size_t Executor::num_queues() const noexcept {
    return m_workers.size() + m_shared_queues.size();
}

inline std::size_t Executor::num_topologies() const noexcept {
    return m_num_topologies.load(std::memory_order_relaxed);
}

inline void Executor::_shutdown() noexcept {
    wait_for_all();

    for(auto& wr : m_workers) {
        wr.m_terminate.test_and_set(std::memory_order_relaxed);
    }

    m_notifier.notify_all();

    for(auto& w : m_workers) {
        if(w.m_thread.joinable()) {
            w.m_thread.join();
        }
    }
}

///////////////////////////////////////////////////////////////////////////////////////

// ==================== Flow 类型定义 ====================
template <typename F>
    requires flow_type<F>
inline AsyncTask Executor::submit(F&& flow) {
    return submit(std::forward<F>(flow), 1ULL);
}

template <typename F, typename C>
    requires (capturable<C> && flow_type<F> && callback<C>)
inline AsyncTask Executor::submit(F&& flow, C&& callback) {
    return submit(std::forward<F>(flow), 1ULL, std::forward<C>(callback));
}

template <typename F>
    requires flow_type<F>
inline AsyncTask Executor::submit(F&& flow, std::uint64_t num) {
    return submit(std::forward<F>(flow), num, []() noexcept {});
}

template <typename F, typename C>
    requires (capturable<C> && flow_type<F> && callback<C>)
inline AsyncTask Executor::submit(F&& flow, std::uint64_t num, C&& callback) {
    return submit(std::forward<F>(flow)
                  ,[num]() mutable noexcept { return num-- == 0; }
                  ,std::forward<C>(callback));
}

template <typename F, typename P>
    requires (capturable<P> && flow_type<F> && predicate<P>)
inline AsyncTask Executor::submit(F&& flow, P&& pred) {
    return submit(std::forward<F>(flow)
                  ,std::forward<P>(pred)
                  ,[]() noexcept {});
}

template <typename F, typename P, typename C>
    requires (capturable<P, C> && flow_type<F> && predicate<P> && callback<C>)
inline AsyncTask Executor::submit(F&& flow, P&& pred, C&& callback) {
    constexpr auto options = Work::Option::ANCHORED | Work::Option::PREEMPTED;
    Work* work = Work::make_dep_flow(*this, options,
                                     std::forward<F>(flow), std::forward<P>(pred), std::forward<C>(callback));
    return AsyncTask{work};
}

// ==================== Basic 类型定义 ====================
template <typename T>
    requires (capturable<T> && basic_invocable<T>)
inline AsyncTask Executor::submit(T&& task) {
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_dep_async_basic(*this, options, std::forward<T>(task));
    return AsyncTask{work};
}

// ==================== Runtime 类型定义 ====================
template <typename T>
    requires (capturable<T> && runtime_invocable<T>)
inline AsyncTask Executor::submit(T&& task) {
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_dep_async_runtime(*this, options, std::forward<T>(task));
    return AsyncTask{work};
}


template <typename T>
    requires (capturable<T> && basic_invocable<T>)
inline void Executor::silent_async(T&& task) {
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_async_basic(*this, options, std::forward<T>(task));
    _increment_topology();
    if(Worker* wr = _this_worker()) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }
}

template <typename T>
    requires (capturable<T> && runtime_invocable<T>)
inline void Executor::silent_async(T&& task) {
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_async_runtime(*this, options, std::forward<T>(task));
    _increment_topology();
    if(Worker* wr = _this_worker()) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }
}

template <typename T>
    requires (capturable<T> && basic_invocable<T>)
inline auto Executor::async(T&& task) -> std::future<basic_return_t<T>> {
    using R = basic_return_t<T>;
    std::promise<R> promise;
    auto future = promise.get_future();
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_async_basic(*this, options, std::forward<T>(task), std::move(promise));
    _increment_topology();
    if(Worker* wr = _this_worker()) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }
    return future;
}

template <typename T>
    requires (capturable<T> && runtime_invocable<T>)
inline auto Executor::async(T&& task) -> std::future<runtime_return_t<T>> {
    using R = runtime_return_t<T>;
    std::promise<R> promise;
    auto future = promise.get_future();
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_async_runtime(*this, options, std::forward<T>(task), std::move(promise));
    _increment_topology();
    if(Worker* wr = _this_worker()) {
        _schedule(*wr, work);
    } else {
        _schedule(work);
    }
    return future;
}

inline void Executor::_spawn(std::size_t num_workers) {
    for(std::size_t id=0; id<num_workers; ++id) {
        auto& wr = m_workers[id];
        wr.m_id = id;
        wr.m_vtm = id;
        wr.m_adaptive_factor = 4;
        wr.m_max_steals = static_cast<std::uint32_t>(num_queues() * 2);
        wr.m_rng = Xoshiro{seed, std::random_device{}};
        wr.m_dist.reset(0, num_queues() - 1);

        wr.m_thread = std::thread([this, id, &wr]() noexcept {
            wr.m_rng.long_jump();
            m_handler.on_start(wr);
            Work* w = nullptr;
            for (;;) {
                while (w) {
                    try {
                        _invoke(wr, w);
                    } catch (...) {
                        if (!m_handler.on_exception(wr, std::current_exception())) {
                            goto exit;
                        }
                    }
                    w = wr.m_wslq.pop();
                }
                w = _wait_for_work(wr);
                if (wr.m_terminate.test(std::memory_order_relaxed)) [[unlikely]] {
                    break;
                }
            }
        exit:
            m_handler.on_stop(wr);
        });

        m_thread_worker_map.emplace(wr.m_thread.get_id(), std::addressof(wr));
    }
}

inline Work* Executor::_wait_for_work(Worker& wr) noexcept {
explore:
    std::size_t vtm = wr.m_vtm;
    std::uint32_t num_steals = 0;
    std::uint32_t const yield_limit = m_workers.size() * wr.m_adaptive_factor + wr.m_max_steals;
    std::size_t const shared_size = m_shared_queues.size();

    for (;;) {
        Work* w = (vtm < m_workers.size()) ? m_workers[vtm].m_wslq.steal() : m_shared_queues.steal(vtm - m_workers.size());

        if (w) {
            wr.m_vtm = vtm;
            wr.m_adaptive_factor = std::min(8u, wr.m_adaptive_factor + 1);
            return w;
        }

        if (++num_steals > wr.m_max_steals) {
            std::this_thread::yield();
            if (num_steals > yield_limit) {
                wr.m_adaptive_factor = std::max(1u, wr.m_adaptive_factor - 1);
                break;
            }
        }

        if (wr.m_terminate.test(std::memory_order_relaxed)) [[unlikely]] {
            return nullptr;
        }

        do {
            vtm = wr.m_dist(wr.m_rng);
        } while (vtm == wr.m_id);
    }

    m_notifier.prepare_wait(wr.m_id);

    for (std::size_t i = 0; i < shared_size; ++i) {
        if (!m_shared_queues.empty(i)) {
            m_notifier.cancel_wait(wr.m_id);
            wr.m_vtm = i + m_workers.size();
            goto explore;
        }
    }

    for (std::size_t i = 0; i < wr.m_id; ++i) {
        if (!m_workers[i].m_wslq.empty()) {
            m_notifier.cancel_wait(wr.m_id);
            wr.m_vtm = i;
            goto explore;
        }
    }

    for (std::size_t i = wr.m_id + 1; i < m_workers.size(); ++i) {
        if (!m_workers[i].m_wslq.empty()) {
            m_notifier.cancel_wait(wr.m_id);
            wr.m_vtm = i;
            goto explore;
        }
    }

    if (wr.m_terminate.test(std::memory_order_relaxed)) [[unlikely]] {
        m_notifier.cancel_wait(wr.m_id);
        return nullptr;
    }

    m_notifier.commit_wait(wr.m_id);
    goto explore;
}


inline void Executor::_set_up_graph(Graph& g, Topology* const topo, Work* parent) {
    if (std::size_t const n = g._set_up(parent, topo); n > 0) {
        _schedule(g.begin(), n);
    }
}

inline void Executor::_set_up_graph(Graph& g, Topology* topo, Worker& wr, Work* parent) {
    if (std::size_t const n = g._set_up(parent, topo); n > 0) {
        _schedule(wr, g.begin(), n);
    }
}

inline void Executor::_tear_down_task(Work* w, Worker& wr, Work*& cache) {
    w->m_join_counter.fetch_add(w->_join_count(), std::memory_order_relaxed);
    auto* parent = w->m_parent;
    if (!w->_is_stopped()) [[likely]] {
        for(auto* suc : w->_successors()) {
            if((suc->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1)) {
                parent->m_join_counter.fetch_add(1, std::memory_order_relaxed);
                if(cache) {
                    _schedule(wr, cache);
                }
                cache = suc;
            }
        }
    }

    _schedule_parent(parent, wr, cache);
}

inline void Executor::_tear_down_jump_task(Work* w, Worker& wr, Work*& cache, Work* target) {
    w->m_join_counter.fetch_add(w->_join_count(), std::memory_order_relaxed);
    auto* parent = w->m_parent;
    if (!w->_is_stopped() && target) [[likely]] {
        target->m_join_counter.store(0, std::memory_order_relaxed);
        parent->m_join_counter.fetch_add(1, std::memory_order_relaxed);
        if (cache) {
            _schedule(wr, cache);
        }
        cache = target;
    }
    _schedule_parent(parent, wr, cache);
}

inline void Executor::_tear_down_multi_jump_task(Work* w, Worker& wr, Work*& cache, const SmallVector<Work*>& targets) {

    w->m_join_counter.fetch_add(w->_join_count(), std::memory_order_relaxed);
    auto* parent = w->m_parent;
    if (!w->_is_stopped()) [[likely]] {
        for (auto* target : targets) {
            target->m_join_counter.store(0, std::memory_order_relaxed);
            parent->m_join_counter.fetch_add(1, std::memory_order_relaxed);
            if (cache) {
                _schedule(wr, cache);
            }
            cache = target;
        }
    }
    _schedule_parent(parent, wr, cache);
}

template <typename I, typename S>
    requires std::sentinel_for<S, I>
inline void Executor::_set_up_dep_async_task(Work* w, I first, S last, std::size_t& num_predecessors) {
    _increment_topology();

    for (; first != last; ++first) {
        auto* work = first->m_work;
        if (!work) {
            num_predecessors = w->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) - 1;
            continue;
        }

        auto& state = work->m_topology->m_state;
        for(;;) {
            auto target = Topology::State::Running;
            if (state.compare_exchange_strong(target, Topology::State::Locking,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
                work->m_edges.push_back(w);
                if (work->m_num_successors < work->m_edges.size() - 1) {
                    std::swap(work->m_edges[work->m_num_successors], work->m_edges.back());
                }
                ++work->m_num_successors;

                state.store(Topology::State::Running, std::memory_order_release);
                break;
            }
            if (target == Topology::State::Finished) {
                num_predecessors = w->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) - 1;
                break;
            }
        }
    }
}


inline void Executor::_tear_down_dep_async_task(Work* w, Worker& wr, Work*& cache) {
    auto* topo = w->m_topology;
    auto target = Topology::State::Running;
    while(!topo->m_state.compare_exchange_weak(target, Topology::State::Finished,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
        target = Topology::State::Running;
    }
    topo->m_state.notify_all();

    for(auto* const suc : w->_successors()) {
        if((suc->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1)) {
            auto& suc_exec = suc->m_topology->m_executor;
            if (&suc_exec == this) {
                if (cache) {
                    _schedule(wr, cache);
                }
                cache = suc;
            } else {
                suc_exec._schedule(suc);
            }
        }
    }
    _decrement_topology();
    if(topo->_decref()) {
        Work::destroy(w);
    }
}

void Executor::_process_exception(Work* w) {
    auto eptr = std::current_exception();
    Work* work = w;
    while (work && !work->_is_anchored()) {
        work->_set_exception();
        work = work->m_parent;
    }
    if (work && work->_try_catch_exception()) {
        work->m_exception_ptr = eptr;
    }
    w->m_exception_ptr = eptr;
}



template <std::random_access_iterator Iterator>
inline void Executor::_schedule(Worker& wr, Iterator first, std::size_t n) {
    if (n == 0) [[unlikely]] {
        return;
    }
    wr.m_wslq.push(first, n, [&](Iterator remaining, std::size_t count) {
        m_shared_queues.push(remaining, count);
    });
    m_notifier.notify_n(n);
}

template <std::random_access_iterator Iterator>
inline void Executor::_schedule(Iterator first, std::size_t n) {
    if (n == 0) [[unlikely]] {
        return;
    }
    m_shared_queues.push(first, n);
    m_notifier.notify_n(n);
}

inline void Executor::_schedule(Worker& wr, Work* w) {
    wr.m_wslq.push(w, [&]() {
        m_shared_queues.push(w);
    });
    m_notifier.notify_one();
}

inline void Executor::_schedule(Work* w) {
    m_shared_queues.push(w);
    m_notifier.notify_one();
}

inline void Executor::_schedule_parent(Work* parent, Worker& wr, Work*& cache) {
    auto ops = parent->m_options;
    if (parent->m_join_counter.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if(ops & Work::Option::PREEMPTED) {
            if (cache) {
                _schedule(wr, cache);
            }
            cache = parent;
        }
    }
}

template <predicate Pred>
inline void Executor::_corun_until(Worker& wr, Pred&& pred) {
    while (!std::invoke_r<bool>(pred)) {
        if (auto* w = wr.m_wslq.pop()) [[likely]] {
            _invoke(wr, w);
            continue;
        }

        std::uint32_t num_steals = 0;
        std::uint32_t const yield_limit = m_workers.size() * wr.m_adaptive_factor + wr.m_max_steals;
        std::size_t vtm = wr.m_vtm;

        while (!std::invoke_r<bool>(pred)) {
            Work* w = (vtm < m_workers.size())
            ? m_workers[vtm].m_wslq.steal()
            : m_shared_queues.steal(vtm - m_workers.size());

            if (w) [[likely]] {
                wr.m_vtm = vtm;
                wr.m_adaptive_factor = std::min(8u, wr.m_adaptive_factor + 1);
                _invoke(wr, w);
                break;
            }

            if (++num_steals > wr.m_max_steals) [[unlikely]] {
                std::this_thread::yield();
                if (num_steals > yield_limit) [[unlikely]] {
                    wr.m_adaptive_factor = std::max(1u, wr.m_adaptive_factor - 1);
                    break;
                }
            }

            do {
                vtm = wr.m_dist(wr.m_rng);
            } while (vtm == wr.m_id);
        }
    }
}

inline void Executor::_corun_graph(Worker& wr, Graph& g, Work* parent) {
    _set_up_graph(g, parent->m_topology, wr, parent);
    _corun_until(wr,[parent]() noexcept { return parent->m_join_counter.load(std::memory_order_acquire) == 0;});
}

inline void Executor::_increment_topology() noexcept {
    m_num_topologies.fetch_add(1, std::memory_order_relaxed);
}
inline void Executor::_decrement_topology() noexcept {
    if(m_num_topologies.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        m_num_topologies.notify_all();
    }
}

inline Worker* Executor::_this_worker() {
    auto itr = m_thread_worker_map.find(std::this_thread::get_id());
    return itr == m_thread_worker_map.end() ? nullptr : itr->second;
}

// inline int Executor::_this_worker_id() const {
//     auto i = m_thread_worker_map.find(std::this_thread::get_id());
//     return i == m_thread_worker_map.end() ? -1 : static_cast<int>(i->second->m_id);
// }

inline void Executor::_invoke(Worker& wr, Work* w) {
    do {
        Work* cache {nullptr};
        w->invoke(*this, wr, cache);
        w = cache;
    } while (w);
}

template <std::input_iterator I, std::sentinel_for<I> S>
    requires std::same_as<std::iter_value_t<I>, AsyncTask>
inline AsyncTask& AsyncTask::start(I first, S last) {
    auto* topo = m_work->m_topology;
    auto& exec = topo->m_executor;
    if (topo->_is_finished()) {
        throw Exception("AsyncTask Error: Cannot start a finished task.");
    }

    auto expected = Topology::State::Idle;
    if (!topo->m_state.compare_exchange_strong(expected, Topology::State::Running,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
        throw Exception("AsyncTask Error: Task is already running.");
    }
    topo->_incref();
    std::size_t num_predecessors = static_cast<std::size_t>(std::ranges::distance(first, last));
    m_work->_set_up(num_predecessors);
    exec._set_up_dep_async_task(m_work, first, last, num_predecessors);
    if (num_predecessors == 0) {
        if (Worker* w = exec._this_worker()) {
            exec._schedule(*w, m_work);
        } else {
            exec._schedule(m_work);
        }
    }

    return *this;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// 子类 invoke 实现
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define TFL_SEM_SCHEDULER [&exe, &wr](Work* t) {               \
auto& target_exec = t->m_topology->m_executor;              \
    if (std::addressof(target_exec) == std::addressof(exe)) {   \
        exe._schedule(wr, t);                                   \
} else {                                                    \
        target_exec._schedule(t);                               \
}                                                           \
}

#define TFL_OBSERVER_BEFORE(w, wr)                              \
if ((w)->m_observers) {                                         \
        for (auto& aspect : (w)->m_observers->observers) {          \
            aspect->on_before(WorkerView{wr});                      \
    }                                                           \
}

#define TFL_OBSERVER_AFTER(w, wr)                               \
if ((w)->m_observers) {                                         \
        for (auto& aspect : (w)->m_observers->observers) {          \
            aspect->on_after(WorkerView{wr});                       \
    }                                                           \
}

// ---------- BasicWork ----------
template <typename F>
void BasicWork<F>::invoke(Executor& exe, Worker& wr, Work*& cache) {
    if (this->_is_stopped()) [[unlikely]] {
        exe._schedule_parent(this->m_parent, wr, cache);
        return;
    }

    if (!this->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
        return;
    }

    TFL_OBSERVER_BEFORE(this, wr);

    if constexpr (noexcept(std::invoke(m_func))) {
        std::invoke(m_func);
    } else {
        try {
            std::invoke(m_func);
        } catch (...) {
            exe._process_exception(this);
        }
    }

    TFL_OBSERVER_AFTER(this, wr);

    this->_release_semaphores(TFL_SEM_SCHEDULER);
    exe._tear_down_task(this, wr, cache);
}

// ---------- BranchWork ----------
template <typename F>
void BranchWork<F>::invoke(Executor& exe, Worker& wr, Work*& cache) {
    if (this->_is_stopped()) [[unlikely]] {
        exe._schedule_parent(this->m_parent, wr, cache);
        return;
    }

    if (!this->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
        return;
    }

    TFL_OBSERVER_BEFORE(this, wr);

    Branch branch(*this);
    if constexpr (noexcept(std::invoke(m_func, branch))) {
        std::invoke(m_func, branch);
    } else {
        try {
            std::invoke(m_func, branch);
        } catch (...) {
            exe._process_exception(this);
        }
    }
    if(auto target = branch.m_target) {
        target->m_join_counter.fetch_sub(1, std::memory_order_relaxed);
    }

    TFL_OBSERVER_AFTER(this, wr);

    this->_release_semaphores(TFL_SEM_SCHEDULER);
    exe._tear_down_task(this, wr, cache);
}

// ---------- MultiBranchWork ----------
template <typename F>
void MultiBranchWork<F>::invoke(Executor& exe, Worker& wr, Work*& cache) {
    if (this->_is_stopped()) [[unlikely]] {
        exe._schedule_parent(this->m_parent, wr, cache);
        return;
    }

    if (!this->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
        return;
    }

    TFL_OBSERVER_BEFORE(this, wr);

    MultiBranch branch(*this);
    if constexpr (noexcept(std::invoke(m_func, branch))) {
        std::invoke(m_func, branch);
    } else {
        try {
            std::invoke(m_func, branch);
        } catch (...) {
            exe._process_exception(this);
        }
    }
    for(auto* target : branch.m_targets) {
        target->m_join_counter.fetch_sub(1, std::memory_order_relaxed);
    }

    TFL_OBSERVER_AFTER(this, wr);

    this->_release_semaphores(TFL_SEM_SCHEDULER);
    exe._tear_down_task(this, wr, cache);
}

// ---------- JumpWork ----------
template <typename F>
void JumpWork<F>::invoke(Executor& exe, Worker& wr, Work*& cache) {
    if (this->_is_stopped()) [[unlikely]] {
        exe._schedule_parent(this->m_parent, wr, cache);
        return;
    }
    if (!this->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
        return;
    }

    TFL_OBSERVER_BEFORE(this, wr);

    Jump jmp{*this};
    if constexpr (noexcept(std::invoke(m_func, jmp))) {
        std::invoke(m_func, jmp);
    } else {
        try {
            std::invoke(m_func, jmp);
        } catch (...) {
            exe._process_exception(this);
        }
    }

    TFL_OBSERVER_AFTER(this, wr);

    this->_release_semaphores(TFL_SEM_SCHEDULER);
    exe._tear_down_jump_task(this, wr, cache, jmp.m_target);
}

// ---------- MultiJumpWork ----------
template <typename F>
void MultiJumpWork<F>::invoke(Executor& exe, Worker& wr, Work*& cache) {
    if (this->_is_stopped()) [[unlikely]] {
        exe._schedule_parent(this->m_parent, wr, cache);
        return;
    }
    if (!this->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
        return;
    }

    TFL_OBSERVER_BEFORE(this, wr);

    MultiJump jmp{*this};
    if constexpr (noexcept(std::invoke(m_func, jmp))) {
        std::invoke(m_func, jmp);
    } else {
        try {
            std::invoke(m_func, jmp);
        } catch (...) {
            exe._process_exception(this);
        }
    }

    TFL_OBSERVER_AFTER(this, wr);

    this->_release_semaphores(TFL_SEM_SCHEDULER);
    exe._tear_down_multi_jump_task(this, wr, cache, jmp.m_targets);
}

// ---------- RuntimeWork ----------
template <typename F>
void RuntimeWork<F>::invoke(Executor& exe, Worker& wr, Work*& cache) {
    if (this->_is_stopped()) [[unlikely]] {
        exe._schedule_parent(this->m_parent, wr, cache);
        return;
    }

    if (!this->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
        return;
    }

    TFL_OBSERVER_BEFORE(this, wr);

    Runtime rt(*this, wr, *this->m_topology, exe);
    if constexpr (noexcept(std::invoke(m_func, rt))) {
        std::invoke(m_func, rt);
    } else {
        try {
            std::invoke(m_func, rt);
        } catch (...) {
            exe._process_exception(this);
        }
    }

    TFL_OBSERVER_AFTER(this, wr);

    this->_release_semaphores(TFL_SEM_SCHEDULER);
    exe._tear_down_task(this, wr, cache);
}

// ---------- SubflowWork ----------
template <typename FlowStore, typename P>
void SubflowWork<FlowStore, P>::invoke(Executor& exe, Worker& wr, Work*& cache) {
    decltype(auto) flow = detail::unwrap(m_flow_store);

    if (m_started) {
        TFL_OBSERVER_AFTER(this, wr);
    }

    if (this->_is_stopped()) [[unlikely]] {
        exe._schedule_parent(this->m_parent, wr, cache);
        m_started = false;
        return;
    }

    if (!m_started) {
        if (!this->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
            return;
        }
    }

    if (!std::invoke_r<bool>(m_pred)) {
        TFL_OBSERVER_BEFORE(this, wr);
        m_started = true;
        exe._set_up_graph(flow.m_graph, this->m_topology, wr, this);
    } else {
        m_started = false;
        this->_release_semaphores(TFL_SEM_SCHEDULER);
        exe._tear_down_task(this, wr, cache);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// ---------- AsyncBasicWork ----------
template <typename F>
void AsyncBasicWork<F>::invoke(Executor& exe, Worker& wr, Work*& cache) {
    if constexpr (noexcept(std::invoke(m_func))) {
        std::invoke(m_func);
    } else {
        try {
            std::invoke(m_func);
        } catch (...) {
        }
    }
    exe._decrement_topology();
    Work::destroy(this);
}

// ---------- AsyncRuntimeWork ----------
template <typename F>
void AsyncRuntimeWork<F>::invoke(Executor& exe, Worker& wr, Work*& cache) {
    Runtime rt(*this, wr, *this->m_topology, exe);
    if constexpr (noexcept(std::invoke(m_func, rt))) {
        std::invoke(m_func, rt);
    } else {
        try {
            std::invoke(m_func, rt);
        } catch (...) {
        }
    }
    exe._decrement_topology();
    Work::destroy(this);
}

// ---------- AsyncBasicPromiseWork ----------
template <typename F, typename R>
void AsyncBasicPromiseWork<F, R>::invoke(Executor& exe, Worker& wr, Work*& cache) {
    if constexpr (noexcept(std::invoke(m_func))) {
        if constexpr (std::is_void_v<R>) {
            std::invoke(m_func);
            m_promise.set_value();
        } else {
            m_promise.set_value(std::invoke(m_func));
        }
    } else {
        if constexpr (std::is_void_v<R>) {
            try {
                std::invoke(m_func);
            } catch (...) {
                m_promise.set_exception(std::current_exception());
                exe._decrement_topology();
                Work::destroy(this);
                return;
            }
            m_promise.set_value();
        } else {
            std::optional<R> result;
            try {
                result.emplace(std::invoke(m_func));
            } catch (...) {
                m_promise.set_exception(std::current_exception());
                exe._decrement_topology();
                Work::destroy(this);
                return;
            }
            m_promise.set_value(std::move(*result));
        }
    }
    exe._decrement_topology();
    Work::destroy(this);
}

// ---------- AsyncRuntimePromiseWork ----------
template <typename F, typename R>
void AsyncRuntimePromiseWork<F, R>::invoke(Executor& exe, Worker& wr, Work*& cache) {
    Runtime rt(*this, wr, *this->m_topology, exe);
    if constexpr (noexcept(std::invoke(m_func, rt))) {
        if constexpr (std::is_void_v<R>) {
            std::invoke(m_func, rt);
            m_promise.set_value();
        } else {
            m_promise.set_value(std::invoke(m_func, rt));
        }
    } else {
        if constexpr (std::is_void_v<R>) {
            try {
                std::invoke(m_func, rt);
            } catch (...) {
                m_promise.set_exception(std::current_exception());
                exe._decrement_topology();
                Work::destroy(this);
                return;
            }
            m_promise.set_value();
        } else {
            std::optional<R> result;
            try {
                result.emplace(std::invoke(m_func, rt));
            } catch (...) {
                m_promise.set_exception(std::current_exception());
                exe._decrement_topology();
                Work::destroy(this);
                return;
            }
            m_promise.set_value(std::move(*result));
        }
    }
    exe._decrement_topology();
    Work::destroy(this);
}

// ---------- DepAsyncBasicWork ----------
template <typename F>
void DepAsyncBasicWork<F>::invoke(Executor& exe, Worker& wr, Work*& cache) {
    if (!this->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
        return;
    }

    TFL_OBSERVER_BEFORE(this, wr);

    if constexpr (noexcept(std::invoke(m_func))) {
        std::invoke(m_func);
    } else {
        try {
            std::invoke(m_func);
        } catch (...) {
            exe._process_exception(this);
        }
    }

    TFL_OBSERVER_AFTER(this, wr);

    this->_release_semaphores(TFL_SEM_SCHEDULER);
    exe._tear_down_dep_async_task(this, wr, cache);
}

// ---------- DepAsyncRuntimeWork ----------
template <typename F>
void DepAsyncRuntimeWork<F>::invoke(Executor& exe, Worker& wr, Work*& cache) {
    if (!this->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
        return;
    }

    TFL_OBSERVER_BEFORE(this, wr);

    Runtime rt(*this, wr, *this->m_topology, exe);
    if constexpr (noexcept(std::invoke(m_func, rt))) {
        std::invoke(m_func, rt);
    } else {
        try {
            std::invoke(m_func, rt);
        } catch (...) {
            exe._process_exception(this);
        }
    }

    TFL_OBSERVER_AFTER(this, wr);

    this->_release_semaphores(TFL_SEM_SCHEDULER);
    exe._tear_down_dep_async_task(this, wr, cache);
}

// ---------- DepFlowWork ----------
template <typename FlowStore, typename P, typename C>
void DepFlowWork<FlowStore, P, C>::invoke(Executor& exe, Worker& wr, Work*& cache) {
    decltype(auto) flow = detail::unwrap(m_flow_store);

    if (m_started) {
        TFL_OBSERVER_AFTER(this, wr);
    } else {
        if (!this->_try_acquire_semaphores(TFL_SEM_SCHEDULER)) {
            return;
        }
    }

    if (!std::invoke_r<bool>(m_pred) && !this->_is_stopped()) {
        TFL_OBSERVER_BEFORE(this, wr);
        m_started = true;
        exe._set_up_graph(flow.m_graph, this->m_topology, wr, this);
    } else {
        m_started = false;
        this->_release_semaphores(TFL_SEM_SCHEDULER);
        std::invoke(m_callback);
        exe._tear_down_dep_async_task(this, wr, cache);
    }
}

#undef TFL_SEM_SCHEDULER
#undef TFL_OBSERVER_BEFORE
#undef TFL_OBSERVER_AFTER

///////////////////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////
template <typename F>
    requires flow_type<F>
inline AsyncTask Runtime::submit(F&& flow) {
    return submit(std::forward<F>(flow), 1ULL);
}

template <typename F, typename C>
    requires (capturable<C> && flow_type<F> && callback<C>)
inline AsyncTask Runtime::submit(F&& flow, C&& callback) {
    return submit(std::forward<F>(flow), 1ULL, std::forward<C>(callback));
}

template <typename F>
    requires flow_type<F>
inline AsyncTask Runtime::submit(F&& flow, std::uint64_t num) {
    return submit(std::forward<F>(flow), num, []() noexcept {});
}

template <typename F, typename C>
    requires (capturable<C> && flow_type<F> && callback<C>)
inline AsyncTask Runtime::submit(F&& flow, std::uint64_t num, C&& callback) {
    return submit(std::forward<F>(flow)
                  ,[num]() mutable noexcept { return num-- == 0; }
                  ,std::forward<C>(callback));
}

template <typename F, typename P>
    requires (capturable<P> && flow_type<F> && predicate<P>)
inline AsyncTask Runtime::submit(F&& flow, P&& pred) {
    return submit(std::forward<F>(flow)
                  ,std::forward<P>(pred)
                  ,[]() noexcept {});
}

template <typename F, typename P, typename C>
    requires (capturable<P, C> && flow_type<F> && predicate<P> && callback<C>)
inline AsyncTask Runtime::submit(F&& flow, P&& pred, C&& callback) {
    constexpr auto options = Work::Option::ANCHORED | Work::Option::PREEMPTED;
    Work* work = Work::make_dep_flow(m_executor, options,
                                     std::forward<F>(flow), std::forward<P>(pred), std::forward<C>(callback));
    return AsyncTask{work};
}

// ==================== Basic 类型定义 ====================
template <typename T>
    requires (capturable<T> && basic_invocable<T>)
inline AsyncTask Runtime::submit(T&& task) {
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_dep_async_basic(m_executor, options, std::forward<T>(task));
    return AsyncTask{work};
}

// ==================== Runtime 类型定义 ====================
template <typename T>
    requires (capturable<T> && runtime_invocable<T>)
inline AsyncTask Runtime::submit(T&& task) {
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_dep_async_runtime(m_executor, options, std::forward<T>(task));
    return AsyncTask{work};
}

template <typename T>
    requires (capturable<T> && basic_invocable<T>)
inline void Runtime::silent_async(T&& task) {
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_async_basic(m_executor, options, std::forward<T>(task));
    m_executor._increment_topology();
    m_executor._schedule(m_worker, work);
}

template <typename T>
    requires (capturable<T> && runtime_invocable<T>)
inline void Runtime::silent_async(T&& task) {
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_async_runtime(m_executor, options, std::forward<T>(task));
    m_executor._increment_topology();
    m_executor._schedule(m_worker, work);
}


template <typename T>
    requires (capturable<T> && basic_invocable<T>)
inline auto Runtime::async(T&& task) -> std::future<basic_return_t<T>> {
    using R = basic_return_t<T>;
    std::promise<R> promise;
    auto future = promise.get_future();
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_async_basic(m_executor, options, std::forward<T>(task), std::move(promise));
    m_executor._increment_topology();
    m_executor._schedule(m_worker, work);
    return future;
}

template <typename T>
    requires (capturable<T> && runtime_invocable<T>)
inline auto Runtime::async(T&& task) -> std::future<runtime_return_t<T>> {
    using R = runtime_return_t<T>;
    std::promise<R> promise;
    auto future = promise.get_future();
    constexpr auto options = Work::Option::ANCHORED;
    Work* work = Work::make_async_runtime(m_executor, options, std::forward<T>(task), std::move(promise));
    m_executor._increment_topology();
    m_executor._schedule(m_worker, work);
    return future;
}

inline void Runtime::run(Task task) {
    auto* w = task.m_work;

    if (w->m_parent != m_work.m_parent) {
        throw Exception("Task does not share the same parent as this Runtime.");
    }

    w->m_join_counter.store(0, std::memory_order_relaxed);
    w->m_parent->m_join_counter.fetch_add(1, std::memory_order_acq_rel);

    Work* cache{nullptr};
    w->invoke(m_executor, m_worker, cache);
    if (cache) {
        m_executor._schedule(m_worker, cache);
    }
}
inline void Runtime::run(Flow& flow) {
    constexpr auto options = Work::Option::ANCHORED;
    NullWork parent{m_work.m_topology, options};
    m_executor._corun_graph(m_worker, flow.m_graph, &parent);

    parent._rethrow_exception();
}

template <predicate Pred>
inline void Runtime::wait_until(Pred&& pred) {
    m_executor._corun_until(m_worker, std::forward<Pred>(pred));
}


}  // end of namespace tfl -----------------------------------------------------
