#pragma once

#include <cstdint>
#include <compare>
#include <string>
#include <ostream>
#include <format>

#include "core/executor.hpp"

namespace tfl {

// 宏定义保留给预处理器指令使用 (#ifdef)
#define TASKFLOWLITE_VERSION_MAJOR 0
#define TASKFLOWLITE_VERSION_MINOR 1
#define TASKFLOWLITE_VERSION_PATCH 0
  
/**
 * @brief A struct used to store a version number.
 */
struct Version {
    std::uint32_t major;
    std::uint32_t minor;
    std::uint32_t patch;

    constexpr Version(std::uint32_t maj, std::uint32_t min, std::uint32_t pat) noexcept
        : major{maj}, minor{min}, patch{pat} {}

    constexpr std::strong_ordering operator<=>(const Version&) const = default;

    [[nodiscard]] std::string to_string() const {
        return std::format("{}.{}.{}", major, minor, patch);
    }

    friend std::ostream& operator<<(std::ostream& stream, const Version& ver) {
        return stream << ver.major << '.' << ver.minor << '.' << ver.patch;
    }
};

// 使用 inline constexpr 替代 static 实例，避免每个编译单元都产生符号
inline constexpr Version version(
    TASKFLOWLITE_VERSION_MAJOR,
    TASKFLOWLITE_VERSION_MINOR,
    TASKFLOWLITE_VERSION_PATCH
    );

} // namespace tfl
