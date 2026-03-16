/// @file 01_basic_dag.cpp
/// @brief 演示最基础的 DAG (有向无环图) 构建，展示 Lambda 捕获与 emplace 传参的混用。

#include "../taskflowlite/taskflowlite.hpp"
#include <iostream>
#include <string>

int main156() {
    std::cout << "=== Example 01: Basic DAG ===\n";

    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);
    tfl::Flow flow;
    flow.name("Basic_DAG_Flow");

    std::string shared_context = "Init_Done";

    // 方式 1：Lambda 引用捕获 [&] (最符合直觉的 C++ 写法)
    auto A = flow.emplace([&shared_context] {
        std::cout << "Task A (Init). Context: " << shared_context << "\n";
        shared_context = "Process_Ready";
        }).name("Task_A");

    // 方式 2：emplace 参数转发 (框架负责拷贝参数)
    auto B = flow.emplace([](int id) {
        std::cout << "Task B (Process " << id << ")\n";
        }, 1).name("Task_B");

    // 方式 3：Lambda 值捕获与初始化捕获 [x = ...]
    auto C = flow.emplace([id = 2, &shared_context] {
        std::cout << "Task C (Process " << id << "). Context: " << shared_context << "\n";
        }).name("Task_C");

    // 方式 4：混合使用
    auto D = flow.emplace([&shared_context](const std::string& msg) {
        std::cout << "Task D (Merge): " << msg << " | Final Context: " << shared_context << "\n";
        }, "All processes done!").name("Task_D");

    // 构建依赖关系：A -> (B, C) -> D
    A.precede(B, C);
    D.succeed(B, C);

    executor.submit(flow).start().wait();

    std::cout << "All tasks completed!\n";
    return 0;
}

int main245254() {
    // 1. 初始化异常处理器和执行器（分配 4 个工作线程）
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, 4);

    std::atomic<int> step{ 0 };

    // 2. 提交独立的异步任务
    auto t1 = executor.submit([&] {
        assert(step.load() == 0);
        std::cout << "Task 1 正在执行... (预期 step=0, 当前 step=" << step.load() << ")" << std::endl;
        step.store(1);
        });

    auto t2 = executor.submit([&] {
        assert(step.load() == 1);
        std::cout << "Task 2 正在执行... (预期 step=1, 当前 step=" << step.load() << ")" << std::endl;
        step.store(2);
        });

    auto t3 = executor.submit([&] {
        assert(step.load() == 2);
        std::cout << "Task 3 正在执行... (预期 step=2, 当前 step=" << step.load() << ")" << std::endl;
        step.store(3);
        });

    std::cout << "--- start ---" << std::endl;

    // 3. 逆序组装并启动：t3 依赖 t2，t2 依赖 t1
    t3.start(t2);
    std::cout << "--- start2 ---" << std::endl;
    t2.start(t1);

    std::cout << "--- start3 ---" << std::endl;
    // 触发点：启动最源头的任务 t1
    t1.start();

    std::cout << "--- start5 ---" << std::endl;
    // 4. 等待执行完成
    t3.wait();
    std::cout << "--- start6 ---" << std::endl;
    executor.wait_for_all();

    // 5. 验证最终结果
    assert(step.load() == 3);
    std::cout << "--- end step = " << step.load() << " ---" << std::endl;

    return 0;
}



/// @file 13_complex_all_forms.cpp
/// @brief 极致复杂示例，展示 taskflowlite 全部 API 形式
///
/// 包含：
/// - 7种任务类型 (Basic, Runtime, Branch, MultiBranch, Jump, MultiJump, Subflow)
/// - 多层嵌套子流程
/// - 信号量资源控制
/// - Jump 状态机循环
/// - Branch/MultiBranch 条件分支
/// - Runtime 动态调度
/// - 多个 Flow 之间的依赖启动
/// - D2 可视化导出

