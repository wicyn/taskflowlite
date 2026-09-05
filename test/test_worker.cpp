/// @file test_worker.cpp
/// @brief WorkerHandler / Worker / WorkerView / Context 的生命周期与只读查询。

#include "test_common.hpp"

namespace {

struct CountingHandler : tfl::WorkerHandler {
    std::atomic<int> started{0}, stopped{0};
    std::atomic<bool> valid{true};
    void on_start(tfl::Worker& worker) noexcept override {
        if (worker.id() >= 4 || worker.queue_size() != 0 || worker.queue_capacity() == 0) valid.store(false);
        started.fetch_add(1);
    }
    void on_stop(tfl::Worker& worker, const std::exception_ptr& exception) noexcept override {
        if (worker.id() >= 4 || exception) valid.store(false);
        stopped.fetch_add(1);
    }
};

}  // namespace

/// @test [worker][handler] 每个 worker 启动/退出各回调一次，handler 必须活得更久。
TEST_CASE("WorkerHandler: one start and stop per worker", "[worker][handler]") {
    CountingHandler handler;
    {
        tfl::Executor executor(handler, 4);
        executor.async([] {}).get();
    }
    REQUIRE(handler.started.load() == 4);
    REQUIRE(handler.stopped.load() == 4);
    REQUIRE(handler.valid.load());
}

/// @test [worker][context] 可变/const Context 返回同一 Executor 和 Worker。
TEST_CASE("Context: const and mutable access share identity", "[worker][context]") {
    tfl::Executor executor(2);
    auto future = executor.async([&](tfl::Runtime& rt) {
        const tfl::Context& context = rt;
        return &rt.executor() == &executor && &context.executor() == &executor &&
               &context.worker() == &rt.worker() && context.worker().id() < 2 &&
               context.worker().queue_capacity() > 0 &&
               context.worker().thread().get_id() == std::this_thread::get_id();
    });
    REQUIRE(future.get());
}

/// @test [worker][executor] 非法 worker 数量被拒绝；空执行器查询计数一致。
TEST_CASE("Executor: worker count validation and idle metrics", "[worker][executor]") {
    REQUIRE_THROWS_AS(tfl::Executor(0), tfl::Exception);
    REQUIRE_THROWS_AS(tfl::Executor(tfl::Notifier::capacity()), tfl::Exception);
    tfl::Executor executor(3);
    executor.wait_for_all();
    REQUIRE(executor.num_workers() == 3);
    REQUIRE(executor.num_queues() > 0);
    REQUIRE(executor.num_topologies() == 0);
    REQUIRE(executor.num_waiters() <= executor.num_workers());
}
