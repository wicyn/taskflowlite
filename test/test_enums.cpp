/// @file test_enums.cpp
/// @brief 版本与枚举测试 —— 转换、未知值、流输出和语义化版本比较。

#include "test_common.hpp"
#include <sstream>

/// @test [enums] 所有有效枚举提供非空标识符，非法值统一返回 unknown。
TEST_CASE("Enums: task types and directions round-trip to text", "[enums]") {
    for (int i = 0; i < tfl::EnumMax<tfl::TaskType>(); ++i) {
        const auto value = static_cast<tfl::TaskType>(i);
        REQUIRE(tfl::to_string_view(value) != "unknown");
        std::ostringstream stream;
        stream << value;
        REQUIRE(stream.str() == tfl::to_string_view(value));
    }
    for (int i = 0; i < tfl::EnumMax<tfl::Direction>(); ++i) {
        const auto value = static_cast<tfl::Direction>(i);
        REQUIRE(tfl::to_string_view(value) != "unknown");
        std::ostringstream stream;
        stream << value;
        REQUIRE(stream.str() == tfl::to_string_view(value));
    }
    REQUIRE(tfl::to_string_view(static_cast<tfl::TaskType>(-1)) == "unknown");
    REQUIRE(tfl::to_string_view(static_cast<tfl::Direction>(255)) == "unknown");
}

/// @test [version] 版本号按 major/minor/patch 比较，并与公开版本宏一致。
TEST_CASE("Version: comparison and formatting", "[version]") {
    STATIC_REQUIRE(tfl::Version{1, 9, 9} < tfl::Version{2, 0, 0});
    STATIC_REQUIRE(tfl::Version{2, 1, 9} < tfl::Version{2, 2, 0});
    STATIC_REQUIRE(tfl::Version{2, 2, 0} < tfl::Version{2, 2, 1});
    REQUIRE(tfl::version.major == TASKFLOWLITE_VERSION_MAJOR);
    REQUIRE(tfl::version.minor == TASKFLOWLITE_VERSION_MINOR);
    REQUIRE(tfl::version.patch == TASKFLOWLITE_VERSION_PATCH);
    std::ostringstream stream;
    stream << tfl::version;
    REQUIRE(stream.str() == tfl::version.to_string());
}
