/// @file  graph.hpp
/// @brief 任务图容器 Graph —— Work 节点的物理存储与生命周期管理。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <vector>
#include "work.hpp"
#include "work_factory_fwd.hpp"
#include "topology.hpp"

namespace tfl {

/// @brief 任务图节点容器 —— 一组 Work* 的所有权管理者。
///
/// 裸指针 + 独占所有权: 节点间互引用非拥有 Work*，物理内存由 Graph 统一释放。
/// O(1) 删除用 swap-with-last 策略。禁拷贝，移动操作为私有（仅友元可转移所有权）。
/// 暴露标准容器接口（begin/end/size/operator[]），供 Executor 调度路径直接使用。
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

    /// @brief 默认构造空图，m_works 为空 vector，不含任何 Work 节点。
    Graph() = default;

    /// @brief 析构时释放所有节点并清空边集合。
    ///
    /// 利用裸指针 + 独占所有权的设计：析构器遍历 m_works，对每个节点
    /// 调用 destroy(w)。节点间互引的 m_edges 不拥有指针，无需额外清理。
    ///
    /// @post 所有 Work* 通过 destroy() 回收内存；m_works 清空。
    ~Graph() noexcept;

    // ---- 迭代器 ----
    [[nodiscard]] iterator begin() noexcept { return m_works.begin(); }
    [[nodiscard]] iterator end() noexcept { return m_works.end(); }
    [[nodiscard]] const_iterator begin() const noexcept { return m_works.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return m_works.end(); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return m_works.cbegin(); }
    [[nodiscard]] const_iterator cend() const noexcept { return m_works.cend(); }
    [[nodiscard]] reverse_iterator rbegin() noexcept { return m_works.rbegin(); }
    [[nodiscard]] reverse_iterator rend() noexcept { return m_works.rend(); }
    [[nodiscard]] const_reverse_iterator rbegin() const noexcept { return m_works.rbegin(); }
    [[nodiscard]] const_reverse_iterator rend() const noexcept { return m_works.rend(); }
    [[nodiscard]] const_reverse_iterator crbegin() const noexcept { return m_works.crbegin(); }
    [[nodiscard]] const_reverse_iterator crend() const noexcept { return m_works.crend(); }

    // ---- 容量 ----
    [[nodiscard]] bool empty() const noexcept { return m_works.empty(); }
    [[nodiscard]] size_type size() const noexcept { return m_works.size(); }
    [[nodiscard]] size_type capacity() const noexcept { return m_works.capacity(); }

    // ---- 元素访问 ----
    [[nodiscard]] reference operator[](size_type pos) noexcept { return m_works[pos]; }
    [[nodiscard]] const_reference operator[](size_type pos) const noexcept { return m_works[pos]; }
    [[nodiscard]] reference front() noexcept { return m_works.front(); }
    [[nodiscard]] const_reference front() const noexcept { return m_works.front(); }
    [[nodiscard]] reference back() noexcept { return m_works.back(); }
    [[nodiscard]] const_reference back() const noexcept { return m_works.back(); }
    [[nodiscard]] pointer data() noexcept { return m_works.data(); }
    [[nodiscard]] const_pointer data() const noexcept { return m_works.data(); }

    /// @brief D2 可视化导出。
    ///
    /// 将图中所有节点及其边以 D2 格式输出。
    /// @param ostream 输出流。
    void dump(std::ostream& ostream) const;

protected:

    /// @brief 禁用拷贝，防止双重释放。
    ///
    /// m_works 中的裸指针由 Graph 独占所有权。若拷贝 Graph，
    /// 两个实例会各自尝试 destroy 同一组指针，导致双重释放。
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;

    /// @brief 移动构造（私有: 仅 friend Flow 调用）。
    ///
    /// 将 other 的所有权转移到 this，other 清空。
    Graph(Graph&& other) noexcept;

