/// @file 15_observer.cpp
/// @brief 演示 TaskObserver —— 在任务执行前后插入自定义逻辑（日志、计时、tracing）。
///
/// 构建: g++ -std=c++23 -O2 -pthread 15_observer.cpp -o 15_observer

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <chrono>
#include <mutex>
#include <string>
#include <iomanip>
#include <memory>
#include <thread>

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
        std::cout << "\n--- Timing Report ---\n";
        for (const auto& r : log) {
            std::cout << "  [" << r.task_name << "]"
                      << " worker=" << r.worker_id
                      << "  " << r.elapsed_ms << " ms\n";
            total += r.elapsed_ms;
        }
        std::cout << "  TOTAL wall-clock contributions: "
                  << total << " ms\n";
        std::cout << "  (Note: parallel tasks' wall-clock times overlap — this is NOT the end-to-end duration)\n";
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

    void on_before(tfl::WorkerView wr) override {
        // Why: 线程本地 start 时间点 —— 不需要锁
        m_start = std::chrono::steady_clock::now();
        m_worker_id = wr.id();
    }

    void on_after(tfl::WorkerView wr) override {
        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - m_start).count();
        // 取当前任务名（on_after 时节点仍存活，m_name 可读）
        m_report.add(m_task_name, m_worker_id, ms);
    }

    // 注入任务名（在 register_observer 之后调用）
    void inject_name(std::string_view name) { m_task_name = name; }

private:
    TimingReport& m_report;
    std::chrono::steady_clock::time_point m_start;
    std::string      m_task_name;
    std::size_t      m_worker_id{};
};

int main() {
    std::cout << "=== Example 15: TaskObserver (Timing) ===\n\n";

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    tfl::Flow flow;
    flow.name("Observed_DAG");

    // 共享报告 —— 所有 observer 实例共用
    TimingReport report;

    auto A = flow.emplace([] {
        volatile int x = 0;
        for (int i = 0; i < 10000000; ++i) x += i;
    }).name("LoadData");

    auto B = flow.emplace([] {
        volatile int x = 0;
        for (int i = 0; i < 5000000; ++i) x += i;
    }).name("Process_A");

    auto C = flow.emplace([] {
        volatile int x = 0;
        for (int i = 0; i < 8000000; ++i) x += i;
    }).name("Process_B");

    auto D = flow.emplace([] {
        volatile int x = 0;
        for (int i = 0; i < 2000000; ++i) x += i;
    }).name("Merge");

    // 为每个任务挂载 observer（各自一个 TimingObserver 实例，共享 report）
    A.register_observer<TimingObserver>(report);
    B.register_observer<TimingObserver>(report);
    C.register_observer<TimingObserver>(report);
    D.register_observer<TimingObserver>(report);

    A.precede(B, C);
    D.succeed(B, C);

    executor.async(flow).wait();

    report.print();

    return 0;
}
