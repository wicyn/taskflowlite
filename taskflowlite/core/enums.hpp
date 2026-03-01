#pragma once
#include <cstdint>
#include <ostream>
#include <string_view>
#include "traits.hpp"
namespace tfl {
// ============================================================================
// TaskType
// ============================================================================
enum class TaskType : std::int32_t
{
    None        = 0,
    Basic       = 1,
    Runtime     = 2,
    Branch      = 3,
    MultiBranch = 4,
    Jump        = 5,
    MultiJump   = 6,
    Graph       = 7
};
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
    }
    return "unknown";
}
constexpr std::string_view to_string_view(TaskType type) noexcept
{
    return to_string(type);
}
namespace impl {
template <>
struct EnumMaxImpl<TaskType>
{
    static constexpr std::int32_t Value = 8;
};
} // namespace impl
inline std::ostream& operator<<(std::ostream& os, TaskType type)
{
    return os << to_string(type);
}

// ============================================================================
// Direction — D2 布局方向
// ============================================================================
enum class Direction : std::uint8_t
{
    Down    = 0,   // direction: down
    Right   = 1,   // direction: right
    Up      = 2,   // direction: up
    Left    = 3,   // direction: left

    Default = Down
};
constexpr const char* to_string(Direction dir) noexcept {
    switch (dir) {
    case Direction::Down:  return "down";
    case Direction::Right: return "right";
    case Direction::Up:    return "up";
    case Direction::Left:  return "left";
    }
    return "down";
}
constexpr std::string_view to_string_view(Direction dir) noexcept
{
    return to_string(dir);
}
namespace impl {
template <>
struct EnumMaxImpl<Direction>
{
    static constexpr std::int32_t Value = 4;
};
} // namespace impl
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
template <>
struct std::formatter<tfl::TaskType> : std::formatter<std::string_view>
{
    auto format(tfl::TaskType type, std::format_context& ctx) const
    {
        return std::formatter<std::string_view>::format(tfl::to_string_view(type), ctx);
    }
};
template <>
struct std::formatter<tfl::Direction> : std::formatter<std::string_view>
{
    auto format(tfl::Direction dir, std::format_context& ctx) const
    {
        return std::formatter<std::string_view>::format(tfl::to_string_view(dir), ctx);
    }
};
#endif
