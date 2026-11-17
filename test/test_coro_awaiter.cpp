/// @brief External operation handoff, token lifetime and cooperative stop.
#include "test_common.hpp"

#include <optional>

namespace {

struct ImmediateOperation {
    bool ready;
    bool notify;
    std::optional<tfl::CoroToken>& saved;
    bool await_ready() const noexcept { return ready; }
    bool await_suspend(tfl::CoroToken token) {
        saved = token;
        if (!notify) return false;
        if (!token.resume() || token.resume()) throw std::runtime_error("invalid inline notification");
        return true;
    }
    int await_resume() const noexcept { return 42; }
};

struct ExternalOperation {
    std::promise<tfl::CoroToken>& registered;
    bool await_ready() const noexcept { return false; }
    bool await_suspend(tfl::CoroToken token) {
        registered.set_value(std::move(token));
        return true;
    }
    void await_resume() const noexcept {}
};

struct ThrowingOperation {
    int stage;
    bool await_ready() const {
        if (stage == 0) throw std::runtime_error("operation error");
        return false;
    }
    bool await_suspend(tfl::CoroToken) const {
        if (stage == 1) throw std::runtime_error("operation error");
        return false;
    }
    void await_resume() const { throw std::runtime_error("operation error"); }
};

} // namespace

TEST_CASE("CoroAwaiter: ready declined and inline-completed operations", "[coro][awaiter]") {
    STATIC_REQUIRE(tfl::CoroOperation<ImmediateOperation>);
    STATIC_REQUIRE_FALSE(tfl::CoroOperation<std::suspend_always>);
    tfl::Executor executor(1);
    const int mode = GENERATE(0, 1, 2);
    std::optional<tfl::CoroToken> saved;
    auto result = executor.async([&](tfl::Coro&) -> tfl::CoroTask<int> {
        ImmediateOperation operation{mode == 0, mode == 2, saved};
        co_return co_await operation;
    });
    REQUIRE(result.get() == 42);
    REQUIRE(saved.has_value() == (mode != 0));
    if (saved) {
        REQUIRE_FALSE(saved->resume());
        REQUIRE(saved->stop_requested());
    }
}

TEST_CASE("CoroAwaiter: each throwing phase unwinds and permits another await", "[coro][awaiter][exception]") {
    tfl::Executor executor(1);
    const int stage = GENERATE(0, 1, 2);
    std::optional<tfl::CoroToken> saved;
    auto result = executor.async([&](tfl::Coro&) -> tfl::CoroTask<int> {
        bool caught = false;
        try {
            co_await ThrowingOperation{stage};
        } catch (const std::runtime_error&) {
            caught = true;
        }
        const int value = co_await ImmediateOperation{false, true, saved};
        co_return caught ? value : 0;
    });
    REQUIRE(result.get() == 42);
}

TEST_CASE("CoroToken: an external wait releases the only worker and invalidates old tokens", "[coro][token]") {
    tfl::Executor executor(1);
    std::promise<tfl::CoroToken> first, second;
    auto first_token = first.get_future();
    auto second_token = second.get_future();
    auto parent = executor.async([&](tfl::Coro& coro) -> tfl::CoroTask<int> {
        // A child call can suspend externally, then return to the root frame.
        co_await coro.call([&](tfl::Coro&) -> tfl::CoroTask<> {
            co_await ExternalOperation{first};
        });
        co_await ExternalOperation{second};
        co_return 42;
    });
    auto old = first_token.get();
    REQUIRE(executor.async([] { return 7; }).get() == 7);
    REQUIRE_FALSE(parent.done());
    REQUIRE(old.resume());
    auto current = second_token.get();
    REQUIRE_FALSE(old.resume());
    REQUIRE(old.stop_requested());
    REQUIRE_FALSE(current.stop_requested());
    REQUIRE(current.resume());
    REQUIRE(parent.get() == 42);
    REQUIRE_FALSE(current.resume());
    REQUIRE(current.stop_requested());
}

TEST_CASE("Coro: suspended join propagates scope errors", "[coro][fork][join][exception]") {
    tfl::Executor executor(1);
    const bool catch_locally = GENERATE(false, true);
    std::promise<tfl::CoroToken> registered;
    auto child_token = registered.get_future();
    std::promise<void> parent_yielded;
    auto suspended = parent_yielded.get_future();
    bool caught = false;
    bool continued = false;
    auto parent = executor.async([&](tfl::Coro& coro) -> tfl::CoroTask<int> {
        co_await coro.silent_fork([&](tfl::Coro&) -> tfl::CoroTask<> {
            co_await ExternalOperation{registered};
            throw std::runtime_error("suspended child error");
        });
        // With one Worker, this marker cannot run until the parent yields at join.
        executor.silent_async([&] { parent_yielded.set_value(); });
        try {
            co_await coro.join();
        } catch (const std::runtime_error& error) {
            if (!catch_locally || std::string_view(error.what()) != "suspended child error") throw;
            caught = true;
        }
        continued = true;
        co_await coro.join();
        auto child = co_await coro.fork([] { return 42; });
        co_await coro.join();
        co_return child.get();
    });

    auto token = child_token.get();
    suspended.get();
    REQUIRE_FALSE(parent.done());
    REQUIRE(token.resume());
    if (catch_locally) {
        REQUIRE(parent.get() == 42);
    } else {
        REQUIRE_THROWS_WITH(parent.get(), "suspended child error");
    }
    REQUIRE(caught == catch_locally);
    REQUIRE(continued == catch_locally);
    executor.wait_for_all();
    REQUIRE(executor.num_topologies() == 0);
}

TEST_CASE("CoroToken: stopping a suspended coroutine still requires completion notification", "[coro][token][stop]") {
    tfl::Executor executor(GENERATE(1, 4));
    std::promise<tfl::CoroToken> registered;
    auto ready = registered.get_future();
    auto parent = executor.async([&](tfl::Coro& coro) -> tfl::CoroTask<bool> {
        co_await ExternalOperation{registered};
        co_return coro.stop_requested();
    });
    auto token = ready.get();
    REQUIRE(parent.request_stop());
    REQUIRE(token.stop_requested());
    REQUIRE_FALSE(parent.done());
    REQUIRE(token.resume());
    REQUIRE(parent.get());
    REQUIRE_FALSE(token.resume());
}
