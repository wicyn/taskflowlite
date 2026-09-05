/// @file 21_observer_tracing.cpp
/// @brief 演示 TaskObserver 注入任务级 before/after 钩子，实现执行时长追踪。
///
/// TaskObserver 是 TaskflowLite 的 AOP（面向切面）切入点：
///   - on_before(WorkerView) 在每次任务 invoke 之前同步调用；
///   - on_after(WorkerView)  在任务「执行范围闭合」后同步调用；
///   - 多 worker 可能并发回调同一 observer 实例 —— 实现必须线程安全且轻量。
///
/// 与 WorkerHandler 的层级互补：
///   | 钩子接口        | 触发粒度 | 用途                          |
///   |-----------------|----------|-------------------------------|
///   | WorkerHandler   | 线程级   | on_start/on_stop/on_exception |
///   | TaskObserver    | 任务级   | on_before / on_after          |
///
/// 本例同一任务挂两个 observer：一个统计调用次数，一个统计执行时长。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <sstream>
#include <syncstream>
// ─────────────────────────────────────────────────────────────
// Observer 1：调用计数器
// ─────────────────────────────────────────────────────────────
class CallCounter : public tfl::TaskObserver {
public:
    std::atomic<int> calls{0};
    std::atomic<int> exits{0};

    void on_before(tfl::WorkerView) noexcept override {
        calls.fetch_add(1, std::memory_order_relaxed);
    }
    void on_after(tfl::WorkerView) noexcept override {
        exits.fetch_add(1, std::memory_order_relaxed);
    }
};

// ─────────────────────────────────────────────────────────────
// Observer 2：执行时长统计
// ─────────────────────────────────────────────────────────────
class TimingObserver : public tfl::TaskObserver {
    using Clock = std::chrono::steady_clock;
    using TP    = Clock::time_point;

            // 起始时间戳改为「按 observer 实例 + worker 分槽」保存，取代原 static thread_local。
            //
            // 为什么不再用 thread_local：
            //   on_after 现在在「执行范围闭合」后触发；抢占型节点（Runtime/Graph）重入经
            //   _schedule_parent 跨线程调度，on_before 与 on_after 可能落在不同 worker。
            //   全局 thread_local 槽按「当前线程」取值，跨 worker 时会读到别线程的残值。
            //   改为实例级存储，槽位生命周期随 observer 而非随线程，从根上去掉这层耦合。
            //
            // 适用范围（重要）：
            //   下面这套「worker 分槽」对【非抢占型任务】（Basic/Branch/Jump —— on_before
            //   与 on_after 必在同一 worker）是正确且免锁的：同一 worker 上
            //   on_before→invoke→on_after 串行不被打断，不同 run 落到不同 worker 互不干扰。
            //   对【抢占型节点】仍不足以配对：worker 变了，按 worker 取值依然错位；要精确
            //   测抢占型节点的单次时长，需按「单次调用」关联起止，而当前 TaskObserver 接口
            //   未透传调用 token —— 属已知限制。
    std::vector<TP> m_begins;   // size == executor.num_workers()

public:
    std::atomic<std::int64_t> total_ns{0};   // 累计纳秒
    std::atomic<int>          samples{0};

    explicit TimingObserver(std::size_t worker_count)
        : m_begins(worker_count) {}

    void on_before(tfl::WorkerView wv) noexcept override {
        m_begins[wv.id()] = Clock::now();    // 写本 worker 槽，免锁
    }
    void on_after(tfl::WorkerView wv) noexcept override {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            Clock::now() - m_begins[wv.id()]).count();
        total_ns.fetch_add(ns, std::memory_order_relaxed);
        samples.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] double avg_us() const noexcept {
        const int n = samples.load();
        return n == 0 ? 0.0 : static_cast<double>(total_ns.load()) / n / 1000.0;
    }
};

