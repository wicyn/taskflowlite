/// @file graph.hpp
/// @brief 任务图容器 Graph —— Work 节点的物理存储与生命周期管理
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
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
/// @details
/// `Graph` 是 `Flow` 的 **物理后端**：Flow 是用户视角的语义壳，Graph 才是真正
/// 持有 `std::vector<Work*>` 并负责 RAII 释放的实体。它出现的理由是分层 ——
/// 让 Flow 专注语义（emplace / precede / dump），Graph 专注存储（emplace /
/// erase / clear）。
///
/// ============================================================================
///  核心设计抉择
/// ============================================================================
/// **裸指针 + 析构清理**，而非 unique_ptr 容器：
/// - 节点之间需要互引（前驱 / 后继都是 Work\*）—— unique_ptr 会让边表持有
///   非拥有指针时类型不一致；
/// - 节点的物理内存归 Graph 独占管理，析构时统一 `destroy(w)` 收尾；
/// - 这使得边表 `std::vector<Work*>` 永远是一致的非拥有引用，逻辑清晰。
///
/// **O(1) 删除（swap-with-last）**：
/// - 删除任意节点：先在容器里找到位置（O(N) 线性搜，但典型图规模可接受），
///   把最后一个节点搬过去 + pop_back —— 实际删除是 O(1)；
/// - 改变了节点的物理顺序，但 DAG 节点遍历顺序与语义无关，安全。
///
/// **禁拷贝 + 私有移动**：
/// - 禁拷贝：节点指针多份会导致双重释放；
/// - 移动是 private：只让 friend Flow 调用，避免外部错误使用。
///
/// ============================================================================
///  与上下游的关系
/// ============================================================================
///
/// ```
///   ┌────────┐    持有     ┌────────┐    持有 (vec)    ┌────────┐
///   │  Flow  │ ───────────►│ Graph  │ ────────────────►│ Work*  │
///   └────────┘             └────────┘                  └────────┘
///                                ▲                          ▲
///                                │ 通过 m_graph 反向         │ 通过 m_edges 互引
///                                └──────────────────────────┘
/// ```
///
/// `Work::m_graph` 反指向所属 Graph，让节点在 `_can_precede` 等校验中能验证
/// "两个节点是否同图"（跨图建边是非法的）。
///
/// ============================================================================
///  迭代器 / 容器接口
/// ============================================================================
/// `Graph` 暴露 `begin/end/size/empty/operator[]/data` 等标准容器接口，
/// 让外部（主要是 Executor 调度路径）能直接把它当 `std::vector<Work*>` 用。
/// 这是为了让 `_set_up_graph` / `_cowait_graph` 等内部协议代码读起来更自然。
///
/// ============================================================================
///  D2 可视化
/// ============================================================================
/// `dump(ostream)` 把整张图渲染为 D2 描述：节点列表 + 出边定义。Branch / Jump
/// 边走专门的样式（虚线 / 红色），让可视化结果一眼就能看出控制流形态。
///
/// @see Flow                语义壳，本类的所有者
/// @see Work                被持有的节点
/// @see D2Renderer          可视化代际
/// @see work_factory.hpp     destroy(Work*) 的实现
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

    Graph() = default;
    ~Graph() noexcept;

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

    [[nodiscard]] bool empty() const noexcept { return m_works.empty(); }
    [[nodiscard]] size_type size() const noexcept { return m_works.size(); }
    [[nodiscard]] size_type capacity() const noexcept { return m_works.capacity(); }

    [[nodiscard]] reference operator[](size_type pos) noexcept { return m_works[pos]; }
    [[nodiscard]] const_reference operator[](size_type pos) const noexcept { return m_works[pos]; }
    [[nodiscard]] reference front() noexcept { return m_works.front(); }
    [[nodiscard]] const_reference front() const noexcept { return m_works.front(); }
    [[nodiscard]] reference back() noexcept { return m_works.back(); }
    [[nodiscard]] const_reference back() const noexcept { return m_works.back(); }
    [[nodiscard]] pointer data() noexcept { return m_works.data(); }
    [[nodiscard]] const_pointer data() const noexcept { return m_works.data(); }

    /// @brief D2 可视化导出
    void dump(std::ostream& ostream) const;
protected:

    // 禁用拷贝：防止双重释放
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;

    Graph(Graph&& other) noexcept;
    Graph& operator=(Graph&& other) noexcept;

    /// @brief 节点注册：添加到图中
    [[nodiscard]] Work* _emplace(Work* work);

    /// @brief O(1) 节点删除：Swap-with-last
    void _erase(Work* const work) noexcept;

    /// @brief 清空所有节点
    void _clear() noexcept;

private:
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

/// @brief O(1) 节点删除：Swap-with-last
///
/// @par 算法
/// 1. 断开该节点的所有前驱后继边
/// 2. 用最后一个节点覆盖当前节点
/// 3. pop_back
///
/// @note 节点顺序无关性：DAG 中节点遍历顺序不影响语义
/// @warning 调用方必须确保目标节点未处于执行状态（构建期单线程调用），否则直接 destroy 运行中的节点导致 UAF
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

inline void Graph::dump(std::ostream& os) const {
    for (const auto* w : m_works) {
        w->dump(os);
        os << "\n";
    }

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
