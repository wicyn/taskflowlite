/// @file 35_bounded_queue_overflow.cpp
/// @brief A bounded queue publishes a prefix and hands the remaining range to its caller.
#include "../taskflowlite/core/bounded_queue.hpp"
#include <array>
#include <iostream>
#include <vector>

int main() {
    std::array<int, 6> values{1, 2, 3, 4, 5, 6};
    std::array<int*, 6> pointers;
    for (std::size_t i = 0; i < values.size(); ++i) pointers[i] = &values[i];
    tfl::BoundedQueue<int*, 4> queue;
    std::vector<int*> overflow;
    overflow.reserve(2);
    queue.push(pointers.begin(), pointers.size(), [&](auto first, std::size_t count) {
        overflow.assign(first, first + count);
    });
    if (queue.size() != 4 || overflow.size() != 2 || overflow[0] != pointers[4]) return 1;
    int total = 0;
    while (auto* value = queue.pop()) total += *value; // Owner consumes LIFO.
    for (auto* value : overflow) total += *value;
    // Elements are borrowed; values must outlive all queue and overflow consumers.
    std::cout << "Consumed 4 queued + 2 overflow values; sum = " << total << '\n';
    return total == 21 ? 0 : 1;
}
