/// @file enums.hpp
/// @brief 定义框架核心枚举类型，包括任务类型与可视化布局方向。
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include <cstdint>
#include <ostream>
#include <string_view>
#include "traits.hpp"

namespace tfl {

// ============================================================================
// TaskType - 任务原子类型
// ============================================================================

/// @brief 描述任务在异步任务图中的功能属性。
///
/// 不同的任务类型决定了调度器（Executor）在任务执行完毕后如何解释返回值，
/// 以及如何激活后续的依赖节点。
enum class TaskType : std::int32_t
{
    None        = 0,   ///< 未定义或空任务
    Basic       = 1,   ///< 标准任务：执行完成后自动激活所有后续节点
    Runtime     = 2,   ///< 运行时任务：允许在执行期间通过 Runtime 接口动态派发新任务
    Branch      = 3,   ///< 分支任务：根据返回值（int）激活特定索引的后续节点
    MultiBranch = 4,   ///< 多路分支：根据返回值（vector<int>）同时激活多个特定后续节点
    Jump        = 5,   ///< 跳转任务：跳转至图中指定的绝对索引节点执行
    MultiJump   = 6,   ///< 多路跳转：同时跳转至多个指定的绝对索引节点
    Graph       = 7    ///< 子图任务：封装了另一个完整的 Taskflow 拓扑
};

/// @brief 将 TaskType 转换为原始字符串常量。
constexpr const char* to_string(TaskType type) noexcept {
    switch (type) {
    case TaskType::None:        return "none";
    case TaskType::Basic:       return "basic";
    case TaskType::Runtime:     return "runtime";
    case TaskType::Branch:      return "branch";
    case TaskType::MultiBranch: return "multi_branch";
    case TaskType::Jump:        return "jump";
    case TaskType::MultiJump:   return "multi_jump";
    case TaskType::Graph:       return "graph";
    default:                    return "unknown";
    }
}

/// @brief 将 TaskType 转换为 string_view 以支持高效的非拷贝字符串操作。
constexpr std::string_view to_string_view(TaskType type) noexcept
{
    return to_string(type);
}

namespace impl {
/// @brief 内部元编程辅助：记录 TaskType 枚举的最大数量。
template <>
struct EnumMaxImpl<TaskType>
{
    static constexpr std::int32_t Value = 8;
};
} // namespace impl

/// @brief 支持 std::ostream 标准输出流。
inline std::ostream& operator<<(std::ostream& os, TaskType type)
{
    return os << to_string(type);
}

// ============================================================================
// Direction - 可视化布局方向 (D2/Dot 渲染专用)
// ============================================================================

/// @brief 定义任务图在进行自动布局渲染时的生长方向。
enum class Direction : std::uint8_t
{
    Down    = 0,   ///< 自上而下
    Right   = 1,   ///< 自左向右
    Up      = 2,   ///< 自下而上
    Left    = 3,   ///< 自右向左

    Default = Down ///< 默认采用从上到下的流向
};

/// @brief 将 Direction 转换为字符串。
constexpr const char* to_string(Direction dir) noexcept {
    switch (dir) {
    case Direction::Down:  return "down";
    case Direction::Right: return "right";
    case Direction::Up:    return "up";
    case Direction::Left:  return "left";
    default:               return "down";
    }
}

/// @brief 将 Direction 转换为 string_view。
constexpr std::string_view to_string_view(Direction dir) noexcept
{
    return to_string(dir);
}

namespace impl {
/// @brief 内部元编程辅助：记录 Direction 枚举的最大数量。
template <>
struct EnumMaxImpl<Direction>
{
    static constexpr std::int32_t Value = 4;
};
} // namespace impl

/// @brief 支持 std::ostream 标准输出流。
inline std::ostream& operator<<(std::ostream& os, Direction dir)
{
    return os << to_string(dir);
}

} // namespace tfl

// ============================================================================
// std::format 支持 (C++20+)
// ============================================================================

#if __cpp_lib_format >= 202110L
#include <format>

/// @brief 为 TaskType 注入 std::format 支持。
template <>
struct std::formatter<tfl::TaskType> : std::formatter<std::string_view>
{
    auto format(tfl::TaskType type, std::format_context& ctx) const
    {
        return std::formatter<std::string_view>::format(tfl::to_string_view(type), ctx);
    }
};

/// @brief 为 Direction 注入 std::format 支持。
template <>
struct std::formatter<tfl::Direction> : std::formatter<std::string_view>
{
    auto format(tfl::Direction dir, std::format_context& ctx) const
    {
        return std::formatter<std::string_view>::format(tfl::to_string_view(dir), ctx);
    }
};
#endif
