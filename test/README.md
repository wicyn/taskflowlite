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
| `TFL_BUILD_CORE_REPROS` | OFF | 构建并注册独立故障复现程序 |

## 故障复现

故障复现程序通过内存分配失败注入检查异常处理，需单独启用：

```sh
cmake -S . -B build/core-repros -DCMAKE_BUILD_TYPE=Release -DTFL_BUILD_TESTS=ON -DTFL_BUILD_EXAMPLES=OFF -DTFL_BUILD_CORE_REPROS=ON -DTFL_SANITIZER=OFF
cmake --build build/core-repros --config Release --target tfl_core_failure_repro
ctest --test-dir build/core-repros -C Release -L core-repro --output-on-failure
```

这组测试可能因待修复问题而失败或超时，每个进程限时 5 秒，不属于 `run_all_tests`。
分配失败注入不支持与 ASan 或 TSan 同时启用。
