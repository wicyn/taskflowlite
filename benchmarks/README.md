# Benchmark

`bench_taskflowlite` 与 `bench_taskflow` 使用相同场景、重复次数和共享计数校验，适合在同一机器和构建配置下比较。`bench_core` 单独覆盖同步 corun、三种执行上下文，以及 Runtime / TaskGroup 的重复和已完成依赖。

```sh
cmake --preset benchmarks
cmake --build --preset benchmarks --parallel 4
ctest --preset benchmarks
```

也可在常规构建中启用 `TFL_BUILD_BENCHMARKS=ON`；Windows 可使用 Visual Studio 生成器并指定 Release。离线时用 `TASKFLOW_LOCAL_PATH` 指定包含 `taskflow/taskflow.hpp` 的目录。

每个程序支持相同参数：

```sh
bench_taskflowlite --smoke
bench_taskflow --smoke
bench_core --smoke
```

`--smoke` 保留场景结构，将重复次数限制为最多 3 次；启用测试后，CTest 注册三个 smoke。校验失败或任务异常会导致非零退出码。smoke 仅验证正确性，不用来得出性能结论。

默认完整运行包括原子计数校验。两套对比基准均在计时前构图，TaskflowLite 的延迟任务也在计时前创建，计时覆盖启动和等待。`bench_core` 的动态场景包含子任务创建成本。

使用 `--no-verify` 可去掉计数校验开销，输出会注明校验已关闭；进行对比时双方须使用相同参数。`--help` 显示帮助，非法参数返回非零退出码。`run_all_benchmarks` 构建并运行三个程序的完整工作量。
