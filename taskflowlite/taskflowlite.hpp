/// @file taskflowlite.hpp
/// @brief TaskflowLite 主入口头文件，包含版本信息与核心组件包含链。
/// @author WiCyn
/// @contact https://github.com/WiCyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 WiCyn

#pragma once

#include <cstdint>
#include <compare>
#include <string>
#include <ostream>
#include <format>

#include "core/executor.hpp"

namespace tfl {

// Why: 宏定义保留用于预处理器条件编译（例如 #if TASKFLOWLITE_VERSION_MAJOR >= 1）。
// 这允许用户在不实例化 struct 的情况下进行编译时特性检测。
#define TASKFLOWLITE_VERSION_MAJOR 1
#define TASKFLOWLITE_VERSION_MINOR 0
#define TASKFLOWLITE_VERSION_PATCH 0

/// @brief 语义化版本号存储结构。
///
/// 该结构体支持编译期常量构造，并提供完整的比较运算符支持。
/// 遵循 SemVer 规范：主版本号.次版本号.修订号。
struct Version {
    std::uint32_t major; ///< 主版本号：涉及不兼容的 API 变更时递增。
    std::uint32_t minor; ///< 次版本号：以向下兼容的方式引入新功能时递增。
    std::uint32_t patch; ///< 修订号：执行向下兼容的问题修复时递增。

    /// @brief 构造函数。
    /// @param maj 主版本号。
    /// @param min 次版本号。
    /// @param pat 修订号。
    constexpr Version(std::uint32_t maj, std::uint32_t min, std::uint32_t pat) noexcept
        : major{maj}, minor{min}, patch{pat} {}

    /// @brief C++20 三路比较运算符（Spaceship Operator）。
    /// @return 返回版本号之间的强序关系。
    // Why: 显式默认化该运算符，编译器会自动按照成员定义的顺序（major -> minor -> patch）
    // 生成全部六种比较逻辑（<, <=, >, >=, ==, !=），极大地减少了样板代码。
    constexpr std::strong_ordering operator<=>(const Version&) const = default;

    /// @brief 将版本信息格式化为标准字符串（如 "1.2.3"）。
    /// @return 格式化后的 std::string。
    [[nodiscard]] std::string to_string() const {
        return std::format("{}.{}.{}", major, minor, patch);
    }

    /// @brief 支持 std::ostream 流式输出版本号。
    friend std::ostream& operator<<(std::ostream& stream, const Version& ver) {
        return stream << ver.major << '.' << ver.minor << '.' << ver.patch;
    }
};

/// @brief 框架全局版本实例。
// Why: 使用 inline constexpr 替代传统的 static 变量。
// 它可以直接定义在头文件中而不会引发多重定义错误（ODR），且保证在编译单元间只有一份物理拷贝。
inline constexpr Version version(
    TASKFLOWLITE_VERSION_MAJOR,
    TASKFLOWLITE_VERSION_MINOR,
    TASKFLOWLITE_VERSION_PATCH
    );

} // namespace tfl
