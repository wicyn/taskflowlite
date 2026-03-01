#pragma once
#include <atomic>
#include "forward.hpp"
namespace tfl {

class Topology {
    friend class Work;
    friend class Task;
    friend class AsyncTask;
    friend class Graph;
    friend class Executor;

    // ---- 子类友元 ----
    TFL_WORK_SUBCLASS_FRIENDS;

protected:
    explicit Topology(Executor&) noexcept;
    ~Topology() noexcept = default;

private:
    enum class State : std::int32_t {
        Idle = 0,       // 未启动
        Running = 1,    // 运行中
        Locking = 2,    // 短暂锁，正在添加后继
        Finished = 3    // 已完成
    };

    std::atomic<State> m_state{State::Idle};
    std::atomic_flag m_stopped = ATOMIC_FLAG_INIT;
    Work* m_work{nullptr};
    Executor& m_executor;
    std::atomic<std::size_t> m_use_count{0};

    void _wait() const noexcept;
    void _stop() noexcept;
    void _incref() noexcept;
    bool _decref() noexcept;

    [[nodiscard]] bool _is_stopped() const noexcept;
    [[nodiscard]] bool _is_running() const noexcept;
    [[nodiscard]] bool _is_finished() const noexcept;
};

inline Topology::Topology(Executor& exec) noexcept
    : m_executor{exec} {}

inline void Topology::_wait() const noexcept {
    auto state = m_state.load(std::memory_order_acquire);
    while (state != State::Finished) {
        m_state.wait(state, std::memory_order_acquire);
        state = m_state.load(std::memory_order_acquire);
    }
}

inline void Topology::_stop() noexcept {
    m_stopped.test_and_set(std::memory_order_relaxed);
}

inline void Topology::_incref() noexcept {
    m_use_count.fetch_add(1, std::memory_order_relaxed);
}

inline bool Topology::_decref() noexcept {
    return m_use_count.fetch_sub(1, std::memory_order_acq_rel) == 1;
}

inline bool Topology::_is_stopped() const noexcept {
    return m_stopped.test(std::memory_order_relaxed);
}

inline bool Topology::_is_running() const noexcept {
    auto s = m_state.load(std::memory_order_relaxed);
    return s == State::Running || s == State::Locking;
}

inline bool Topology::_is_finished() const noexcept {
    return m_state.load(std::memory_order_relaxed) == State::Finished;
}

}  // end of namespace tfl
