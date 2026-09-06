# 构建配置

要求 CMake 3.21+、C++20。

```sh
cmake --preset release
cmake --build --preset release --parallel 4
ctest --preset release
```

Ninja 预设：`debug`、`release`、`asan`、`tsan`、`benchmarks`。
Windows 的 Ninja 构建需在编译器开发环境中运行；VS 2022 可直接使用
`windows-release`、`windows-asan`。TSan 不支持 Windows。

Windows ASan 测试前，在同一个 PowerShell 会话执行：

```powershell
./.github/scripts/asan_runtime.ps1 -BuildDirectory build/windows-asan
ctest --preset windows-asan
```

| 选项 | 默认值 | 用途 |
|---|---|---|
| `TFL_BUILD_EXAMPLES` | 顶层 ON，子项目 OFF | 构建示例 |
| `TFL_BUILD_TESTS` | OFF | 构建并注册单元测试 |
| `TFL_BUILD_BENCHMARKS` | OFF | 构建两套基准 |
| `TFL_BUILD_DOCS` | OFF | 提供 Doxygen 的 `GenerateDocs` 目标 |
| `TFL_SANITIZER` | OFF | OFF / ASAN / TSAN，互斥 |
| `TFL_NATIVE_ARCH` | OFF | GCC/Clang 内部 Release 目标启用 `-march=native` |

`benchmarks` 预设启用 `TFL_NATIVE_ARCH`，两套基准使用相同编译选项。
旧 sanitizer 布尔选项仅保留兼容入口；与新选项冲突时配置失败。
内部警告、优化和 sanitizer 选项不导出到安装包。

## 依赖

本地目录优先，未找到时才下载固定版本：

- [Catch2 v3.15.0](https://github.com/catchorg/Catch2/releases/tag/v3.15.0)：两个 amalgamated 文件均校验 SHA256。
- [Taskflow v4.1.0](https://github.com/taskflow/taskflow/releases/tag/v4.1.0)：固定提交，保留稀疏检出和 Git 元数据。

`TFL_CATCH2_LOCAL_PATH` 指向包含 `catch_amalgamated.cpp/.hpp` 的目录。
`TASKFLOW_LOCAL_PATH` 指向包含 `taskflow/taskflow.hpp` 的目录。
显式路径无效时直接报错，不回退到联网下载。

切换 Catch2 使用 `TFL_CATCH2_REF`；自定义版本须同时提供
`TFL_CATCH2_CPP_SHA256` 和 `TFL_CATCH2_HPP_SHA256`。
切换 Taskflow 使用完整 40 位 `TFL_TASKFLOW_COMMIT`。
缓存按版本隔离，不再使用 `TFL_UPDATE_DEPS` 跟随分支更新。
镜像、前缀和代理分别通过 `TFL_GITHUB_MIRROR`、`TFL_GITHUB_PREFIX`、
`TFL_GIT_PROXY` 配置；未设置代理时沿用系统代理环境变量。

## 安装与文档

```sh
cmake -S . -B build/install-only -DTFL_BUILD_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX=install
cmake --install build/install-only

cmake -S . -B build/docs -DTFL_BUILD_EXAMPLES=OFF -DTFL_BUILD_DOCS=ON
cmake --build build/docs --target GenerateDocs
```

自定义头文件安装目录使用 `CMAKE_INSTALL_INCLUDEDIR`。
下游通过 `find_package(TaskflowLite CONFIG REQUIRED)` 和
`TaskflowLite::taskflowlite` 使用安装包。
