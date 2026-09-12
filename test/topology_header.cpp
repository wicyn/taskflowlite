/// @file topology_header.cpp
/// @brief 独立包含 Topology 时必须能够构造和销毁，不依赖其它翻译单元。

#include "../taskflowlite/core/topology.hpp"

namespace {
void destroy_topology(tfl::Topology* topology) {
    delete topology;
}
} // namespace

void check_topology_header(tfl::Executor& executor) {
    // 本翻译单元只包含 topology.hpp；Executor 的完整类型由独立驱动提供。
    void (*volatile destroy)(tfl::Topology*) = destroy_topology;
    destroy(new tfl::Topology(executor));
}
