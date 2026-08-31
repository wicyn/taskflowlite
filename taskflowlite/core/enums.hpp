/// @file enums.hpp
/// @brief 定义任务类型与可视化布局方向枚举。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-05-28
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <cstdint>
#include <ostream>
#include <string_view>
#include "traits.hpp"

namespace tfl {
// ============================================================================
// 枚举工具
// ============================================================================

namespace impl {
template <typename T>
struct EnumMaxImpl;
} // namespace impl

/// @brief 获取框架为枚举类型登记的枚举器数量。
/// @tparam T 具有 `impl::EnumMaxImpl<T>` 特化的枚举类型。
/// @return 对应特化的编译期 `Value`。
template <typename T>
constexpr std::int32_t EnumMax() noexcept {
    return impl::EnumMaxImpl<T>::Value;
}

// ============================================================================
// TaskType 任务类型
// ============================================================================

/// @brief 标识任务节点的调度语义。
enum class TaskType : std::int32_t
{
    None        = 0,   ///< 未指定任务类型。
    Placeholder = 1,   ///< 占位节点：无用户 callable，仅参与依赖传播。
    Basic       = 2,   ///< 执行普通 callable，并通知所有后继。
    Branch      = 3,   ///< 执行单目标条件分支，并通知选中的后继。
    MultiBranch = 4,   ///< 执行多目标条件分支，并通知选中的后继。
    Jump        = 5,   ///< 强制激活一个指定后继。
    MultiJump   = 6,   ///< 强制激活多个指定后继。
    Runtime     = 7,   ///< 通过 `Runtime` 在执行期间派发任务。
    Graph       = 8    ///< 执行封装的子图。
};

/// @brief 将任务类型转换为稳定的英文标识符。
/// @param type 要转换的任务类型。
/// @return 指向静态字符串的指针；未知枚举值返回 `"unknown"`。
constexpr const char* to_string(TaskType type) noexcept {
    switch (type) {
    case TaskType::None:        return "none";
    case TaskType::Placeholder: return "placeholder";
    case TaskType::Basic:       return "basic";
    case TaskType::Branch:      return "branch";
    case TaskType::MultiBranch: return "multi_branch";
    case TaskType::Jump:        return "jump";
    case TaskType::MultiJump:   return "multi_jump";
    case TaskType::Runtime:     return "runtime";
    case TaskType::Graph:       return "graph";
    default:                    return "unknown";
    }
}

/// @brief 将任务类型转换为不拥有字符数据的 string_view。
/// @param type 要转换的任务类型。
/// @return 引用静态字符串的视图，内容与 `to_string(type)` 相同。
constexpr std::string_view to_string_view(TaskType type) noexcept {
    return to_string(type);
}

namespace impl {
/// @brief 为 `TaskType` 登记包含 `None` 在内的有效枚举器数量。
///
/// 该特化只提供编译期常量，不验证任意整数是否为有效枚举值。
template <>
struct EnumMaxImpl<TaskType>
{
    static constexpr std::int32_t Value = 9;
};
} // namespace impl

/// @brief 将任务类型英文标识写入输出流。
/// @param os 目标输出流。
/// @param type 要输出的任务类型。
/// @return os，支持连续插入。
inline std::ostream& operator<<(std::ostream& os, TaskType type) {
    return os << to_string(type);
}

// ============================================================================
// Direction 可视化布局方向
// ============================================================================

/// @brief 定义任务图在进行自动布局渲染时的生长方向。
enum class Direction : std::uint8_t
{
    Down    = 0,   ///< 自上而下。
    Right   = 1,   ///< 自左向右。
    Up      = 2,   ///< 自下而上。
    Left    = 3,   ///< 自右向左。

    Default = Down ///< 默认采用从上到下的流向。
};

/// @brief 将布局方向转换为 D2 使用的英文字符串。
/// @param dir 要转换的方向。
/// @return 指向静态字符串的指针；未知枚举值返回 `"unknown"`。
constexpr const char* to_string(Direction dir) noexcept {
    switch (dir) {
    case Direction::Down:  return "down";
    case Direction::Right: return "right";
    case Direction::Up:    return "up";
    case Direction::Left:  return "left";
    default:               return "unknown";
    }
}

/// @brief 将布局方向转换为不拥有字符数据的 string_view。
/// @param dir 要转换的方向。
/// @return 引用静态字符串的视图。
constexpr std::string_view to_string_view(Direction dir) noexcept {
    return to_string(dir);
}

namespace impl {
/// @brief 为 `Direction` 登记四个独立布局方向的枚举器数量。
///
/// `Default` 是 `Down` 的别名，因此不单独计数。
template <>
struct EnumMaxImpl<Direction>
{
    static constexpr std::int32_t Value = 4;
};
} // namespace impl

/// @brief 将布局方向英文标识写入输出流。
/// @param os 目标输出流。
/// @param dir 要输出的方向。
/// @return os，支持连续插入。
inline std::ostream& operator<<(std::ostream& os, Direction dir) {
    return os << to_string(dir);
}

} // namespace tfl

// ============================================================================
// std::format 支持
// ============================================================================

#if __cpp_lib_format >= 202110L
#include <format>

/// @brief 将 `tfl::TaskType` 适配为按稳定英文标识输出的 `std::formatter`。
///
/// 格式解析和字符串格式化行为复用 `std::formatter<std::string_view>`。
template <>
struct std::formatter<tfl::TaskType> : std::formatter<std::string_view>
{
    auto format(tfl::TaskType type, std::format_context& ctx) const
    {
        return std::formatter<std::string_view>::format(tfl::to_string_view(type), ctx);
    }
};

/// @brief 将 `tfl::Direction` 适配为按 D2 方向标识输出的 `std::formatter`。
///
/// 格式解析和字符串格式化行为复用 `std::formatter<std::string_view>`。
template <>
struct std::formatter<tfl::Direction> : std::formatter<std::string_view>
{
    auto format(tfl::Direction dir, std::format_context& ctx) const
    {
        return std::formatter<std::string_view>::format(tfl::to_string_view(dir), ctx);
    }
};
#endif