    /// @brief 移动赋值（私有: 仅 friend Flow 调用）。
    ///
    /// 先 _clear() 释放当前持有的节点，再接管 other 的所有权。
    Graph& operator=(Graph&& other) noexcept;

    /// @brief 节点注册: 添加到图中并设置 m_graph 回指。
    ///
    /// @param work 要注册的节点。
    /// @return 返回 work 指针以便链式调用。
    [[nodiscard]] Work* _emplace(Work* work);

    /// @brief O(1) 节点删除 —— swap-with-last 策略。
    ///
    /// @param work 要删除的节点。
    ///
    /// 算法步骤:
    /// 1. 断开该节点的所有前驱和后继边（双向同步清理）
    /// 2. 在 m_works 中找到目标位置（线性搜索）
    /// 3. 用最后一个节点覆盖当前节点（swap-with-last）
    /// 4. pop_back + destroy(work)
    ///
    /// DAG 中节点遍历顺序不影响语义，改变物理顺序是安全的。
    ///
    /// @warning 调用方必须确保目标节点未处于执行状态（构建期单线程调用），
    ///          否则直接 destroy 运行中的节点会导致 UAF。
    void _erase(Work* const work) noexcept;

    /// @brief 清空所有节点（遍历 m_works 逐个 destroy）。
    ///
    /// @post m_works 为空，所有节点的边表也随之失效。
    void _clear() noexcept;

private:
    /// @brief 节点存储 —— 裸指针数组，Graph 独占所有权。
    std::vector<Work*> m_works;
};

// ============================================================================
// Implementation
// ============================================================================

inline Graph::Graph(Graph&& other) noexcept
    : m_works{std::exchange(other.m_works, {})} {}

inline Graph& Graph::operator=(Graph&& other) noexcept {
    if (this != &other) {
        _clear();
        m_works = std::exchange(other.m_works, {});
    }
    return *this;
}

inline void Graph::_clear() noexcept {
    for (Work* w : m_works) {
        destroy(w);
    }
    m_works.clear();
}

inline Graph::~Graph() noexcept {
    _clear();
}

inline Work* Graph::_emplace(Work* work) {
    m_works.push_back(work);
    return work;
}

/// @brief O(1) 节点删除 —— swap-with-last 策略（实现）。
///
/// 算法步骤:
/// 1. 前置校验: work 非空且属于本图
/// 2. 调用 work->_clear_predecessors() / _clear_successors() 断开所有边，
///    同时在每个邻接节点侧同步删除反向边
/// 3. 在 m_works 中线性搜索 work 位置，用 back() 覆盖之
/// 4. pop_back + destroy(work) 释放节点内存
///
/// @post work 节点已从图中移除并销毁；m_works 大小减少 1。
///
/// @warning 本函数仅应在图构建期调用（单线程）。执行期图结构为只读。
inline void Graph::_erase(Work* const work) noexcept {
    if (!work || work->m_graph != this) return;

    work->_clear_predecessors();
    work->_clear_successors();

    auto it = std::ranges::find(m_works, work);
    TFL_ASSERT(it != m_works.end() && "target must exist in m_works");

    *it = m_works.back();
    m_works.pop_back();
    destroy(work);
}

/// @brief D2 可视化导出（实现）。
///
/// 两遍扫描: 第一遍输出节点声明（各节点调用自己的 dump），第二遍输出边定义。
///
/// 边样式:
/// - Jump/MultiJump: 红色虚线，加粗，带索引
/// - Branch/MultiBranch: 蓝色粗线，带索引
/// - 普通边: 灰色
inline void Graph::dump(std::ostream& os) const {
    // 第一遍: 节点声明
    for (const auto* w : m_works) {
        w->dump(os);
        os << "\n";
    }

    // 第二遍: 边定义
    for (const auto* w : m_works) {
        char src[24];
        std::snprintf(src, sizeof(src), "p%zx", reinterpret_cast<std::uintptr_t>(w));
        auto tt = w->m_type;
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
