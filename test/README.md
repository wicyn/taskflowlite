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
| `TFL_TEST_CORE_ALLOCATION_FAILURE` | ON | 普通构建中运行独立分配失败回滚测试；Sanitizer 构建跳过 |

## 回归覆盖

默认测试覆盖异步依赖、提交前的依赖校验、停止继承、执行上下文、corun、观察者异常、对象任务、信号量、调度容器及内存生命周期。

协程测试覆盖 Coroutine 图节点与重复执行、各提交入口、值/引用/移动结果、call 的帧回收及异常恢复、
fork/join 与重复依赖、外部等待的提前通知/跨线程恢复、Token 失效、协作停止，以及 Stacklet 的分配/归还。
Promise 参数查找用例覆盖捕获 Lambda、const 成员协程、非首位的 `Coro&` 和只可移动参数，防止 GCC 13.3 编译回归。
单 Worker 场景验证挂起会让出执行资源；多 Worker 场景验证相同结果。
可用 `TaskflowLiteTest "[coro]"` 单独运行，或构建 `tfl_test_coro`、`tfl_test_coro_awaiter`、`tfl_test_stack`。
当前 `Coro::fork` 的 AsyncTask 依赖需显式转为 AsyncFuture，详见[接口限制](../documentation/coroutines.md)。

头文件分别在普通模式和禁用编译器异常模式下独立编译。独立进程验证显式 `TFL_ENABLE_EXCEPTIONS=0` 与编译器自动检测，以及非法调用的终止行为。

普通构建默认运行 `tfl_test.async_task_allocation_failure`，检查延迟任务构造失败时的资源清理与重新创建。Sanitizer 构建保留正常提交检查，不替换全局分配器。

## 分配失败回滚测试

`core_allocation_failure.cpp` 包含七项 `tfl_test.core_allocation.*` 测试，检查已完成依赖不分配前驱表、
活动前驱登记失败后的解锁与重试、各提交上下文的分配失败清理，以及 callable 替换失败后释放旧对象、
重新绑定再执行的契约。测试按当前实现的分配点注入失败，并同时要求失败与成功路径都被执行。
旧的重复缺陷复现程序已合并到这组测试中。

普通构建默认开启，也可单独运行：

```sh
cmake -S . -B build/allocation-diagnostics -DCMAKE_BUILD_TYPE=Release -DTFL_BUILD_TESTS=ON -DTFL_BUILD_EXAMPLES=OFF -DTFL_TEST_CORE_ALLOCATION_FAILURE=ON -DTFL_SANITIZER=OFF
cmake --build build/allocation-diagnostics --config Release --target tfl_test_core_allocation_failure
ctest --test-dir build/allocation-diagnostics -C Release -R '^tfl_test[.]core_allocation[.]' --output-on-failure
```

设置 `TFL_TEST_CORE_ALLOCATION_FAILURE=OFF` 可跳过分配注入；该开关不改变库本身的行为。
