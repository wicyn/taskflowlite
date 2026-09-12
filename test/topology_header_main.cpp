/// @brief 为 topology.hpp 独立翻译单元提供有效的 Executor。
#include "../taskflowlite/taskflowlite.hpp"

void check_topology_header(tfl::Executor& executor);

int main() {
    tfl::Executor executor(1);
    check_topology_header(executor);
}
