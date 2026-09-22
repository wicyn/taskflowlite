/// @file test_spin_mutex.cpp
/// @brief SpinMutex mutual exclusion and try_lock on a different thread.
#include "test_common.hpp"
#include "../taskflowlite/core/spin_mutex.hpp"

TEST_CASE("SpinMutex: try_lock and RAII release", "[mutex][unit]") {
    tfl::SpinMutex mutex;
    bool acquired = true;
    {
        std::lock_guard lock(mutex);
        std::jthread contender([&] {
            acquired = mutex.try_lock();
            if (acquired) mutex.unlock();
        });
    }
    REQUIRE_FALSE(acquired);
    REQUIRE(mutex.try_lock());
    mutex.unlock();
}

TEST_CASE("SpinMutex: concurrent increments are serialized", "[mutex][stress]") {
    tfl::SpinMutex mutex;
    int count = 0;
    std::vector<std::jthread> threads;
    for (int i = 0; i < 4; ++i) threads.emplace_back([&] {
        for (int j = 0; j < 10000; ++j) {
            std::lock_guard lock(mutex);
            ++count;
        }
    });
    threads.clear();
    REQUIRE(count == 40000);
}
