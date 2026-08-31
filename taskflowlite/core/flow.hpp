/// @file flow.hpp
/// @brief DAG 构建器 —— 用户层任务图定义入口。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-07-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cinttypes>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <format>
#include <functional>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "d2_render.hpp"
#include "flow_builder.hpp"
#include "work_factory.hpp"

namespace tfl {

/// @brief 表示可构建、命名、可视化并提交执行的静态 DAG 所有者。
///
/// `Flow` 独占底层 `Graph` 及其全部 `Work` 节点；由构建接口返回的 `Task` 只
/// 借用节点。类型不可复制，移动会转移节点所有权并重绑节点的图回指。
///
/// @warning 本类型不提供结构修改同步；图执行期间不得移动、修改、清空或销毁。
class Flow : public FlowBuilder {
    friend class Work;
    friend class Executor;
    friend class Task;
    friend class Runtime;

    using Builder = FlowBuilder;

public:
    /// @brief 构造名称为空且不包含节点的任务图。
    Flow() noexcept;

    /// @brief 构造带名称的空任务图。
    /// @tparam S 可构造 `std::string` 的名称类型。
    /// @param name 图的诊断和可视化名称。
    template <typename S>
        requires std::constructible_from<std::string, S>
    explicit Flow(S&& name);

    /// @brief Flow 独占 Work 节点，因此禁止复制构造。
    Flow(const Flow&) = delete;

    /// @brief Flow 独占 Work 节点，因此禁止复制赋值。
    Flow& operator=(const Flow&) = delete;

    /// @brief 接管另一 Flow 的节点、名称并重绑所有 `Work::m_graph` 回指。
    /// @pre other 当前没有被 Executor 执行或引用。
    /// @post other 变为空图但仍可继续使用。
    Flow(Flow&& other) noexcept;

    /// @brief 清空当前图后接管另一 Flow，并重绑节点的图回指。
    /// @return `*this`。
    /// @pre 两个 Flow 均未被 Executor 执行或引用。
    Flow& operator=(Flow&& other) noexcept;

    /// @brief 销毁全部节点并释放图存储。
    /// @pre Flow 不得仍被 Executor 执行或引用。
    ~Flow() noexcept = default;

    /// @brief 将当前任务图导出为 D2 描述字符串。
    /// @param direction D2 布局方向。
    /// @return 包含图容器、节点和边的完整 D2 文本。
    /// @note 不得与图结构或名称修改并发调用。
    [[nodiscard]] std::string dump(Direction direction = Direction::Default) const;

    /// @brief 将当前任务图的 D2 描述写入输出流。
    /// @param ostream 接收 D2 文本的输出流。
    /// @param direction D2 布局方向。
    /// @note 不得与图结构或名称修改并发调用。
    void dump(std::ostream& ostream, Direction direction = Direction::Default) const;

    /// @brief 设置当前任务图名称。
    /// @tparam S 可构造 `std::string` 的名称类型。
    /// @param name 新名称。
    /// @return `*this`。
    /// @note 不得与图执行、移动或其他名称修改并发调用。
    template <typename S>
        requires std::constructible_from<std::string, S>
    Flow& name(S&& name);

    /// @brief 返回当前任务图名称。
    /// @return 视图在下次修改名称、移动或销毁 Flow 前有效。
    [[nodiscard]] std::string_view name() const noexcept;

protected:
    /// @brief Flow 独占的任务图物理存储。
    Graph m_graph;

    /// @brief 调试和可视化名称。
    std::string m_name;
};

// ============================================================================
// 构造、移动和析构
// ============================================================================

inline Flow::Flow() noexcept
    : Builder{m_graph}
    , m_graph{} {
}

template <typename S>
    requires std::constructible_from<std::string, S>
inline Flow::Flow(S&& name)
    : Builder{m_graph}
    , m_graph{}
    , m_name{std::forward<S>(name)} {
}

inline Flow::Flow(Flow&& other) noexcept
    : Builder{m_graph}
    , m_graph{std::move(other.m_graph)}
    , m_name{std::move(other.m_name)} {
}

inline Flow& Flow::operator=(Flow&& other) noexcept {
    if (this != std::addressof(other)) {
        m_graph = std::move(other.m_graph);
        m_name = std::move(other.m_name);
    }

    return *this;
}

// ============================================================================
// D2 可视化
// ============================================================================

inline std::string Flow::dump(Direction direction) const {
    std::ostringstream stream;
    dump(stream, direction);
    return stream.str();
}

inline void Flow::dump(std::ostream& os, Direction dir) const {
    os << "direction: " << to_string(dir) << "\n\n";
    os << "root: |md\n  <center>";

    if (m_name.empty()) {
        const std::string id = std::format("p{:x}", reinterpret_cast<std::uintptr_t>(std::addressof(m_graph)));
        D2Renderer::write_html_escaped(os, id);
    } else {
        D2Renderer::write_html_escaped(os, m_name);
    }

    os << "<br/><span style=\"color: #6b7280;\">[ "
       << to_string(TaskType::Graph)
       << " ]</span></center>\n"
          "| {\n"
          "  shape: rectangle\n"
          "  label.near: top-center\n"
          "  style.fill: \"#e8f5e9\"\n"
          "  style.stroke: \"#10b981\"\n"
          "  style.stroke-width: 2\n"
          "  style.border-radius: 14\n\n";

    m_graph.dump(os);
    os << "}\n";
}

// ============================================================================
// 名称
// ============================================================================

template <typename S>
    requires std::constructible_from<std::string, S>
inline Flow& Flow::name(S&& name) {
    m_name = std::string{std::forward<S>(name)};
    return *this;
}

inline std::string_view Flow::name() const noexcept {
    return m_name;
}


// ============================================================================
// 输出流
// ============================================================================

/// @brief 将 Flow 的完整 D2 描述写入输出流。
/// @param ostream 目标输出流。
/// @param flow 要导出的任务图。
/// @return ostream，支持连续插入。
inline std::ostream& operator<<(std::ostream& ostream, const Flow& flow) {
    flow.dump(ostream);
    return ostream;
}

} // namespace tfl

namespace std {

/// @brief 按底层 `Graph` 对象身份计算 `tfl::Flow` 的标准哈希值。
///
/// 哈希不读取图内容，因此仅在同一 `Flow` 对象身份保持不变时具有稳定含义。
template <>
struct hash<tfl::Flow> {
    /// @brief 计算 Flow 图对象地址对应的哈希值。
    /// @param flow 要哈希的 Flow。
    /// @return `flow.hash_value()`。
    [[nodiscard]] std::size_t operator()(const tfl::Flow& flow) const noexcept {
        return flow.hash_value();
    }
};

} // namespace std
