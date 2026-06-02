/// @file taskflowlite.hpp
/// @brief TaskflowLite 主入口头文件，包含版本信息与核心组件包含链。
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

#include "core/work_factory.hpp"
#include "core/runtime.hpp"
#include "core/branch.hpp"
#include "core/jump.hpp"
#include "core/executor.hpp"
#include "core/async_task.hpp"
#include "core/task.hpp"
#include "core/flow.hpp"
#include "core/future.hpp"


// ==============================================================================
// 编译期版本宏 (Source of Truth：源码即真理，CMake 会自动来读取这里)
// ==============================================================================
#define TASKFLOWLITE_VERSION_MAJOR 2
#define TASKFLOWLITE_VERSION_MINOR 0
#define TASKFLOWLITE_VERSION_PATCH 2

namespace tfl {

/// @brief 语义化版本三元组 `{major, minor, patch}`,支持 `<=>` 字典序比较。
///
/// @details
/// 可平凡拷贝的值类型。三路比较按字典序进行,因此 `1.2.3 < 1.10.0` 自然成立。
///
/// @code
/// constexpr Version v{1, 2, 0};
/// static_assert(v < Version{1, 10, 0});
/// std::cout << std::format("{}", v);  // "1.2.0"
/// @endcode
struct Version {
    std::uint32_t major;   ///< 主版本号
    std::uint32_t minor;   ///< 次版本号
    std::uint32_t patch;   ///< 修订号

    constexpr Version(std::uint32_t maj, std::uint32_t min, std::uint32_t pat) noexcept
        : major{maj}, minor{min}, patch{pat} {}

    constexpr std::strong_ordering operator<=>(const Version&) const = default;

    /**
     * @brief 格式化为 `"major.minor.patch"` 字符串。
     */
    [[nodiscard]] std::string to_string() const {
        return std::format("{}.{}.{}", major, minor, patch);
    }

    /// @brief 流输出操作符 —— 格式与 `to_string()` 一致
    friend std::ostream& operator<<(std::ostream& stream, const Version& ver) {
        return stream << ver.major << '.' << ver.minor << '.' << ver.patch;
    }
};

/**
 * @brief 框架全局版本实例，编译期常量，ODR 安全。
 */
inline constexpr Version version(TASKFLOWLITE_VERSION_MAJOR,
                                 TASKFLOWLITE_VERSION_MINOR,
                                 TASKFLOWLITE_VERSION_PATCH);

}  // namespace tfl
