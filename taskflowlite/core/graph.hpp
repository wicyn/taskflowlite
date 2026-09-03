/// @file graph.hpp
/// @brief 任务图容器 Graph —— Work 节点的物理存储与生命周期管理。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <ostream>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include "work.hpp"

namespace tfl {

/// @brief 保存任务图的节点集合，并维护 `Work` 节点的物理归属关系。
///
/// `Graph` 记录并独占其静态节点的 `Work*`，负责插入、摘除、销毁和移动后的图回指重绑。
/// 节点的具体创建统一由 Work 工厂完成；Graph 只接管已经构造完成且物理归属于自身的节点。
/// 节点间的边均为非拥有引用。
///
/// @note 类型不可复制；移动会转移节点集合并更新每个节点的图回指。
/// @warning 任何结构修改都不得与图执行并发。
class Graph final {
    friend class Executor;
    friend class Runtime;
    friend class Work;
    friend class SubFlow;
    friend class Flow;
    friend class TaskGroup;
    friend class D2Renderer;
    friend class FlowBuilder;

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

private:
    /// @brief 默认构造空图，m_works 为空 vector，不含任何 Work 节点。
    Graph() = default;

    /// @brief 禁止复制，防止节点被重复销毁。
    Graph(const Graph&) = delete;

    /// @brief 禁止复制赋值，防止节点被重复销毁。
    Graph& operator=(const Graph&) = delete;

    /// @brief 转移节点所有权，并把各节点的 `m_graph` 回指绑定到新 Graph。
    Graph(Graph&& other) noexcept;

    /// @brief 清理当前节点后接管另一个 Graph。
    Graph& operator=(Graph&& other) noexcept;

    /// @brief 销毁 Graph 及其当前拥有的全部 Work。
    ///
    /// 析构时统一调用 `clear()`；节点最终通过 `destroy_work()` 进入统一销毁路径。
    ~Graph() noexcept;

    // ---- 迭代器 ----
    /// @brief 获取节点指针序列的可修改首迭代器。
    /// @return `m_works.begin()`。
    [[nodiscard]] iterator begin() noexcept { return m_works.begin(); }

    /// @brief 获取节点指针序列的可修改尾后迭代器。
    /// @return `m_works.end()`。
    [[nodiscard]] iterator end() noexcept { return m_works.end(); }

    /// @brief 获取节点指针序列的只读首迭代器。
    /// @return `m_works.begin()`。
    [[nodiscard]] const_iterator begin() const noexcept { return m_works.begin(); }

    /// @brief 获取节点指针序列的只读尾后迭代器。
    /// @return `m_works.end()`。
    [[nodiscard]] const_iterator end() const noexcept { return m_works.end(); }

    /// @brief 获取显式只读首迭代器。
    /// @return `m_works.cbegin()`。
    [[nodiscard]] const_iterator cbegin() const noexcept { return m_works.cbegin(); }

    /// @brief 获取显式只读尾后迭代器。
    /// @return `m_works.cend()`。
    [[nodiscard]] const_iterator cend() const noexcept { return m_works.cend(); }

    /// @brief 获取节点指针序列的可修改反向首迭代器。
    /// @return 指向最后一个物理节点的反向迭代器。
    [[nodiscard]] reverse_iterator rbegin() noexcept { return m_works.rbegin(); }

    /// @brief 获取节点指针序列的可修改反向尾后迭代器。
    /// @return 反向遍历终点。
    [[nodiscard]] reverse_iterator rend() noexcept { return m_works.rend(); }

    /// @brief 获取节点指针序列的只读反向首迭代器。
    /// @return 指向最后一个物理节点的只读反向迭代器。
    [[nodiscard]] const_reverse_iterator rbegin() const noexcept { return m_works.rbegin(); }

    /// @brief 获取节点指针序列的只读反向尾后迭代器。
    /// @return 只读反向遍历终点。
    [[nodiscard]] const_reverse_iterator rend() const noexcept { return m_works.rend(); }

