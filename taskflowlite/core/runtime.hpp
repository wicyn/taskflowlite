#pragma once

#include "worker.hpp"
#include "flow.hpp"

namespace tfl {

class Runtime  {
    friend class Work;
    friend class Flow;
    friend class Executor;
    friend class Worker;

    // ---- 子类友元 ----
    TFL_WORK_SUBCLASS_FRIENDS;

public:

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

    void run(Task task);

    void run(Flow& flow);

    template <predicate Pred>
    void wait_until(Pred&& pred);

    Worker& worker() noexcept;
    const Worker& worker() const noexcept;
private:
    Work& m_work;
    Worker& m_worker;
    Topology& m_topology;
    Executor& m_executor;

    explicit Runtime(Work& work, Worker& wr, Topology& topo, Executor& exec) noexcept
        : m_work{work}, m_worker{wr}, m_topology{topo}, m_executor{exec} {}
};

inline Worker& Runtime::worker() noexcept{
    return m_worker;
}

inline const Worker& Runtime::worker() const noexcept{
    return m_worker;
}

}



