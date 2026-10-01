# 测试

测试使用 Catch2 v3，源文件按模块组织为 `test_*.cpp`。

## 构建和运行

在仓库根目录执行：

```sh
cmake -S . -B build/tests -DCMAKE_BUILD_TYPE=Release -DTFL_BUILD_TESTS=ON -DTFL_BUILD_EXAMPLES=OFF
cmake --build build/tests --config Release
ctest --test-dir build/tests -C Release --output-on-failure
```

默认构建单元测试、独立回归程序和头文件编译检查。也可以一次构建并运行全部常规测试：

```sh
cmake --build build/tests --config Release --target run_all_tests
```

离线构建时，使用 `TFL_CATCH2_LOCAL_PATH` 指定包含
`catch_amalgamated.cpp` 和 `catch_amalgamated.hpp` 的目录。
编译器和 Sanitizer 配置见[构建配置](../cmake/README.md)。

## 按模块运行

开启 `TFL_TEST_RUN_TARGETS` 后，可使用 `run_test_<模块名>` 构建并运行指定模块。例如信号量测试：

```sh
cmake -S . -B build/tests -DTFL_BUILD_TESTS=ON -DTFL_BUILD_EXAMPLES=OFF -DTFL_TEST_RUN_TARGETS=ON
cmake --build build/tests --config Release --target run_test_semaphore
```

## 测试选项

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `TFL_TEST_HEADERS` | ON | 检查各有效 core 头文件能否独立编译 |
| `TFL_TEST_PER_FILE_DEFAULT` | OFF | 将各模块测试加入默认构建并注册到 CTest |
| `TFL_TEST_RUN_TARGETS` | OFF | 生成 `run_test_<模块名>` 目标 |
| `TFL_TEST_CORE_ALLOCATION_FAILURE` | OFF | 普通构建中启用 core 分配失败回滚诊断，当前实现可能失败或超时 |

## 回归覆盖

默认测试覆盖异步依赖、提交前的依赖校验、停止继承、执行上下文、corun、观察者异常、对象任务、信号量、调度容器及内存生命周期。

头文件分别在普通模式和禁用编译器异常模式下独立编译。独立进程验证显式 `TFL_ENABLE_EXCEPTIONS=0` 与编译器自动检测，以及非法调用的终止行为。

普通构建默认运行 `tfl_test.async_task_allocation_failure`，检查延迟任务构造失败时的资源清理与重新创建。Sanitizer 构建保留正常提交检查，不替换全局分配器。

## 可选的分配失败回滚诊断

`core_allocation_failure.cpp` 包含七项 `tfl_test.core_allocation.*` 诊断，检查启动失败后的重试、依赖登记失败后的清理，以及 callable 替换失败后是否保留旧对象。当前 core 对提交中途的分配失败不提供完整回滚保证，因此这些诊断可能失败或超时，默认不构建、不注册到 CTest。

如需单独复现，在普通构建中显式开启：

```sh
cmake -S . -B build/allocation-diagnostics -DCMAKE_BUILD_TYPE=Release -DTFL_BUILD_TESTS=ON -DTFL_BUILD_EXAMPLES=OFF -DTFL_TEST_CORE_ALLOCATION_FAILURE=ON -DTFL_SANITIZER=OFF
cmake --build build/allocation-diagnostics --config Release --target tfl_test_core_allocation_failure
ctest --test-dir build/allocation-diagnostics -C Release -R '^tfl_test[.]core_allocation[.]' --output-on-failure
```

这个开关只控制诊断的构建与运行，不改变 core 的异常处理行为，也不表示上述回滚问题已修复。
