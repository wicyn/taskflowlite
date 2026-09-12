# 接口迁移与测试

测试以当前 `taskflowlite/core` 为准，使用 Catch2 v3，保留按模块拆分的
`test_*.cpp`、`TEST_CASE` 标签及 SECTION 注释格式。

## 构建和运行

在仓库根目录执行：

```sh
cmake -S . -B build/check -DTFL_BUILD_TESTS=ON -DTFL_BUILD_EXAMPLES=ON -DTFL_BUILD_BENCHMARKS=ON
cmake --build build/check --config Release
ctest --test-dir build/check -C Release --output-on-failure
```

离线构建可指定 `TFL_CATCH2_LOCAL_PATH`（包含 amalgamated 两个文件的目录）和
`TASKFLOW_LOCAL_PATH`（包含 `taskflow/taskflow.hpp` 的目录）。
默认构建并注册单体测试，以及 Topology 独立头文件、延迟任务构造分配失败两个独立程序，
避免 CTest 运行尚未构建的按文件目标。
需要同时构建/注册按文件测试时开启 `TFL_TEST_PER_FILE_DEFAULT`；
`TFL_TEST_RUN_TARGETS` 则提供按文件的一键构建运行目标。

## 当前接口约定

- callable 不再接收额外业务参数；使用 lambda 捕获或 `std::bind_front`。
- `executor.defer_async(...)` 返回绑定执行器的 Idle `AsyncTask<R>`；配置后用
  `task.start(deps...)` 启动一次。Idle 任务不计入 `wait_for_all()`。
- `start()` / `async()` 接受混合结果类型的 AsyncTask / AsyncFuture 前驱，
  前驱必须已启动或完成；空依赖忽略，重复依赖分别持有强引用。
- 前驱引用保留到后继 Work 销毁；任务完成或 `get()` 不会提前释放它们。
- `async` 返回 `AsyncFuture<R>`，`silent_async` 不返回结果句柄。
- `AsyncFuture::get()` 不消耗句柄；值返回 `const R&`，引用结果返回 `R`，void 无返回值。
- `Runtime::wait/wait_until/corun` 提供协作等待。不能用阻塞 Future 等待占住唯一 worker。
- `SubFlow` 在 callable 内构建动态子图，必须显式 `run()`；每轮会清空并重建子图。
- `TaskGroup` 在作用域结束时协作等待。借用的图、捕获对象和父停止域必须保持有效。
- 同一图只在前一次运行完成后复用，不能同时挂载执行同一可变子图。
- `TaskObserver` 回调必须 `noexcept`。普通节点的异常标记不代表本节点拥有异常对象；
  异常可能已归档到上层 Future。
- Runtime / TaskGroup 子任务使用各自的 `async()`，独立任务的 `start()` 不加入父任务计数。
  两者的 `run(graph)` 仍用于直接提交图。

新增独立模块覆盖 FlowBuilder、TaskGroup、TaskView、Worker/Context、ResultSlot、
SplitMix64、枚举/版本和三个提交上下文的重载矩阵；原模块中补充了动态 SubFlow、
Task::work 重绑定、AsyncTask 返回值/配置、共享 Future 生命周期等用例。

## 回归覆盖与验证

`test_async_task_dependencies.cpp` 覆盖依赖校验失败后的再次启动、混合结果、空依赖、
重复引用、右值句柄、跨执行器、子任务作用域、256 个后继扩容、并发启动和登记竞争，
以及 20,000 个节点的依赖长链回收。

下面两项历史回归继续默认启用：

- `TaskGroup: result types and dependency fan-in`
- `SubFlow: child exception reaches the future`

两项保留 `[core-regression]` 标签，没有 skip、预期失败或默认过滤。
2026-09-11 在 Windows x64 / MSVC 19.44 Release 下，不带过滤的 347 个单元测试通过，
包括这两项；旧文档中的“当前必然崩溃”结论不再适用于该次实测。

独立程序均通过 CMake 构建并由 CTest 注册：

| CTest 名称 | 检查内容 |
| --- | --- |
| `tfl_test.topology_header` | 仅包含 topology.hpp 的翻译单元能使用另一个翻译单元提供的 Executor 构造、销毁 Topology |
| `tfl_test.async_task_allocation_failure` | 关闭任务池，注入 defer_async 构造失败，检查捕获清理、活动计数与重新创建 |

`run_all_tests` 同时运行单体与上述两个独立程序。

## 尚未提供的保证

当前 core 暂不恢复 `start()` / 依赖插边期间的内存分配失败，不能保证捕获
`std::bad_alloc` 后重试提交或继续等待。这项限制没有通过预期失败或过滤掩盖：
构造分配测试明确只验证创建阶段，不再使用旧批量提交接口来断言提交回滚安全。
源码位置、生命周期规则及迁移说明见[异步任务依赖说明](../documentation/async-task-dependency-design.md)。
