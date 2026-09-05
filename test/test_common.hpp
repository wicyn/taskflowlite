/// @file test_common.hpp
/// @brief 测试套件公共头 —— 复用 TestEnv 夹具与常用 include。
///
/// @details
/// 几乎所有测试都需要固定线程数的 `Executor`，
/// 把样板抽到本头文件，保持每个测试文件聚焦于业务断言。
///
/// 用法：
/// @code
///   #include "test_common.hpp"
///   using tfl_test::TestEnv;
///   TEST_CASE("...", "[xxx]") {
///       TestEnv env;            // 4 工作线程
///       TestEnv env2(8);        // 显式指定线程数
///       env.executor.async(flow).wait();
///   }
/// @endcode

#pragma once

#include "catch_amalgamated.hpp"
#include "../taskflowlite/taskflowlite.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace tfl_test {

/// @brief 通用测试夹具：N 工作线程 Executor。
///
/// 设计要点：
/// - `Executor` 不可移动，因此 TestEnv 也不可移动 —— 在 TEST_CASE 里按值持有局部变量即可。
/// - 默认 4 worker；偏向 stress 测试时显式构造 `TestEnv(N)` 即可。
/// - 异常由 AsyncFuture 保存；get() 重抛异常，wait() 只负责同步。
struct TestEnv {
    tfl::Executor    executor;         ///< 工作线程池

    TestEnv() : executor(4) {}
    explicit TestEnv(std::size_t n) : executor(n) {}
};

}  // namespace tfl_test