#include <iostream>
#include <fstream>
#include <atomic>
#include <string>
#include <random>

int main() {
    std::cout << "=== Example 13: Complex All Forms ===\n\n";

    // ═══════════════════════════════════════════════════════════════════════
    // 0. 初始化
    // ═══════════════════════════════════════════════════════════════════════
    tfl::ResumeNever handler;
    tfl::Executor executor(handler, std::thread::hardware_concurrency());

    tfl::Semaphore data_sem(2);
    tfl::Semaphore gpu_sem(1);
    tfl::Semaphore io_sem(3);

    std::atomic<int> epoch{ 0 };
    std::atomic<int> global_counter{ 0 };
    std::atomic<bool> stop_flag{ false };

    std::random_device rd;
    std::mt19937 gen(rd());

    // ═══════════════════════════════════════════════════════════════════════
    // 1. Flow_A: 数据处理管道（多层嵌套 + Semaphore + Runtime动态提交）
    // ═══════════════════════════════════════════════════════════════════════
    std::cout << "--- Building Flow_A: Data Pipeline ---\n";
    {
        tfl::Flow flow_a;
        flow_a.name("Data_Pipeline");

        // A1. Basic Task (带参数)
        auto a1 = flow_a.emplace([](std::string& cfg) {
            std::cout << "  [A1] Load config: " << cfg << "\n";
            }, std::string("v2.0")).name("A1_LoadConfig");

        // A2. Subflow (循环3次, 带Semaphore)
        tfl::Flow stage1_sub;
        stage1_sub.name("Stage1_Preprocess");
        auto s1_t1 = stage1_sub.emplace([] {
            std::cout << "    [S1_1] Load data\n";
            });
        auto s1_t2 = stage1_sub.emplace([](int id) {
            std::cout << "    [S1_" << id << "] Process batch\n";
            }, 1);
        auto s1_t3 = stage1_sub.emplace([](int id) {
            std::cout << "    [S1_" << id << "] Process batch\n";
            }, 2);
        auto s1_t4 = stage1_sub.emplace([](int id) {
            std::cout << "    [S1_" << id << "] Process batch\n";
            }, 3);
        auto s1_merge = stage1_sub.emplace([] {
            std::cout << "    [S1_Merge] Combine results\n";
            });
        s1_t1.precede(s1_t2, s1_t3, s1_t4);
        s1_t2.precede(s1_merge);
        s1_t3.precede(s1_merge);
        s1_t4.precede(s1_merge);

        auto a2 = flow_a.emplace(std::move(stage1_sub), 3ULL);
        a2.acquire(data_sem, 1).release(data_sem);
        a2.name("A2_Preprocess_Loop");

        // A3. 嵌套子流程 (子流程内再嵌套子流程)
        tfl::Flow stage2_sub;
        stage2_sub.name("Stage2_Transform");

        tfl::Flow nested_sub;
        nested_sub.name("Nested_Transform");
        auto n_t1 = nested_sub.emplace([](std::atomic<int>& c) {
            c.fetch_add(1);
            std::cout << "    [Nested_1] Transform\n";
            }, std::ref(global_counter));
        auto n_t2 = nested_sub.emplace([](std::atomic<int>& c) {
            c.fetch_add(1);
            std::cout << "    [Nested_2] Transform\n";
            }, std::ref(global_counter));
        n_t1.precede(n_t2);

        auto s2_nested = stage2_sub.emplace(std::move(nested_sub));
        auto s2_main = stage2_sub.emplace([](std::atomic<int>& c, tfl::Runtime& rt) {
            c.fetch_add(1);
            std::cout << "    [Stage2_Main] Runtime dispatch\n";
            rt.silent_async([] {
                std::cout << "    [silent_async] Runtime dispatch\n";
                /* background task */ });
            }, std::ref(global_counter));
        s2_nested.precede(s2_main);

        auto a3 = flow_a.emplace(std::move(stage2_sub));
        a3.acquire(gpu_sem).release(gpu_sem);
        a3.name("A3_Nested_Subflow");

        // A4. Runtime Task (内再提交 Flow)
        auto a4 = flow_a.emplace([](std::atomic<int>& c, tfl::Runtime& rt) {
            std::cout << "  [A4] Runtime dynamic dispatch\n";
            c.fetch_add(1);

            tfl::Flow dyn_flow;
            dyn_flow.name("Dynamic_SubFlow");
            auto d1 = dyn_flow.emplace([&c] {
                c.fetch_add(1);
                std::cout << "    [Dynamic_1] Run\n";
                });
            auto d2 = dyn_flow.emplace([&c] {
                c.fetch_add(1);
                std::cout << "    [Dynamic_2] Run\n";
                });
            d1.precede(d2);

            auto dyn_task = rt.submit(std::move(dyn_flow));
            dyn_task.start();
            dyn_task.wait();
            }, std::ref(global_counter)).name("A4_Runtime_Submit");

        // A5. Basic Task
        auto a5 = flow_a.emplace([](std::atomic<int>& c) {
            std::cout << "  [A5] Pipeline complete. Counter=" << c.load() << "\n";
            }, std::ref(global_counter)).name("A5_Final");

        a1.precede(a2);
        a2.precede(a3);
        a3.precede(a4);
        a4.precede(a5);

        auto task_a = executor.submit(std::move(flow_a));
        task_a.name("Task_A_Data");
        //task_a.dump(std::cout);
        task_a.start();
        task_a.wait();
    }

     // ═══════════════════════════════════════════════════════════════════════
     // 2. Flow_B: 模型训练（Jump状态机 + Branch + MultiBranch + MultiJump）
     // ═══════════════════════════════════════════════════════════════════════
     std::cout<< "\n--- Building Flow_B: Model Training ---\n";
     {
         tfl::Flow flow_b;
         flow_b.name("Model_Training");

         // B1. Basic Task
         auto b1 = flow_b.emplace([](std::atomic<int>& e) {
                             e.store(0);
                             std::cout<< "  [B1] Initialize training, epoch=" << e.load() << "\n";
                         }, std::ref(epoch)).name("B1_Init");

         // B2. Jump 状态机循环
         auto b2_train = flow_b.emplace([](std::atomic<int>& e, tfl::Runtime& rt) {
             e.fetch_add(1);
             std::cout<< "  [B2_Train] Epoch " << e.load() << " - Training...\n";
             for (int i = 0; i < 2; ++i) {
                 rt.silent_async([i] { /* train batch i */ });
             }
         }, std::ref(epoch));

         auto b3_validate = flow_b.emplace([](std::atomic<int>& e) {
             std::cout<< "  [B3_Validate] Epoch " << e.load() << " - Validating...\n";
         }, std::ref(epoch));

         auto b4_check = flow_b.emplace([](std::atomic<int>& e, std::atomic<bool>& stop, tfl::Jump& jmp) {
             if (e.load() < 3 && !stop.load()) {
                 std::cout<< "  [B4_Check] Continue loop, jump to train\n";
                 jmp.to(0);  // 跳回 b2_train
             } else {
                 std::cout<< "  [B4_Check] Exit loop, jump to save\n";
                 jmp.to(1);  // 跳到 b5_save
             }
         }, std::ref(epoch), std::ref(stop_flag));

         auto b5_save = flow_b.emplace([] {
                                  std::cout<< "  [B5_Save] Save checkpoint\n";
                              }).name("B5_Save");

         // B6. MultiBranch: 多路径分发 (使用索引)
         auto b6_test_br = flow_b.emplace([](tfl::MultiBranch& br) {
             std::cout<< "  [B6_TestBr] MultiBranch dispatch to tests 0 and 2\n";
             br.allow(0, 2);  // 只运行 test_0 和 test_2
         });

         auto b7_t0 = flow_b.emplace([] { std::cout<< "    [B7_Test_0] Unit test\n"; });
         auto b7_t1 = flow_b.emplace([] { std::cout<< "    [B7_Test_1] Integration test\n"; });
         auto b7_t2 = flow_b.emplace([] { std::cout<< "    [B7_Test_2] E2E test\n"; });
         auto b7_t3 = flow_b.emplace([] { std::cout<< "    [B7_Test_3] Stress test\n"; });

         // B7. MultiJump: 收集所有测试结果
         auto b8_collect = flow_b.emplace([](tfl::MultiJump& jmp) {
             std::cout<< "  [B8_Collect] MultiJump to all successors\n";
             jmp.to_all();
         });

         // B8. Branch: 条件决策 (使用索引)
         auto b9_decision = flow_b.emplace([](tfl::Branch& br) {
             std::cout<< "  [B9_Decision] Branch condition\n";
             br.allow(0);  // 选择第一个后继 (pass)
         });

         auto b10_pass = flow_b.emplace([] {
             std::cout<< "  [B10_Pass] All tests passed\n";
         });
         auto b11_fail = flow_b.emplace([] {
             std::cout<< "  [B11_Fail] Some tests failed\n";
         });

         auto b12_final = flow_b.emplace([] {
             std::cout<< "  [B12_Finalize] Training complete\n";
         });

         // 构建 B 内部拓扑
         b1.precede(b2_train);
         b2_train.precede(b3_validate);
         b3_validate.precede(b4_check);
         b4_check.precede(b2_train);   // 闭环 - 循环
         b4_check.precede(b5_save);     // 退出

         b5_save.precede(b6_test_br);
         b6_test_br.precede(b7_t0, b7_t1, b7_t2, b7_t3);

         b7_t0.precede(b8_collect);
         b7_t1.precede(b8_collect);
         b7_t2.precede(b8_collect);
         b7_t3.precede(b8_collect);

         b8_collect.precede(b9_decision);
         b9_decision.precede(b10_pass, b11_fail);

         b10_pass.precede(b12_final);
         b11_fail.precede(b12_final);

         auto task_b = executor.submit(std::move(flow_b));
         task_b.name("Task_B_Training");
         task_b.start();
         task_b.wait();
     }

     // ═══════════════════════════════════════════════════════════════════════
     // 3. Flow_C: 验证流程（条件 Subflow + Predicate）
     // ═══════════════════════════════════════════════════════════════════════
     std::cout<< "\n--- Building Flow_C: Validation ---\n";
     {
         tfl::Flow flow_c;
         flow_c.name("Validation");

         auto c1 = flow_c.emplace([] {
                             std::cout<< "  [C1] Load validation data\n";
                         }).name("C1_Load");

         // 条件子流程：基于谓词决定是否执行
         tfl::Flow validate_sub;
         validate_sub.name("Validate_SubFlow");
         auto v1 = validate_sub.emplace([](int id) {
             std::cout<< "    [V_" << id << "] Validate batch\n";
         }, 1);
         auto v2 = validate_sub.emplace([](int id) {
             std::cout<< "    [V_" << id << "] Validate batch\n";
         }, 2);
         v1.precede(v2);

         auto c2 = flow_c.emplace(std::move(validate_sub),
                                  [&]() mutable {
                 ++epoch;
                                      bool result = (epoch.load() % 2 == 0);
                                      std::cout<< "  [C2_Predicate] epoch=" << epoch.load()
                                                << " execute=" << (result ? "yes" : "no") << "\n";
                                      return result;
                                  });

         auto c3 = flow_c.emplace([](tfl::Branch& br) {
             std::cout<< "  [C3] Check validation result\n";
             br.allow(0);  // 选择 good
         });

         auto c4_good = flow_c.emplace([] {
             std::cout<< "  [C4_Good] Validation good\n";
         });
         auto c5_bad = flow_c.emplace([] {
             std::cout<< "  [C5_Bad] Validation bad\n";
         });

         c1.precede(c2);
         c2.precede(c3);
         c3.precede(c4_good, c5_bad);

         auto task_c = executor.submit(std::move(flow_c));
         task_c.name("Task_C_Validation");
         task_c.start();
         task_c.wait();
     }

     // ═══════════════════════════════════════════════════════════════════════
     // 4. Flow_D: 独立后台监控（无依赖，立即启动）
     // ═══════════════════════════════════════════════════════════════════════
     std::cout<< "\n--- Building Flow_D: Monitoring (Independent) ---\n";
     {
         tfl::Flow flow_d;
         flow_d.name("Monitoring");

         auto d1 = flow_d.emplace([](tfl::Runtime& rt) {
             std::cout<< "  [D1] Monitor dispatching background tasks\n";
             rt.silent_async([] { std::cout<< "    [Monitor] GPU usage\n"; });
             rt.silent_async([] { std::cout<< "    [Monitor] Memory usage\n"; });
             rt.silent_async([] { std::cout<< "    [Monitor] Network I/O\n"; });
         });

         auto d2 = flow_d.emplace([](std::atomic<int>& c) {
             c.fetch_add(1);
             std::cout<< "  [D2] Log metrics, counter=" << c.load() << "\n";
         }, std::ref(global_counter));

         auto d3 = flow_d.emplace([](std::atomic<bool>& stop, std::atomic<int>& ep, tfl::Branch& br) {
             std::cout<< "  [D3] Check stop condition\n";
             if (stop.load() || ep.load() >= 10) {
                 br.allow(0);  // stop
             } else {
                 br.allow(1);  // continue
             }
         }, std::ref(stop_flag), std::ref(epoch));

         auto d4_stop = flow_d.emplace([](std::atomic<bool>& stop) {
             stop.store(true);
             std::cout<< "  [D4_Stop] Stop monitoring\n";
         }, std::ref(stop_flag));
         auto d5_cont = flow_d.emplace([] {
             std::cout<< "  [D5_Continue] Continue monitoring\n";
         });

         d1.precede(d2);
         d2.precede(d3);
         d3.precede(d4_stop, d5_cont);

         auto task_d = executor.submit(std::move(flow_d));
         task_d.name("Task_D_Monitor");
         task_d.start();  // 立即启动，无需等待其他 Flow
         task_d.wait();
     }

     // ═══════════════════════════════════════════════════════════════════════
     // 5. Flow_E: 展示 submit 的各种形式
     // ═══════════════════════════════════════════════════════════════════════
     std::cout<< "\n--- Testing various submit forms ---\n";
     {
         std::atomic<int> test_counter{0};

         // E1. submit(Flow, N) - 执行N次
         tfl::Flow flow_e1;
         flow_e1.emplace([](std::atomic<int>& c) {
             c.fetch_add(1);
         }, std::ref(test_counter));
         executor.submit(flow_e1, 3ULL).start().wait();
         executor.wait_for_all();
         std::cout<< "  [E1] submit(Flow, 3) counter=" << test_counter.load() << "\n";

         // E2. submit(Flow, predicate) - 条件循环
         test_counter.store(0);
         int runs = 0;
         tfl::Flow flow_e2;
         flow_e2.emplace([](std::atomic<int>& c) { c.fetch_add(1); }, std::ref(test_counter));
         executor.submit(flow_e2, [&runs]() mutable noexcept {
                     return ++runs >= 2;
                 }).start().wait();
         executor.wait_for_all();
         std::cout<< "  [E2] submit(Flow, pred) counter=" << test_counter.load() << "\n";

         // E3. submit(basic_task) - 独立任务
         executor.submit([]{
                     std::cout<< "  [E3] submit(basic_task)\n";
                 }).start().wait();

         // E4. submit(task, args) - 带参数
         int val = 0;
         executor.submit([](int& v) { v = 42; std::cout<< "  [E4] value=" << v << "\n"; },
                         std::ref(val)).start().wait();
         executor.wait_for_all();

         // E5. silent_async
         executor.silent_async([](std::atomic<int>& c) {
             c.fetch_add(1);
         }, std::ref(test_counter));
         executor.wait_for_all();
         std::cout<< "  [E5] silent_async counter=" << test_counter.load() << "\n";

         // E6. async -> future
         auto fut = executor.async([]() -> int {
             std::cout<< "  [E6] async returning int\n";
             return 100;
         });
         int result = fut.get();
         executor.wait_for_all();
         std::cout<< "  [E6] async result=" << result << "\n";
     }

     // ═══════════════════════════════════════════════════════════════════════
     // 6. Flow 之间的依赖启动演示
     // ═══════════════════════════════════════════════════════════════════════
     std::cout<< "\n--- Testing Flow dependency startup ---\n";
     {
         // Flow_X: 首先执行
         tfl::Flow flow_x;
         flow_x.emplace([] { std::cout<< "  [X] First flow starts\n"; });
         auto task_x = executor.submit(std::move(flow_x));
         task_x.name("Task_X_First");
         task_x.start();
         // Flow_Y: 依赖 X 完成后启动
         tfl::Flow flow_y;
         flow_y.emplace([] { std::cout<< "  [Y] Second flow (after X)\n"; });
         auto task_y = executor.submit(std::move(flow_y));
         task_y.name("Task_Y_Second");
         task_y.start(std::move(task_x));  // 依赖 X

         // Flow_Z: 独立启动
         tfl::Flow flow_z;
         flow_z.emplace([] { std::cout<< "  [Z] Independent flow (starts immediately)\n"; });
         auto task_z = executor.submit(std::move(flow_z));
         task_z.name("Task_Z_Independent");
         task_z.start();  // 立即启动
         
         // 等待所有完成
         task_y.wait();
         task_z.wait();
         std::cout<< "  [Flow_Dependency] All done\n";
     }

     // ═══════════════════════════════════════════════════════════════════════
     // 7. D2 可视化导出
     // ═══════════════════════════════════════════════════════════════════════
     std::cout<< "\n--- D2 Visualization Export ---\n";
     {
         tfl::Flow flow_viz;
         flow_viz.name("Complex_Visualization");

         auto v1 = flow_viz.emplace([] {}).name("Init");

         tfl::Flow sub_viz;
         sub_viz.name("Parallel_Stage");
         auto s1 = sub_viz.emplace([]{}).name("Task1");
         auto s2 = sub_viz.emplace([]{}).name("Task2");
         auto s3 = sub_viz.emplace([]{}).name("Task3");
         s1.precede(s2, s3);

         auto v2 = flow_viz.emplace(std::move(sub_viz)).name("Subflow");

         auto v3 = flow_viz.emplace([](tfl::Branch& br){ br.allow(0); }).name("Branch");
         auto v4 = flow_viz.emplace([]{}).name("PathA");
         auto v5 = flow_viz.emplace([]{}).name("PathB");

         v1.precede(v2);
         v2.precede(v3);
         v3.precede(v4, v5);

         std::cout<< "Dumping to 'complex_pipeline.d2'...\n";
         std::ofstream file("complex_pipeline.d2");
         flow_viz.dump(file);
         file.close();

         std::cout<< "\n========== D2 Script ==========\n";
         std::cout<< flow_viz.dump();
         std::cout<< "================================\n";
     }

     // ═══════════════════════════════════════════════════════════════════════
     // 8. 全局等待
     // ═══════════════════════════════════════════════════════════════════════
     executor.wait_for_all();

     std::cout<< "\n=== Example 13 Complete ===\n";
     std::cout<< "Final global_counter: " << global_counter.load() << "\n";
     std::cout<< "Final epoch: " << epoch.load() << "\n";

    return 0;
}
