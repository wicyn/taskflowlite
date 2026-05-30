/// @file 19_retry_backoff.cpp
/// @brief 演示 Jump 实现生产级 "错误重试 + 指数退避" 模式。
///
/// 应用场景：调用不稳定的外部服务（HTTP API、消息队列、远端 DB），失败时
/// 不立即重试以免雪崩，而是按指数级时间间隔退避（100ms → 200ms → 400ms ...）。
///
/// 拓扑设计（Jump 自环 + Branch 终态分流）：
///
///     entry ──▶ call ──▶ check ──┬──▶ success      (Branch 索引 0)
///                ▲                │
///                │    backoff     ├──▶ backoff     (Branch 索引 1)
///                └──── jmp(call) ─┘    │
///                                      └──▶ jmp.select_if(name=="call")
///
///     check 是 Branch（决策成功路径还是失败路径）；
///     backoff 是 Jump（睡一段时间后跳回 call 再试一次）。
///
/// 关键设计要点：
///   1. 把 "决策" 和 "等待" 拆成两个节点 —— Branch 不能 sleep（会卡 worker），
///      backoff 节点单独承担睡眠责任，便于做超时控制；
///   2. 重试上限由 retries++ 计数器在 check 里判断，超过上限走 fail 终态；
///   3. 指数退避时间在 backoff 节点里读 retries 计算 100 * 2^k ms。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <random>
#include <string>

int main() {
    std::cout << "=== Example 19: Retry with Exponential Backoff ===\n\n";

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    tfl::Flow flow;
    flow.name("Retry_Backoff");

    // 共享上下文
    std::atomic<int>  attempts{0};      // 当前已尝试次数
    std::atomic<bool> succeeded{false}; // 最终是否成功
    constexpr int MAX_RETRIES = 5;      // 最多重试次数

    // ─────────────────────────────────────────────────────────
    // 1. entry：初始化
    // ─────────────────────────────────────────────────────────
    auto entry = flow.emplace([&attempts] {
        attempts.store(0);
        std::cout << "[entry] Starting external service call\n";
    }).name("entry");

    // ─────────────────────────────────────────────────────────
    // 2. call：模拟外部调用 —— 前 3 次失败，第 4 次成功
    // ─────────────────────────────────────────────────────────
    auto call = flow.emplace([&attempts, &succeeded] {
        const int n = attempts.fetch_add(1) + 1;
        std::cout << "[call]  Attempt " << n << "... ";

        // 模拟前 3 次失败、第 4 次成功
        if (n < 4) {
            std::cout << "FAILED (connection timeout)\n";
            succeeded.store(false);
        } else {
            std::cout << "SUCCESS (HTTP 200)\n";
            succeeded.store(true);
        }
    }).name("call");

    // ─────────────────────────────────────────────────────────
    // 3. check：Branch 决策成功 / 退避重试 / 放弃
    //
    //   Branch 索引：
    //     0 → success   （call 返回成功）
    //     1 → backoff   （失败但还能重试）
    //     2 → fail      （重试次数耗尽）
    // ─────────────────────────────────────────────────────────
    auto check = flow.emplace([&attempts, &succeeded](tfl::Branch& br) {
        if (succeeded.load()) {
            br.select(0);                   // → success
        } else if (attempts.load() >= MAX_RETRIES) {
            std::cout << "[check] Max retries (" << MAX_RETRIES
                      << ") reached, giving up\n";
            br.select(2);                   // → fail
        } else {
            br.select(1);                   // → backoff
        }
    }).name("check");

    // ─────────────────────────────────────────────────────────
    // 4. backoff：Jump 节点 —— 睡眠后跳回 call 再试
    //
    //   退避时长：100 * 2^(attempts-1) ms，封顶 3200ms
    //
    //   注意：sleep 卡的是 backoff 这个节点的 worker，不影响其他任务，
    //         因此 worker 数 ≥ 2 时整体仍能并行（其他任务被偷取调度）。
    // ─────────────────────────────────────────────────────────
    auto backoff = flow.emplace([&attempts](tfl::Jump& jmp) {
        const int n = attempts.load();
        const int ms = std::min(100 << (n - 1), 3200);   // 100, 200, 400, 800...
        std::cout << "[backoff] Backing off " << ms << " ms then retrying\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));

        // 跳回 call 节点（按名字定位）
        jmp.select_if([](tfl::TaskView tv) {
            return tv.name() == "call";
        });
    }).name("backoff");

    // ─────────────────────────────────────────────────────────
    // 5. 终态节点
    // ─────────────────────────────────────────────────────────
    auto success = flow.emplace([&attempts] {
        std::cout << "[success] Service call succeeded (total " << attempts.load()
                  << " attempts)\n";
    }).name("success");

    auto fail = flow.emplace([&attempts] {
        std::cout << "[fail]   Permanently failed (retried " << attempts.load()
                  << " times, manual intervention recommended)\n";
    }).name("fail");

    auto cleanup = flow.emplace([] {
        std::cout << "[cleanup] Releasing connection resources / writing audit log\n";
    }).name("cleanup");

    // ─────────────────────────────────────────────────────────
    // 6. 连边（顺序至关重要）
    // ─────────────────────────────────────────────────────────
    entry.precede(call);
    call.precede(check);

    // check 是 Branch：必须按顺序绑定 0/1/2 → success/backoff/fail
    check.precede(success, backoff, fail);

    // backoff 是 Jump：跳回 call（自环）
    backoff.precede(call);

    // 终态汇聚
    success.precede(cleanup);
    fail.precede(cleanup);

    // ─────────────────────────────────────────────────────────
    // 7. 执行 —— 测两个场景
    // ─────────────────────────────────────────────────────────
    std::cout << "\n--- Scenario 1: Succeeds on 4th try ---\n";
    executor.async(flow).wait();

    std::cout << "\n--- Scenario 2: Eventual failure (modified logic: always fails) ---\n";
    // 重新构造一个新 Flow，模拟永远失败
    {
        tfl::Flow flow2;
        std::atomic<int>  a{0};
        std::atomic<bool> ok{false};

        auto e  = flow2.emplace([&a] { a.store(0); }).name("entry");
        auto c  = flow2.emplace([&a, &ok] {
            a.fetch_add(1);
            ok.store(false);   // 永远失败
            std::cout << "[call]  Attempt " << a.load() << "... FAILED\n";
        }).name("call");
        auto ck = flow2.emplace([&a, &ok](tfl::Branch& br) {
            if (ok.load())               br.select(0);
            else if (a.load() >= 3)      br.select(2);   // 这次只重试 3 次
            else                         br.select(1);
        }).name("check");
        auto bo = flow2.emplace([&a](tfl::Jump& jmp) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50 << (a.load() - 1)));
            jmp.select_if([](tfl::TaskView tv) { return tv.name() == "call"; });
        }).name("backoff");
        auto sx = flow2.emplace([] { std::cout << "[success] Will not execute\n"; }).name("success");
        auto fx = flow2.emplace([&a] {
            std::cout << "[fail]   Bottomed out (total " << a.load() << " attempts)\n";
        }).name("fail");

        e.precede(c);
        c.precede(ck);
        ck.precede(sx, bo, fx);
        bo.precede(c);

        executor.async(flow2).wait();
    }

    return 0;
}
