#pragma once
#include <vector>
#include "work.hpp"
#include "topology.hpp"
namespace tfl {

class Graph {
    friend class Executor;
    friend class Flow;
    friend class Runtime;
    friend class Work;
public:
    // 类型别名
    using value_type = Work*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = Work*&;
    using const_reference = Work* const&;
    using pointer = Work**;
    using const_pointer = Work* const*;
    using iterator = std::vector<Work*>::iterator;
    using const_iterator = std::vector<Work*>::const_iterator;
    using reverse_iterator = std::vector<Work*>::reverse_iterator;
    using const_reverse_iterator = std::vector<Work*>::const_reverse_iterator;

    constexpr Graph() = default;
    constexpr ~Graph() noexcept;

    // 迭代器
    [[nodiscard]] constexpr iterator begin() noexcept { return m_works.begin(); }
    [[nodiscard]] constexpr iterator end() noexcept { return m_works.end(); }
    [[nodiscard]] constexpr const_iterator begin() const noexcept { return m_works.begin(); }
    [[nodiscard]] constexpr const_iterator end() const noexcept { return m_works.end(); }
    [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return m_works.cbegin(); }
    [[nodiscard]] constexpr const_iterator cend() const noexcept { return m_works.cend(); }

    // 反向迭代器
    [[nodiscard]] constexpr reverse_iterator rbegin() noexcept { return m_works.rbegin(); }
    [[nodiscard]] constexpr reverse_iterator rend() noexcept { return m_works.rend(); }
    [[nodiscard]] constexpr const_reverse_iterator rbegin() const noexcept { return m_works.rbegin(); }
    [[nodiscard]] constexpr const_reverse_iterator rend() const noexcept { return m_works.rend(); }
    [[nodiscard]] constexpr const_reverse_iterator crbegin() const noexcept { return m_works.crbegin(); }
    [[nodiscard]] constexpr const_reverse_iterator crend() const noexcept { return m_works.crend(); }

    // 容量
    [[nodiscard]] constexpr bool empty() const noexcept { return m_works.empty(); }
    [[nodiscard]] constexpr size_type size() const noexcept { return m_works.size(); }
    [[nodiscard]] constexpr size_type capacity() const noexcept { return m_works.capacity(); }

    // 元素访问
    [[nodiscard]] constexpr reference operator[](size_type pos) noexcept { return m_works[pos]; }
    [[nodiscard]] constexpr const_reference operator[](size_type pos) const noexcept { return m_works[pos]; }
    [[nodiscard]] constexpr reference front() noexcept { return m_works.front(); }
    [[nodiscard]] constexpr const_reference front() const noexcept { return m_works.front(); }
    [[nodiscard]] constexpr reference back() noexcept { return m_works.back(); }
    [[nodiscard]] constexpr const_reference back() const noexcept { return m_works.back(); }
    [[nodiscard]] constexpr pointer data() noexcept { return m_works.data(); }
    [[nodiscard]] constexpr const_pointer data() const noexcept { return m_works.data(); }

private:
    std::vector<Work*> m_works;

    constexpr Graph(const Graph&) = delete;
    constexpr Graph& operator=(const Graph&) = delete;
    constexpr Graph(Graph&& other) noexcept;
    constexpr Graph& operator=(Graph&& other) noexcept;

    template <typename F>
        requires std::invocable<F, Executor&, Worker&, Work*, Work*&>
    [[nodiscard]] Work* _emplace(TaskType, Work::Option::type, F&&);
    constexpr void _erase(Work* const) noexcept;
    [[nodiscard]] std::size_t _set_up(Work* const parent, Topology* const t) noexcept;
    constexpr void _clear() noexcept;
};


// 移动构造
constexpr Graph::Graph(Graph&& other) noexcept
    : m_works{std::exchange(other.m_works, {})}
{}

// 移动赋值
constexpr Graph&  Graph::operator=(Graph&& other) noexcept {
    if (this != &other) {
        // 先清理自己的资源
        _clear();
        m_works = std::exchange(other.m_works, {});
    }
    return *this;
}

constexpr void Graph::_clear() noexcept {
    for (Work* w : m_works) {
        Work::destroy(w);
    }
    m_works.clear();
}


constexpr Graph::~Graph() noexcept {
    _clear();
}

template <typename F>
    requires std::invocable<F, Executor&, Worker&, Work*, Work*&>
Work* Graph::_emplace(TaskType type, Work::Option::type options, F&& f) {
    Work* work = Work::make(this, type, options, std::forward<F>(f));
    m_works.push_back(work);
    return work;
}

// Graph 内部成员函数
constexpr void Graph::_erase(Work* const target) noexcept {
    if (!target || target->m_graph != this) return;
    target->_clear_predecessors();
    target->_clear_successors();

    auto it = std::ranges::find(m_works, target);
    TFL_ASSERT(it != m_works.end() && "target must exist in m_works");

    *it = m_works.back();
    m_works.pop_back();
    Work::destroy(target);
}

inline std::size_t Graph::_set_up(Work* const parent, Topology* const t) noexcept {
    Work** const data = m_works.data();
    std::size_t const size = m_works.size();
    std::size_t n = 0;

    for (std::size_t i = 0; i < size; ++i) {
        data[i]->_set_up(parent, t);
        if (data[i]->_num_predecessors() == 0) {
            std::swap(data[i], data[n++]);
        }
    }

    parent->m_join_counter.store(n, std::memory_order_relaxed);
    return n;
}

}