    /// @brief 获取显式只读反向首迭代器。
    /// @return `m_works.crbegin()`。
    [[nodiscard]] const_reverse_iterator crbegin() const noexcept { return m_works.crbegin(); }

    /// @brief 获取显式只读反向尾后迭代器。
    /// @return `m_works.crend()`。
    [[nodiscard]] const_reverse_iterator crend() const noexcept { return m_works.crend(); }

    // ---- 容量 ----
    /// @brief 判断图是否不包含节点。
    /// @return `size() == 0` 时返回 true。
    [[nodiscard]] bool empty() const noexcept { return m_works.empty(); }

    /// @brief 获取当前拥有的节点数量。
    /// @return `m_works.size()`。
    [[nodiscard]] size_type size() const noexcept { return m_works.size(); }

    /// @brief 获取节点指针数组当前容量。
    /// @return `m_works.capacity()`。
    [[nodiscard]] size_type capacity() const noexcept { return m_works.capacity(); }

    // ---- 元素访问 ----
    /// @brief 获取指定物理位置的节点指针引用。
    /// @param pos 零基物理下标。
    /// @return 可修改的 `Work*&`。
    /// @pre `pos < size()`。
    [[nodiscard]] reference operator[](size_type pos) noexcept { return m_works[pos]; }

    /// @brief 获取指定物理位置的节点指针只读引用。
    /// @param pos 零基物理下标。
    /// @return `Work* const&`。
    /// @pre `pos < size()`。
    [[nodiscard]] const_reference operator[](size_type pos) const noexcept { return m_works[pos]; }

    /// @brief 获取首个物理节点指针引用。
    /// @return `m_works.front()`。
    /// @pre 图非空。
    [[nodiscard]] reference front() noexcept { return m_works.front(); }

    /// @brief 获取首个物理节点指针只读引用。
    /// @return `m_works.front()`。
    /// @pre 图非空。
    [[nodiscard]] const_reference front() const noexcept { return m_works.front(); }

    /// @brief 获取最后一个物理节点指针引用。
    /// @return `m_works.back()`。
    /// @pre 图非空。
    [[nodiscard]] reference back() noexcept { return m_works.back(); }

    /// @brief 获取最后一个物理节点指针只读引用。
    /// @return `m_works.back()`。
    /// @pre 图非空。
    [[nodiscard]] const_reference back() const noexcept { return m_works.back(); }

    /// @brief 获取节点指针连续数组首地址。
    /// @return 可修改 `Work**`；空图时遵循 vector::data 语义。
    [[nodiscard]] pointer data() noexcept { return m_works.data(); }

    /// @brief 获取节点指针连续数组首地址。
    /// @return 只读 `Work* const*`；空图时遵循 vector::data 语义。
    [[nodiscard]] const_pointer data() const noexcept { return m_works.data(); }

    /// @brief D2 可视化导出。
    ///
    /// 将图中所有节点及其边以 D2 格式输出。
    /// @param ostream 输出流。
    void dump(std::ostream& ostream) const;


    /// @brief 接管已经创建并绑定到当前 Graph 的 Work。
    ///
    /// @param work 已创建完成的 Work；必须非空且 `work->m_graph == this`。
    /// @return `work`。
    /// @throws std::bad_alloc 节点指针数组扩容失败。
    /// @note 插入失败时由本函数通过 `destroy_work()` 销毁传入节点。
    [[nodiscard]] Work* emplace(Work* work);

    /// @brief 线性查找节点，找到后以 swap-with-last 移除并销毁。
    ///
    /// @param work 要删除的节点。
    ///
    /// 算法步骤：
    /// 1. 断开该节点的所有前驱和后继边（双向同步清理）
    /// 2. 在 m_works 中找到目标位置（线性搜索）
    /// 3. 用最后一个节点覆盖当前节点（swap-with-last）
    /// 4. pop_back + destroy(work)
    ///
    /// DAG 中节点遍历顺序不影响语义，改变物理顺序是安全的。
    ///
    /// @warning 调用方必须确保目标节点未处于执行状态（构建期单线程调用），
    ///          否则直接 destroy 运行中的节点会导致 UAF。
    void erase(Work* const work) noexcept;

