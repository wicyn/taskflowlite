# 示例

每个 `.cpp` 文件都是可以独立构建和运行的完整示例。

## 构建和运行

在仓库根目录执行：

```sh
cmake -S . -B build/examples -DCMAKE_BUILD_TYPE=Release -DTFL_BUILD_EXAMPLES=ON
cmake --build build/examples --config Release --target run_all_examples
```

只构建一个示例：

```sh
cmake --build build/examples --config Release --target tfl_ex_01_basic_dag
```

可执行文件位于 `build/examples/bin/examples/`，多配置生成器会再按 `Release` 等配置分目录。

## 示例索引

| 示例 | 内容 |
| --- | --- |
| 01–06 | DAG、并行、循环、Runtime、Branch、Jump |
| 07–13 | Semaphore、SubFlow、Flow 流水线、D2 导出、构图与归约 |
| 14–19 | 异步链、Observer、异常、取消、生产消费流水线、动态依赖 |
| 20–26 | 并行索引、追踪、状态机、归约、递归 Runtime、重试、TaskGroup |
| 27–33 | 动态子图、任务重绑定、Future 结果、Worker 回调、对象任务、unchecked 建边 |
| [34_dependency_errors.cpp](34_dependency_errors.cpp) | 依赖校验、异常传播与错误处理 |
| [35_bounded_queue_overflow.cpp](35_bounded_queue_overflow.cpp) | 有界队列与溢出回调 |
