/// @file taskflowlite.hpp
/// @brief TaskflowLite 主入口头文件，包含版本信息与核心组件包含链。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn
#pragma once

#include <cstdint>
#include <compare>
#include <string>
#include <ostream>
#include <format>

#include "core/executor.hpp"

// ==============================================================================
// 编译期版本宏 (Source of Truth：源码即真理，CMake 会自动来读取这里)
// ==============================================================================
#define TASKFLOWLITE_VERSION_MAJOR 1
#define TASKFLOWLITE_VERSION_MINOR 2
#define TASKFLOWLITE_VERSION_PATCH 2

namespace tfl {

/// @brief 语义化版本号存储结构。
struct Version {
    std::uint32_t major; ///< 主版本号
    std::uint32_t minor; ///< 次版本号
    std::uint32_t patch; ///< 修订号

    constexpr Version(std::uint32_t maj, std::uint32_t min, std::uint32_t pat) noexcept
        : major{maj}, minor{min}, patch{pat} {}

    /// @brief C++20 三路比较运算符
    constexpr std::strong_ordering operator<=>(const Version&) const = default;

    /// @brief 将版本信息格式化为标准字符串
    [[nodiscard]] std::string to_string() const {
        return std::format("{}.{}.{}", major, minor, patch);
    }

    /// @brief 支持 std::ostream 流式输出版本号
    friend std::ostream& operator<<(std::ostream& stream, const Version& ver) {
        return stream << ver.major << '.' << ver.minor << '.' << ver.patch;
    }
};

/// @brief 框架全局版本实例 (inline constexpr 保证 ODR 安全且零开销)
inline constexpr Version version(
    TASKFLOWLITE_VERSION_MAJOR,
    TASKFLOWLITE_VERSION_MINOR,
    TASKFLOWLITE_VERSION_PATCH
    );

} // namespace tfl
