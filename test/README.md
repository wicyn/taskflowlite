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
默认只构建并注册单体测试，避免 CTest 运行尚未构建的按文件目标。
需要同时构建/注册按文件测试时开启 `TFL_TEST_PER_FILE_DEFAULT`；
`TFL_TEST_RUN_TARGETS` 则提供按文件的一键构建运行目标。

## 当前接口约定

- callable 不再接收额外业务参数；使用 lambda 捕获或 `std::bind_front`。
- `AsyncTask<R>` 是延迟启动句柄，使用 `run(task, deps...)`；已启动任务不可重启。
- `async` 返回 `AsyncFuture<R>`，`silent_async` 不返回结果句柄。
- `AsyncFuture::get()` 不消耗句柄；值返回 `const R&`，引用结果返回 `R`，void 无返回值。
- `Runtime::wait/wait_until/corun` 提供协作等待。不能用阻塞 Future 等待占住唯一 worker。
- `SubFlow` 在 callable 内构建动态子图，必须显式 `run()`；每轮会清空并重建子图。
- `TaskGroup` 在作用域结束时协作等待。借用的图、捕获对象和父停止域必须保持有效。
- 同一图只在前一次运行完成后复用，不能同时挂载执行同一可变子图。
- `TaskObserver` 回调必须 `noexcept`。普通节点的异常标记不代表本节点拥有异常对象；
  异常可能已归档到上层 Future。
- 当前 `run` 的公开约束接受 Future，但内部 `_start` 只接受 AsyncTask 前驱；
  本次不修改 core，Future 前驱通过 `async(callable, future...)` 测试。

新增独立模块覆盖 FlowBuilder、TaskGroup、TaskView、Worker/Context、ResultSlot、
SplitMix64、枚举/版本和三个提交上下文的重载矩阵；原模块中补充了动态 SubFlow、
Task::work 重绑定、AsyncTask 返回值/配置、共享 Future 生命周期等用例。

## 已复现的核心回归（未隐藏）

Windows x64 / MSVC 19.44 下，下面两项默认启用的测试会触发访问异常：

- `TaskGroup: result types and dependency fan-in`
- `SubFlow: child exception reaches the future`

两项使用 `[core-regression]` 标签，**没有 skip、预期失败或默认过滤**。
完整套件仍会暴露问题，不能将其余测试通过报告为全套通过。

独立 ASan 程序同样复现无捕获 SubFlow callable 的崩溃，最小形式为：

```cpp
tfl::Executor executor(1);
auto future = executor.async([](tfl::SubFlow& sf) {
    (void)sf.emplace([] {});
    sf.run();
    return 3;
});
future.get();
```

调用栈位于 `AsyncSubFlowInvoker::invoke` 创建 SubFlow 时的 `Graph::clear()`，
在用户 callable 进入之前发生。使用非空捕获的对照程序通过。
这不是因测试未等待或捕获对象已析构导致的问题；根因修复需要单独修改 core。

仅为继续诊断其余用例，可显式运行：

```sh
build/check/bin/Release/TaskflowLiteTest "~[core-regression]"
```

修复 core 后应重新执行不带过滤的完整套件及两个回归测试。
