/// @file 15_observer.cpp
/// @brief 演示 TaskObserver —— 在任务执行前后插入自定义逻辑（日志、计时、tracing）。
///
/// 构建（Linux/GCC）:
/// g++ -std=c++20 -O2 -pthread 15_observer.cpp -o 15_observer -latomic

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <syncstream>

/// @brief 共享报告存储 —— 多个 observer 实例并发写入，由 mutex 保护。
struct TimingReport {
    struct Record {
        std::string task_name;
        std::size_t worker_id;
        double      elapsed_ms;
    };

    mutable std::mutex mtx;
    std::vector<Record> log;

    void add(std::string_view name, std::size_t wid, double ms) {
        std::lock_guard lk(mtx);
        log.push_back({std::string(name), wid, ms});
    }

    void print() const {
        std::lock_guard lk(mtx);
        double total = 0;

        std::osyncstream(std::cout) << "\n--- Timing Report ---\n";
        for (const auto& r : log) {
            std::osyncstream(std::cout) << "  [" << r.task_name << "]"
                      << " worker=" << r.worker_id
                      << "  " << r.elapsed_ms << " ms\n";
            total += r.elapsed_ms;
        }

        std::osyncstream(std::cout) << "  TOTAL wall-clock contributions: "
                  << total << " ms\n";
        std::osyncstream(std::cout) << "  (Note: parallel tasks' wall-clock times overlap — this is NOT the end-to-end duration)\n";
    }
};

/// @brief 计时 Observer：记录任务开始/结束时间并写入共享报告。
///
/// 每个 register_observer<TimingObserver>(report) 构造一个新实例，
/// 所有实例共享同一个 TimingReport 引用。
class TimingObserver : public tfl::TaskObserver {
public:
    explicit TimingObserver(TimingReport& report) noexcept
        : m_report(report) {}

    void on_before(tfl::WorkerView wr) noexcept override {
        // 每个任务使用独立实例，本例不会并发复用同一个 Observer。
        m_start = std::chrono::steady_clock::now();
        m_worker_id = wr.id();
    }

    void on_after(tfl::WorkerView) noexcept override {
        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - m_start).count();

                // 使用任务启动前注入的名称。
        m_report.add(m_task_name, m_worker_id, ms);
    }

            // 注入任务名（在 register_observer 之后、任务启动之前调用）。
    void inject_name(std::string_view name) {
        m_task_name = name;
    }

private:
    TimingReport& m_report;
    std::chrono::steady_clock::time_point m_start{};
    std::string m_task_name;
    std::size_t m_worker_id{};
};

int main() {
    std::osyncstream(std::cout) << "=== Example 15: TaskObserver (Timing) ===\n\n";

    tfl::Executor executor(4);

            // 共享报告先于 Flow 创建，保证其生命周期覆盖所有 Observer。
    TimingReport report;

    tfl::Flow flow;
    flow.name("Observed_DAG");

            // 使用 64 位累计值，避免大量累加导致有符号整数溢出。
            // volatile 仅用于保留模拟计算负载，不用于线程同步。
            // 使用普通赋值，避免 C++20 对 volatile 复合赋值的弃用警告。
    auto A = flow.emplace([] {
                     volatile std::int64_t x = 0;
                     for (int i = 0; i < 10000000; ++i) {
                         x = x + i;
                     }
                 }).name("LoadData");

    auto B = flow.emplace([] {
                     volatile std::int64_t x = 0;
                     for (int i = 0; i < 5000000; ++i) {
                         x = x + i;
                     }
                 }).name("Process_A");

    auto C = flow.emplace([] {
                     volatile std::int64_t x = 0;
                     for (int i = 0; i < 8000000; ++i) {
                         x = x + i;
                     }
                 }).name("Process_B");

    auto D = flow.emplace([] {
                     volatile std::int64_t x = 0;
                     for (int i = 0; i < 2000000; ++i) {
                         x = x + i;
                     }
                 }).name("Merge");

            // 为每个任务挂载 Observer（各自一个实例，共享 report）。
    A.register_observer<TimingObserver>(report)->inject_name(A.name());
    B.register_observer<TimingObserver>(report)->inject_name(B.name());
    C.register_observer<TimingObserver>(report)->inject_name(C.name());
    D.register_observer<TimingObserver>(report)->inject_name(D.name());

    A.precede(B, C);
    D.succeed(B, C);

            // 等待图完成，同时传播任务异常。
    executor.async(flow).get();
    executor.wait_for_all();

    report.print();

    return 0;
}
