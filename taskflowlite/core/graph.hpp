/// @file graph.hpp
/// @brief 任务图容器 - DAG 节点的物理存储与生命周期管理
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include <vector>
#include "work.hpp"
#include "topology.hpp"

namespace tfl {

/// @brief 任务图容器 - DAG 节点的物理存储与生命周期管理
///
/// @details
/// 物理上持有一组 Work* 指针，管理其动态分配、O(1) 删除、运行前起点整理及 D2 可视化导出。
///
/// @par 核心职责
/// - RAII 语义：析构时自动清理所有节点内存
/// - O(1) 节点删除：Swap-with-last 算法
/// - 原地分区：将入度为 0 的起点节点整理到数组前端
///
/// @note 线程安全：非线程安全。构建图时必须单线程，调度时 Executor 只读访问。
class Graph {
    friend class Executor;
    friend class Flow;
    friend class Runtime;
    friend class Work;

    TFL_WORK_SUBCLASS_FRIENDS;

public:
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

    [[nodiscard]] constexpr iterator begin() noexcept { return m_works.begin(); }
    [[nodiscard]] constexpr iterator end() noexcept { return m_works.end(); }
    [[nodiscard]] constexpr const_iterator begin() const noexcept { return m_works.begin(); }
    [[nodiscard]] constexpr const_iterator end() const noexcept { return m_works.end(); }
    [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return m_works.cbegin(); }
    [[nodiscard]] constexpr const_iterator cend() const noexcept { return m_works.cend(); }
    [[nodiscard]] constexpr reverse_iterator rbegin() noexcept { return m_works.rbegin(); }
    [[nodiscard]] constexpr reverse_iterator rend() noexcept { return m_works.rend(); }
    [[nodiscard]] constexpr const_reverse_iterator rbegin() const noexcept { return m_works.rbegin(); }
    [[nodiscard]] constexpr const_reverse_iterator rend() const noexcept { return m_works.rend(); }
    [[nodiscard]] constexpr const_reverse_iterator crbegin() const noexcept { return m_works.crbegin(); }
    [[nodiscard]] constexpr const_reverse_iterator crend() const noexcept { return m_works.crend(); }

    [[nodiscard]] constexpr bool empty() const noexcept { return m_works.empty(); }
    [[nodiscard]] constexpr size_type size() const noexcept { return m_works.size(); }
    [[nodiscard]] constexpr size_type capacity() const noexcept { return m_works.capacity(); }

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

    // 禁用拷贝：防止双重释放
    constexpr Graph(const Graph&) = delete;
    constexpr Graph& operator=(const Graph&) = delete;

    constexpr Graph(Graph&& other) noexcept;
    constexpr Graph& operator=(Graph&& other) noexcept;

    /// @brief 节点注册：添加到图中
    [[nodiscard]] Work* _emplace(Work* work);

    /// @brief O(1) 节点删除：Swap-with-last
    constexpr void _erase(Work* const work) noexcept;

    /// @brief 原地分区：提取所有起点节点到数组前端
    /// @return 起点任务数量
    [[nodiscard]] std::size_t _set_up(Work* const parent, Topology* const t) noexcept;

    /// @brief 清空所有节点
    constexpr void _clear() noexcept;

