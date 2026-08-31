/// @file taskflowlite.hpp
/// @brief TaskflowLite 主入口头文件，包含版本信息与公共组件。
/// @author wicyn
/// @contact https://github.com/wicyn
/// @date 2026-03-02
/// @license MIT
/// @copyright Copyright (c) 2026 wicyn

#pragma once

#include <compare>
#include <cstdint>
#include <format>
#include <ostream>
#include <string>

#include "core/runtime.hpp"
#include "core/branch.hpp"
#include "core/jump.hpp"
#include "core/executor.hpp"
#include "core/async_task.hpp"
#include "core/task.hpp"
#include "core/flow.hpp"
#include "core/async_future.hpp"
#include "core/work_factory.hpp"
#include "core/task_group.hpp"


// ============================================================================
// 编译期版本宏：版本号的源码定义。
// ============================================================================
#define TASKFLOWLITE_VERSION_MAJOR 2
#define TASKFLOWLITE_VERSION_MINOR 2
#define TASKFLOWLITE_VERSION_PATCH 0

namespace tfl {

/// @brief 表示 TaskflowLite 的语义化版本三元组。
///
/// 该值对象按 major、minor、patch 的顺序比较，并提供点分字符串和流输出；
/// 它只描述版本信息，不关联任何运行时状态。
struct Version {
    std::uint32_t major;   ///< 主版本号。
    std::uint32_t minor;   ///< 次版本号。
    std::uint32_t patch;   ///< 修订号。

    /// @brief 构造语义化版本三元组。
    /// @param maj 主版本号，表示不兼容接口变化。
    /// @param min 次版本号，表示向后兼容的功能变化。
    /// @param pat 修订号，表示向后兼容的问题修复。
    constexpr Version(std::uint32_t maj, std::uint32_t min, std::uint32_t pat) noexcept
        : major{maj}, minor{min}, patch{pat} {}

    /// @brief 按 major、minor、patch 的字典序比较两个版本。
    /// @return 标准三路比较结果；相等性由默认比较同时生成。
    constexpr std::strong_ordering operator<=>(const Version&) const = default;

    /// @brief 格式化为 `"major.minor.patch"` 字符串。
    /// @return 新建的十进制点分版本字符串。
    [[nodiscard]] std::string to_string() const {
        return std::format("{}.{}.{}", major, minor, patch);
    }

    /// @brief 按 `major.minor.patch` 格式写入版本号。
    /// @param stream 目标输出流。
    /// @param ver 要输出的版本值。
    /// @return stream，支持连续插入。
    friend std::ostream& operator<<(std::ostream& stream, const Version& ver) {
        return stream << ver.major << '.' << ver.minor << '.' << ver.patch;
    }
};

/// @brief 框架全局版本实例；该 inline 编译期常量可安全定义在头文件中。
inline constexpr Version version(TASKFLOWLITE_VERSION_MAJOR,
                                 TASKFLOWLITE_VERSION_MINOR,
                                 TASKFLOWLITE_VERSION_PATCH);

}  // namespace tfl