// ─────────────────────────────────────────────────────────────
// Observer 3：详细 trace 日志（演示如何用锁保证 cout 顺序）
// ─────────────────────────────────────────────────────────────
class TraceLogger : public tfl::TaskObserver {
    std::mutex            m_mtx;
    std::vector<std::string> m_log;

public:
    void on_before(tfl::WorkerView wv) noexcept override {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_log.emplace_back("worker#" + std::to_string(wv.id()) + " ENTER");
    }
    void on_after(tfl::WorkerView wv) noexcept override {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_log.emplace_back("worker#" + std::to_string(wv.id()) + " EXIT");
    }

    void dump(std::ostream& os) {
        std::lock_guard<std::mutex> lk(m_mtx);
        for (auto const& l : m_log) os << "  " << l << "\n";
    }
};

// ─────────────────────────────────────────────────────────────
int main() {
    std::osyncstream(std::cout) << "=== Example 21: TaskObserver Performance Tracing ===\n\n";

    tfl::Executor executor(4);
    tfl::Flow flow;

            // 构造一个简单的 fork-join：A → (B, C, D) → E
    auto A = flow.emplace([] {
                     std::this_thread::sleep_for(std::chrono::microseconds(100));
                 }).name("A");

    auto B = flow.emplace([] {
                     std::this_thread::sleep_for(std::chrono::microseconds(200));
                 }).name("B");

    auto C = flow.emplace([] {
                     std::this_thread::sleep_for(std::chrono::microseconds(150));
                 }).name("C");

    auto D = flow.emplace([] {
                     std::this_thread::sleep_for(std::chrono::microseconds(250));
                 }).name("D");

    auto E = flow.emplace([] {
                     std::this_thread::sleep_for(std::chrono::microseconds(50));
                 }).name("E");

    A.precede(B, C, D);
    E.succeed(B, C, D);

            // ─────────────────────────────────────────────────────────
            // 关键：同一任务可挂多个 observer，互不干扰
            // TimingObserver 需要 worker 数量来分槽（槽位上限 == num_workers）
            // ─────────────────────────────────────────────────────────
    const std::size_t nworkers = executor.num_workers();

    auto counter = B.register_observer<CallCounter>();
    auto timing  = B.register_observer<TimingObserver>(nworkers);
    auto tracer  = B.register_observer<TraceLogger>();

            // 给 A/C/D/E 也挂 timing，看哪条路径最慢
    auto t_a = A.register_observer<TimingObserver>(nworkers);
    auto t_c = C.register_observer<TimingObserver>(nworkers);
    auto t_d = D.register_observer<TimingObserver>(nworkers);

            // 重复跑 50 次以累积统计样本
    constexpr std::uint64_t RUNS = 50;
    executor.async(flow, RUNS).wait();

            // ─────────────────────────────────────────────────────────
            // 输出统计结果
            // ─────────────────────────────────────────────────────────
    std::osyncstream(std::cout) << "[Counter]    B invoked " << counter->calls.load()
              << " times, exited " << counter->exits.load() << " times\n";

    std::osyncstream(std::cout) << "[Timing]     A avg = " << t_a->avg_us() << " us\n";
    std::osyncstream(std::cout) << "             B avg = " << timing->avg_us() << " us\n";
    std::osyncstream(std::cout) << "             C avg = " << t_c->avg_us() << " us\n";
    std::osyncstream(std::cout) << "             D avg = " << t_d->avg_us() << " us\n";

    std::osyncstream(std::cout) << "\n[Trace] B recent 10 trace entries:\n";
    {
        std::ostringstream oss;
        tracer->dump(oss);
        // 只打印前 10 行（防刷屏）
        std::istringstream iss(oss.str());
        std::string line;
        for (int i = 0; i < 10 && std::getline(iss, line); ++i) {
            std::osyncstream(std::cout) << line << "\n";
        }
    }

            // ─────────────────────────────────────────────────────────
            // 演示 unregister：动态移除 observer
            // ─────────────────────────────────────────────────────────
    std::osyncstream(std::cout) << "\n--- Unregister counter, run 1 more time ---\n";
    B.unregister_observer(counter);
    executor.async(flow).wait();

    std::osyncstream(std::cout) << "[Counter] After unregister B calls = " << counter->calls.load()
              << " (unchanged, unregister succeeded)\n";

    return 0;
}