    /// @brief 清空所有节点（遍历 m_works 逐个 destroy）。
    ///
    /// @post m_works 为空，所有节点的边表也随之失效。
    void clear() noexcept;

    /// @brief 从图中断开并移出节点，但不销毁它。
    /// @param work 要移出的节点。
    /// @return 成功时返回 work；未找到时返回 nullptr。
    /// @warning 成功后调用方接管所有权，并负责使用匹配的释放路径销毁节点。
    Work* extract(Work* const work) noexcept;

    /// @brief Graph 移动后更新所有 Work::m_graph 回指。
    void rebind() noexcept;

    /// @brief 节点存储 —— 裸指针数组，Graph 独占所有权。
    std::vector<Work*> m_works;

};

// ============================================================================
// Graph 实现
// ============================================================================

inline Graph::Graph(Graph&& other) noexcept
    : m_works{std::exchange(other.m_works, {})} {
    rebind();
}

inline Graph& Graph::operator=(Graph&& other) noexcept {
    if (this != std::addressof(other)) {
        clear();
        m_works = std::exchange(other.m_works, {});
        rebind();
    }
    return *this;
}

inline void Graph::clear() noexcept {
    for (Work* work : m_works) {
        destroy_work(work);
    }
    m_works.clear();
}

inline Graph::~Graph() noexcept {
    clear();
}

inline Work* Graph::emplace(Work* work) {
    TFL_ASSERT(work);
    TFL_ASSERT(work->m_graph == this);
    m_works.push_back(work);
    return work;
}

inline void Graph::erase(Work* const work) noexcept {
    Work* extracted = extract(work);

    if (extracted) {
        destroy_work(extracted);
    }
}

inline Work* Graph::extract(Work* const work) noexcept {
    if (!work || work->m_graph != this) {
        return nullptr;
    }
    auto it = std::ranges::find(m_works, work);
    TFL_ASSERT(it != m_works.end() && "target must exist in m_works");

    work->_clear_predecessors();
    work->_clear_successors();

    if (it != std::prev(m_works.end())) {
        *it = m_works.back();
    }
    m_works.pop_back();
    work->m_graph = nullptr;
    return work;
}

inline void Graph::rebind() noexcept {
    for(Work* work : m_works) {
        work->m_graph = this;
    }
}

inline void Graph::dump(std::ostream& os) const {
    static constexpr std::string_view jump_style =
        " {\n"
        "  style.stroke: \"#ef4444\"\n"
        "  style.stroke-width: 2\n"
        "  style.stroke-dash: 5\n"
        "  style.font-size: 14\n"
        "  style.font-color: \"#dc2626\"\n"
        "  style.bold: true\n"
        "}\n";

    static constexpr std::string_view branch_style =
        " {\n"
        "  style.stroke: \"#3b82f6\"\n"
        "  style.stroke-width: 2\n"
        "  style.font-size: 14\n"
        "  style.font-color: \"#2563eb\"\n"
        "  style.bold: true\n"
        "}\n";

    static constexpr std::string_view normal_style =
        ": {style.stroke: \"#6b7280\"}\n";

    for (const Work* work : m_works) {
        work->dump(os);
        os << '\n';
    }

    auto output = std::ostreambuf_iterator<char>{os};

    for (const Work* work : m_works) {
        const TaskType type = work->type();
        std::size_t index = 0;

        for (const Work* successor : work->_successors()) {
            output = std::format_to(
                output,
                "p{:x} -> p{:x}",
                reinterpret_cast<std::uintptr_t>(work),
                reinterpret_cast<std::uintptr_t>(successor)
                );

            switch (type) {
            case TaskType::Jump:
            case TaskType::MultiJump:
                os << ": " << index++ << jump_style;
                break;

            case TaskType::Branch:
            case TaskType::MultiBranch:
                os << ": " << index++ << branch_style;
                break;

            default:
                os << normal_style;
                break;
            }
        }
    }
}

}  // namespace tfl
