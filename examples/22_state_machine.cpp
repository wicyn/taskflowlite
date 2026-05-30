/// @file 16_state_machine.cpp
/// @brief 演示用 Jump 实现复杂的有限状态机（订单处理流程）。
///
/// 状态机拓扑（自环 / 跨状态跳转 / 终态汇聚）：
///
///        ┌────────────┐
///        │   created  │  ← 入口
///        └─────┬──────┘
///              ▼
///        ┌────────────┐    invalid    ┌────────────┐
///        │  validate  │ ────────────▶ │  rejected  │ → done
///        └─────┬──────┘               └────────────┘
///              │ valid
///              ▼
///        ┌────────────┐    no stock   ┌────────────┐
///        │   reserve  │ ────────────▶ │  backorder │
///        └─────┬──────┘               └─────┬──────┘
///              │ ok                         │ retry
///              │                            ▼
///              │                      [回到 reserve]
///              ▼
///        ┌────────────┐    fail       ┌────────────┐
///        │    pay     │ ────────────▶ │  refund    │ → done
///        └─────┬──────┘               └────────────┘
///              │ ok
///              ▼
///        ┌────────────┐
///        │    ship    │ → done
///        └────────────┘
///
/// 关键点：
///   - Jump 是表达 "状态机迁移" 的合适工具，因为它直接 store(0) 重置后继
///     的 join_counter，绕过常规依赖等待协议；
///   - Jump 边权重为 0，**不参与拓扑环检测**，因此可以建自环 / 跨层跳转；
///   - 用 select_if(predicate) 按名字寻找跳转目标，比写死索引更可读。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <atomic>
#include <random>
#include <string>
#include <thread>
#include <chrono>

// 模拟外部状态：库存 / 支付服务
struct OrderContext {
    std::atomic<int>  stock{0};         // 当前库存（每次 reserve 减 1）
    std::atomic<int>  retry_count{0};   // 缺货重试次数
    std::atomic<bool> validated{false};
    std::atomic<bool> paid{false};
    std::atomic<bool> shipped{false};
    std::atomic<bool> rejected{false};
    std::atomic<bool> refunded{false};
};

int main() {
    std::cout << "=== Example 16: Order Processing State Machine ===\n\n";

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    tfl::Flow flow;

    OrderContext ctx;
    ctx.stock.store(0);   // 故意没库存：触发 backorder 重试

    std::random_device rd;
    std::mt19937 gen(rd());

    // ─────────────────────────────────────────────────────────
    // 1. 状态节点
    // ─────────────────────────────────────────────────────────
    auto created = flow.emplace([] {
        std::cout << "[State] created  → Order created\n";
    }).name("created");

    auto validate = flow.emplace([&ctx](tfl::Branch& br) {
        std::cout << "[State] validate → Validating order fields...\n";
        // 校验通过率 80%
        std::uniform_int_distribution<> dist(0, 9);
        thread_local std::mt19937 g(std::random_device{}());
        if (dist(g) < 8) {
            ctx.validated.store(true);
            br.select(0);   // → reserve（索引 0）
        } else {
            ctx.rejected.store(true);
            br.select(1);   // → rejected（索引 1）
        }
    }).name("validate");

    auto reserve = flow.emplace([&ctx](tfl::Jump& jmp) {
        const int s = ctx.stock.load();
        std::cout << "[State] reserve  → Attempting inventory reserve (current: " << s << ")\n";
        if (s > 0) {
            ctx.stock.fetch_sub(1);
            // 不跳转：走常规依赖路径流向 pay
            jmp.reset();
        } else {
            // 跳到 backorder（select_if 按名字找）
            jmp.select_if([](tfl::TaskView tv) {
                return tv.name() == "backorder";
            });
        }
    }).name("reserve");

    auto backorder = flow.emplace([&ctx](tfl::Jump& jmp) {
        const int n = ctx.retry_count.fetch_add(1) + 1;
        std::cout << "[State] backorder → Out of stock, waiting restock (attempt " << n << ")...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // 第 2 次重试时模拟补货成功
        if (n >= 2) ctx.stock.store(5);

        // 跳回 reserve 重试
        jmp.select_if([](tfl::TaskView tv) {
            return tv.name() == "reserve";
        });
    }).name("backorder");

    auto pay = flow.emplace([&ctx](tfl::Branch& br) {
        std::cout << "[State] pay      → Calling payment service...\n";
        thread_local std::mt19937 g(std::random_device{}());
        std::uniform_int_distribution<> dist(0, 9);
        if (dist(g) < 9) {
            ctx.paid.store(true);
            br.select(0);   // → ship
        } else {
            br.select(1);   // → refund
        }
    }).name("pay");

    auto ship = flow.emplace([&ctx] {
        std::cout << "[State] ship     → Shipping / logistics dispatch\n";
        ctx.shipped.store(true);
    }).name("ship");

    auto rejected = flow.emplace([] {
        std::cout << "[State] rejected → Order rejected\n";
    }).name("rejected");

    auto refund = flow.emplace([&ctx] {
        std::cout << "[State] refund   → Payment failed, initiating refund\n";
        ctx.refunded.store(true);
    }).name("refund");

    auto done = flow.emplace([&ctx] {
        std::cout << "[State] done     → Process complete (validated="
                  << ctx.validated.load() << ", paid=" << ctx.paid.load()
                  << ", shipped=" << ctx.shipped.load()
                  << ", rejected=" << ctx.rejected.load()
                  << ", refunded=" << ctx.refunded.load()
                  << ", retry=" << ctx.retry_count.load() << ")\n";
    }).name("done");

    // ─────────────────────────────────────────────────────────
    // 2. 拓扑连接（顺序很重要：Branch / Jump 用索引/名字定位后继）
    // ─────────────────────────────────────────────────────────
    created.precede(validate);

    // validate 是 Branch：索引 0 = reserve, 索引 1 = rejected
    validate.precede(reserve, rejected);

    // reserve 是 Jump：常规边走 pay；跳转边走 backorder（自环重试）
    reserve.precede(pay, backorder);

    // backorder 是 Jump：跳转边走 reserve（形成 reserve ⇄ backorder 循环）
    backorder.precede(reserve);

    // pay 是 Branch：索引 0 = ship, 索引 1 = refund
    pay.precede(ship, refund);

    // 三个终态汇聚到 done
    ship.precede(done);
    rejected.precede(done);
    refund.precede(done);

    executor.async(flow).wait();

    std::cout << "\n=== State machine execution complete ===\n";
    return 0;
}