    /// @brief D2 可视化导出
    [[nodiscard]] std::string _dump() const;
    void _dump(std::ostream& ostream) const;
};

// ============================================================================
// Implementation
// ============================================================================

constexpr Graph::Graph(Graph&& other) noexcept
    : m_works{std::exchange(other.m_works, {})} {}

constexpr Graph& Graph::operator=(Graph&& other) noexcept {
    if (this != &other) {
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

Work* Graph::_emplace(Work* work) {
    m_works.push_back(work);
    return work;
}

/// @brief O(1) 节点删除：Swap-with-last
///
/// @par 算法
/// 1. 断开该节点的所有前驱后继边
/// 2. 用最后一个节点覆盖当前节点
/// 3. pop_back
///
/// @note 节点顺序无关性：DAG 中节点遍历顺序不影响语义
constexpr void Graph::_erase(Work* const work) noexcept {
    if (!work || work->m_graph != this) return;

    work->_clear_predecessors();
    work->_clear_successors();

    auto it = std::ranges::find(m_works, work);
    TFL_ASSERT(it != m_works.end() && "target must exist in m_works");

    *it = m_works.back();
    m_works.pop_back();
    Work::destroy(work);
}

/// @brief 原地分区：提取起点节点
///
/// @par 算法（类似 std::partition）
/// 遍历数组，当发现入度为 0 的节点时，将其与前端交换。
/// 遍历完成后，前 n 个节点即为起点任务。
///
/// @post 前 n 个节点为起点任务，可直接提交给调度器
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

inline std::string Graph::_dump() const {
    std::string out;
    out.reserve(m_works.size() * 120);

    for (const auto* w : m_works) {
        out += w->dump();
        out += "\n";
    }

    for (const auto* w : m_works) {
        char src[24];
        std::snprintf(src, sizeof(src), "p%zx", reinterpret_cast<std::uintptr_t>(w));
        auto tt = w->type();
        std::size_t idx = 0;

        for (const auto* succ : w->_successors()) {
            char dst[24];
            std::snprintf(dst, sizeof(dst), "p%zx", reinterpret_cast<std::uintptr_t>(succ));

            out += src;
            out += " -> ";
            out += dst;

            if (tt == TaskType::Jump || tt == TaskType::MultiJump) {
                out += ": ";
                out += std::to_string(idx++);
                out += " {\n";
                out += "  style.stroke: \"#ef4444\"\n";
                out += "  style.stroke-width: 2\n";
                out += "  style.stroke-dash: 5\n";
                out += "  style.font-size: 14\n";
                out += "  style.font-color: \"#dc2626\"\n";
                out += "  style.bold: true\n";
                out += "}\n";
            }
            else if (tt == TaskType::Branch || tt == TaskType::MultiBranch) {
                out += ": ";
                out += std::to_string(idx++);
                out += " {\n";
                out += "  style.stroke: \"#3b82f6\"\n";
                out += "  style.stroke-width: 2\n";
                out += "  style.font-size: 14\n";
                out += "  style.font-color: \"#2563eb\"\n";
                out += "  style.bold: true\n";
                out += "}\n";
            }
            else {
                out += ": {style.stroke: \"#6b7280\"}\n";
            }
        }
    }

    return out;
}

inline void Graph::_dump(std::ostream& os) const {
    for (const auto* w : m_works) {
        w->dump(os);
        os << "\n";
    }

    for (const auto* w : m_works) {
        char src[24];
        std::snprintf(src, sizeof(src), "p%zx", reinterpret_cast<std::uintptr_t>(w));
        auto tt = w->type();
        std::size_t idx = 0;

        for (const auto* succ : w->_successors()) {
            char dst[24];
            std::snprintf(dst, sizeof(dst), "p%zx", reinterpret_cast<std::uintptr_t>(succ));

            os << src << " -> " << dst;

            if (tt == TaskType::Jump || tt == TaskType::MultiJump) {
                os << ": " << idx++ << " {\n";
                os << "  style.stroke: \"#ef4444\"\n";
                os << "  style.stroke-width: 2\n";
                os << "  style.stroke-dash: 5\n";
                os << "  style.font-size: 14\n";
                os << "  style.font-color: \"#dc2626\"\n";
                os << "  style.bold: true\n";
                os << "}\n";
            } else if (tt == TaskType::Branch || tt == TaskType::MultiBranch) {
                os << ": " << idx++ << " {\n";
                os << "  style.stroke: \"#3b82f6\"\n";
                os << "  style.stroke-width: 2\n";
                os << "  style.font-size: 14\n";
                os << "  style.font-color: \"#2563eb\"\n";
                os << "  style.bold: true\n";
                os << "}\n";
            } else {
                os << ": {style.stroke: \"#6b7280\"}\n";
            }
        }
    }
}

}  // namespace tfl
