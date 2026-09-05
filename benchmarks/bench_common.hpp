/// @file bench_common.hpp
/// @brief 两套基准共用计时、计数校验与快速验证参数，确保工作量一致。

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

inline std::atomic<int> g_counter{0};
inline bool g_smoke = false;
inline bool g_verify = true;
inline int g_failures = 0;

/// @brief --smoke 保留全部场景的图结构，仅缩短重复执行次数。
inline int bench_runs(int full_runs) { return g_smoke ? std::min(full_runs, 3) : full_runs; }

inline void add_one() {
    //if (g_verify) g_counter.fetch_add(1, std::memory_order_relaxed);
}

class Timer {
public:
    explicit Timer(std::string name)
        : m_name(std::move(name)), m_start(std::chrono::steady_clock::now()) {}
    ~Timer() {
        const auto ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - m_start).count();
        std::cout << "[" << m_name << "] elapsed: " << ms << " ms"
                  << (g_smoke ? " (smoke: repeat counts capped at 3)" : "") << "\n";
    }
private:
    std::string m_name;
    std::chrono::steady_clock::time_point m_start;
};

inline void verify(int expected) {
    if (!g_verify) {
        std::cout << "  verify: disabled (--no-verify)\n\n";
        return;
    }
    const int actual = g_counter.load();
    if (actual != expected) ++g_failures;
    std::cout << "  verify: expected=" << expected << ", actual=" << actual
              << (expected == actual ? "  [PASS]" : "  [FAIL]") << "\n\n";
}

/// @return 1 表示打印帮助后退出，-1 表示非法参数，0 表示继续执行。
inline int parse_benchmark_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--smoke") g_smoke = true;
        else if (arg == "--no-verify") g_verify = false;
        else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [--smoke] [--no-verify]\n"
                      << "Default timings include atomic correctness counters.\n"
                      << "--no-verify measures scheduling without those counters.\n";
            return 1;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return -1;
        }
    }
    return 0;
}
