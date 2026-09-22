/// @file test_shared_work_stack.cpp
/// @brief SharedWorkStack ownership, publication, batches and concurrent consumption.
#include "test_common.hpp"
#include "../taskflowlite/core/shared_work_stack.hpp"
#include <array>
#include <barrier>
#include <unordered_map>

TEST_CASE("SharedWorkStack: empty, batches and node reuse", "[queue][shared][unit]") {
    tfl::SharedWorkStack queue;
    std::array<tfl::Work, 7> nodes;
    std::array<tfl::Work*, 7> pointers;
    for (std::size_t i = 0; i < nodes.size(); ++i) pointers[i] = &nodes[i];
    REQUIRE(queue.empty());
    REQUIRE(queue.size() == 0);
    REQUIRE(queue.steal() == nullptr);

    for (int round = 0; round < 10; ++round) {
        queue.push(pointers[0]);
        queue.push(pointers.begin() + 1, 3);
        queue.push(&nodes[4], &nodes[4], 1); // Public prelinked overload, singleton chain.
        queue.push(pointers.begin() + 5, 2);
        REQUIRE(queue.size() == 7);
        std::set<tfl::Work*> received;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            auto* work = queue.steal();
            REQUIRE(work != nullptr);
            REQUIRE(received.insert(work).second);
        }
        REQUIRE(received == std::set<tfl::Work*>(pointers.begin(), pointers.end()));
        REQUIRE(queue.size() == 0);
        REQUIRE(queue.empty());
        REQUIRE(queue.steal() == nullptr);
        // Successful steal detached m_next, so the same nodes can be published again.
    }
}

TEST_CASE("SharedWorkStack: destruction leaves borrowed Work alive", "[queue][shared][unit]") {
    tfl::Work work;
    {
        tfl::SharedWorkStack queue;
        queue.push(&work);
    }
    REQUIRE(work.empty());
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<tfl::SharedWorkStack>);
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<tfl::SharedWorkStack>);
}

TEST_CASE("SharedWorkStack: multiple producers and thieves publish every payload once",
          "[queue][shared][stress]") {
    constexpr std::size_t count = 8192;
    constexpr std::size_t producers = 4;
    constexpr std::size_t thieves = 4;
    auto nodes = std::make_unique<tfl::Work[]>(count);
    std::vector<tfl::Work*> pointers(count);
    std::unordered_map<tfl::Work*, std::size_t> indexes;
    std::vector<int> payload(count, 0);
    std::vector<std::atomic<unsigned>> seen(count);
    for (std::size_t i = 0; i < count; ++i) {
        pointers[i] = &nodes[i];
        indexes.emplace(pointers[i], i);
    }
    const auto& lookup = indexes; // Only const lookups during concurrent access.
    tfl::SharedWorkStack queue;
    std::atomic<std::size_t> consumed{0};
    std::atomic<bool> error{false};
    std::barrier start(static_cast<std::ptrdiff_t>(producers + thieves));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::vector<std::jthread> threads;
    for (std::size_t p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            start.arrive_and_wait();
            const auto end = (p + 1) * count / producers;
            for (std::size_t i = p * count / producers; i < end; i += 8) {
                // Writes happen after the start barrier; queue publication must expose them.
                for (std::size_t j = i; j < i + 8; ++j) payload[j] = static_cast<int>(j + 1);
                if ((i / 8) % 2 == 0) queue.push(pointers.begin() + i, 8);
                else for (std::size_t j = i; j < i + 8; ++j) queue.push(pointers[j]);
            }
        });
    }
    for (std::size_t t = 0; t < thieves; ++t) {
        threads.emplace_back([&] {
            start.arrive_and_wait();
            while (consumed.load() < count && !error.load()) {
                if (auto* work = queue.steal()) {
                    const auto found = lookup.find(work);
                    if (found == lookup.end()) { error = true; break; }
                    const auto index = found->second;
                    if (seen[index].fetch_add(1) != 0 ||
                        payload[index] != static_cast<int>(index + 1)) error = true;
                    ++consumed;
                } else {
                    // nullptr is transient under contention; never use it as completion.
                    if (std::chrono::steady_clock::now() >= deadline) { error = true; break; }
                    std::this_thread::yield();
                }
            }
        });
    }
    threads.clear(); // All producers/thieves joined before assertions or destruction.
    REQUIRE_FALSE(error.load());
    REQUIRE(consumed.load() == count);
    for (const auto& hits : seen) REQUIRE(hits.load() == 1);
    REQUIRE(queue.empty());
    REQUIRE(queue.size() == 0);
}
