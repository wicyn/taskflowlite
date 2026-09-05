/// @file 18_recursive_runtime.cpp
/// @brief 演示嵌套 Runtime 递归任务 —— 经典并行 fibonacci。
///
/// 这是 Runtime 最有威力也最危险的用法：
///   一个 Runtime 任务在执行期间通过 rt.async() 派发自己的 "子任务"，
///   子任务又是 Runtime 任务，可以递归再派发，形成一棵动态计算树。
///
/// 核心机制：
///   1. rt.async(F)   返回 std::future<R>，可拿计算结果；
///   2. rt.wait()   协作式等齐当前 Runtime 派发的所有子任务；
///   3. rt.wait_until(pred)
///                    协作式等到谓词成真。**期间 worker 不阻塞睡眠，会主动
///                    窃取其他任务执行** —— 这是避免死锁的关键。
///
/// 为什么不会死锁？
///   传统 std::async 在 future.get() 时会阻塞调用线程。当线程数有限、递归深
///   时，所有线程都可能卡在等子任务，子任务却没人执行 → 死锁。
///   TaskflowLite 的协作式等待让等子任务的线程同时也是干活的线程，从根上
///   消除了这个风险。
///
/// 注意：fib 这种小粒度递归任务，框架开销远大于计算本身，仅作示意；
///       实际中适合粒度更大的递归算法（并行 quicksort、树搜索、
///       N-body 模拟的 octree build 等）。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <atomic>
#include <chrono>
#include <future>
#include <syncstream>
// ─────────────────────────────────────────────────────────
// 递归 fibonacci：阈值以下转串行，避免任务过细
// ─────────────────────────────────────────────────────────
constexpr int CUTOFF = 16;   // n < CUTOFF 时直接串行

static std::int64_t fib_serial(int n) noexcept {
    if (n < 2) return n;
    return fib_serial(n - 1) + fib_serial(n - 2);
}

// 并行版本：注意签名是 (int n, tfl::Runtime& rt) → 自动识别为 Runtime 任务
static std::int64_t fib_parallel(int n, tfl::Runtime& rt) {
    if (n < CUTOFF) return fib_serial(n);

    // 派发 fib(n-1) 和 fib(n-2) 到任意空闲 worker
    auto fa = rt.async([n](tfl::Runtime& sub) { return fib_parallel(n - 1, sub); });
    auto fb = rt.async([n](tfl::Runtime& sub) { return fib_parallel(n - 2, sub); });

    // 协作式等齐 —— 期间本 worker 会窃取其他任务执行，绝不会死锁
    rt.wait_until([&]() noexcept {
        using namespace std::chrono_literals;
        return fa.done()
            && fb.done();
    });

    return fa.get() + fb.get();
}

int main() {
    std::osyncstream(std::cout) << "=== Example 18: Recursive Runtime (Parallel Fibonacci) ===\n\n";

    tfl::Executor executor(std::thread::hardware_concurrency());

    constexpr int N = 35;

    // ─────────────────────────────────────────────────────────
    // 方式 A：通过 Executor::async 直接拿到 future
    // ─────────────────────────────────────────────────────────
    {
        std::osyncstream(std::cout) << "--- Method A: executor.async(runtime_task) ---\n";

        using Clock = std::chrono::steady_clock;
        const auto t0 = Clock::now();
        auto fut = executor.async([N](tfl::Runtime& rt) {
            return fib_parallel(N, rt);
        });
        const auto result = fut.get();
        const auto t1 = Clock::now();

        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            t1 - t0).count();
        std::osyncstream(std::cout) << "  fib(" << N << ") = " << result
                  << "  (parallel elapsed " << ms << " ms)\n";
    }

    // ─────────────────────────────────────────────────────────
    // 方式 B：在 Flow 节点里用 Runtime 启动递归
    // ─────────────────────────────────────────────────────────
    {
        std::osyncstream(std::cout) << "\n--- Method B: Embedded in Flow node ---\n";

        std::atomic<std::int64_t> answer{0};
        tfl::Flow flow;

        auto root = flow.emplace([&answer](tfl::Runtime& rt) {
            answer.store(fib_parallel(N, rt));
        }).name("fib_root");

        auto report = flow.emplace([&answer, N] {
            std::osyncstream(std::cout) << "  [report] fib(" << N << ") = " << answer.load() << "\n";
        }).name("report");

        root.precede(report);

        executor.async(flow).wait();
    }

    // ─────────────────────────────────────────────────────────
    // 方式 C：silent_async fire-and-forget，不取结果
    //          适合 "我只想并行做完，不关心返回值" 的场景
    // ─────────────────────────────────────────────────────────
    {
        std::osyncstream(std::cout) << "\n--- Method C: silent_async fan-out + implicit join ---\n";

        std::atomic<int> done{0};
        tfl::Flow flow;

        flow.emplace([&done](tfl::Runtime& rt) {
            for (int i = 0; i < 8; ++i) {
                rt.silent_async([&done, i] {
                    // 模拟独立的并行计算
                    std::int64_t r = fib_serial(20 + (i % 4));
                    done.fetch_add(1);
                    std::osyncstream(std::cout) << "    silent_async#" << i << " -> " << r << "\n";
                });
            }
            // 不调用 wait —— 框架会在 invoke 返回后自动等齐子任务
        });

        executor.async(flow).wait();
        std::osyncstream(std::cout) << "  [verify] All silent_async subtasks complete: "
                  << done.load() << "/8\n";
    }

    // 串行 baseline 用于结果验证
    const auto baseline = fib_serial(N);
    std::osyncstream(std::cout) << "\n[Verify] fib_serial(" << N << ") = " << baseline << "\n";

    return 0;
}
