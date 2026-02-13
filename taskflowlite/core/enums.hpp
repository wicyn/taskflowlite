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
    Jump        = 4,
    Graph       = 5
};

constexpr const char* to_string(TaskType type) noexcept
{
    switch (type) {
    case TaskType::None:        return "none";
    case TaskType::Basic:       return "basic";
    case TaskType::Runtime:     return "runtime";
    case TaskType::Branch:      return "branch";
    case TaskType::Jump:        return "jump";
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
    static constexpr std::int32_t Value = 6;
};
} // namespace impl

inline std::ostream& operator<<(std::ostream& os, TaskType type)
{
    return os << to_string(type);
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
#endif
