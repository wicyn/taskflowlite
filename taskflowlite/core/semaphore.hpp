#pragma once

#include <cstddef>
#include <vector>
#include <algorithm>
#include <cassert>
#include <mutex>
#include "exception.hpp"

namespace tfl {

class Semaphore {
    friend class Work;
    friend class Executor;
    friend class Task;

public:
    Semaphore(std::size_t max_value) noexcept
        : m_max_value{max_value}
        , m_value{max_value} {}

    Semaphore(std::size_t max_value, std::size_t current_value) noexcept
        : m_max_value{max_value}
        , m_value{(std::min)(current_value, max_value)} {}


    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    [[nodiscard]] std::size_t value() const noexcept {
        std::lock_guard lk{m_lock};
        return m_value;
    }

    [[nodiscard]] std::size_t max_value() const noexcept {
        return m_max_value;
    }

    /// 重置为 value, 清空等待队列 (如果非空则抛出 Exception)
    void reset(std::size_t max_value) {
        std::lock_guard lk{m_lock};
        if (!m_waiters.empty()) {
            throw Exception("cannot reset while waiters exist.");
        }
        m_max_value = max_value;
        m_value = m_max_value;
    }

    /// 重新设定 value 并重置 (如果等待队列非空则抛出 Exception)
    void reset(std::size_t max_value, std::size_t current_value) {
        std::lock_guard lk{m_lock};
        if (!m_waiters.empty()) {
            throw Exception("cannot reset while waiters exist.");
        }
        m_max_value = max_value;
        m_value = (std::min)(current_value, max_value);
    }

private:
    mutable std::mutex m_lock;
    std::size_t m_max_value{0};
    std::size_t m_value{0};
    std::vector<Work*> m_waiters;

    [[nodiscard]] bool _try_acquire(Work* w) {
        std::lock_guard lk{m_lock};

        if (m_value > 0) {
            --m_value;
            return true;
        }
        m_waiters.push_back(w);
        return false;
    }

    template <typename F>
        requires std::invocable<F&, Work*>
    void _release(F&& on_wake) {
        std::vector<Work*> batch;
        {
            std::lock_guard lk{m_lock};
            if (m_value < m_max_value) {
                ++m_value;
                batch.swap(m_waiters);
            }
        }
        for (auto* w : batch) {
            std::invoke(on_wake, w);
        }
    }
};

}  // namespace tfl
