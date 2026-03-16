/// @file example_loop_workflow.cpp
/// @brief 演示如何利用 Jump 节点构建合法的带环拓扑（状态机/循环迭代）

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
// 假设框架内提供了特定的闭包参数类型用于跳转，例如 tfl::Jump&
// 这里根据你在 work.hpp 中定义的 JumpWork 语义进行模拟

void build_training_loop() {
    tfl::Flow flow;

    int current_epoch = 0;
    const int max_epochs = 5;
    double accuracy = 0.0;

    // 1. 初始化任务
    tfl::Task init = flow.emplace([]{
                             std::cout << "[1. Init] Loading dataset and initializing model weights...\n";
                         }).name("Init");

    // 2. 核心训练任务 (被循环的主体)
    tfl::Task train = flow.emplace([&]{
                              current_epoch++;
                              std::cout << "[2. Train] Running Epoch " << current_epoch << "/" << max_epochs << "...\n";
                              accuracy += 0.15; // 模拟精度提升
                          }).name("Train");

    // 3. 评估任务
    tfl::Task evaluate = flow.emplace([&]{
                                 std::cout << "[3. Evaluate] Current Model Accuracy: " << (accuracy * 100) << "%\n";
                             }).name("Evaluate");

    // 4. 条件跳转任务 (这是一个 Jump 类型的节点)
    // 假设 flow.emplace_jump 对应生成 TaskType::Jump 的节点
    tfl::Task check_and_jump = flow.emplace([&](tfl::Jump& jump_ctrl) {
                                       if (accuracy < 0.8 && current_epoch < max_epochs) {
                                           std::cout << "[4. Check] Accuracy not met criteria. Jumping back to Train...\n\n";
                                           // 触发底层的执行器跃迁，重置后续依赖计数，跳回 train 节点
                                           // jump_ctrl.to(train); // 假设的 Jump API
                                       } else {
                                           std::cout << "[4. Check] Training finished! Criteria met or max epochs reached.\n\n";
                                       }
                                   }).name("CheckAndJump");

    // 5. 导出与保存模型
    tfl::Task save_model = flow.emplace([]{
                                   std::cout << "[5. Save] Exporting model to ONNX format...\n";
                               }).name("SaveModel");

    // ========================================================================
    //  拓扑编排 (Topology Orchestration)
    // ========================================================================

    init.precede(train);
    train.precede(evaluate);
    evaluate.precede(check_and_jump);

    // 【核心高光时刻】：闭环连边！
    // 尝试将 check_and_jump 连接回 train，形成: Train -> Evaluate -> Jump -> Train 的环路。
    // 得益于我们在 Work::_can_precede 中写下的 O(1) 短路剪枝逻辑：
    // 当 this_is_jump 为 true 时，这根导致闭环的边会被直接放行，不会抛出死锁异常！
    check_and_jump.precede(train);

    // 正常退出循环的分支
    check_and_jump.precede(save_model);

    // ========================================================================
    //  执行与验证
    // ========================================================================

    std::cout << "=== Starting Loop Workflow ===\n";
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    executor.submit(flow).start().wait();

    // 如果你通过 Task::dump 打印，你会看到生成的 D2 图代码中完美包含了一个后向反馈箭头。
    // std::cout << "\n=== D2 Graph Diagram ===\n" << init.dump() << "\n";
}

int main() {
    build_training_loop();
    return 0;
}
