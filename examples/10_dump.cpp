/// @file 10_dump.cpp
/// @brief 演示任务命名与 D2 图形化脚本导出，直观展示框架拓扑结构。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <fstream>

int main() {
    std::cout << "=== Example 10: D2 Dump (Visualization) ===\n";

    tfl::Flow flow;
    flow.name("AI_Inference_Pipeline");

    auto pre_process = flow.emplace([] {}).name("Data_Loader");

    // 构建子图
    tfl::Flow net_flow;
    net_flow.name("Neural_Network");
    auto layer1 = net_flow.emplace([]{}).name("Conv2D");
    auto layer2 = net_flow.emplace([]{}).name("MaxPooling");
    layer1.precede(layer2);

    // 挂载子图
    auto inference = flow.emplace(std::move(net_flow)).name("Inference_Engine");

    // 分支判断
    auto branch = flow.emplace([](tfl::Branch& br){} ).name("Confidence_Check");
    auto pass = flow.emplace([]{}).name("Save_Result");
    auto fail = flow.emplace([]{}).name("Drop_Frame");

    // 拓扑连线
    pre_process.precede(inference);
    inference.precede(branch);
    branch.precede(pass, fail);

    std::cout << "Dumping complex flow to 'pipeline.d2'...\n";
    std::ofstream file("pipeline.d2");
    flow.dump(file);
    file.close();

    std::cout << "\n========== D2 Script Preview ==========\n";
    std::cout << flow.dump();
    std::cout << "=======================================\n";

    std::cout << "\nTo visualize, paste the output at: https://play.d2lang.com\n";
    return 0;
}
