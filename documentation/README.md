# 文档版本与入口

当前库版本为 **3.3.0**。以下 Markdown 文档按当前 `core` 核对：

- [使用手册](TaskflowLite-Guide.zh-CN.md)：构图、提交、运行时、停止、异常和示例路线。
- [架构参考](TaskflowLite-Architecture.zh-CN.md)：调度、完成依赖和协程帧生命周期。
- [协程指南](coroutines.md)：`CoroTask`、`call`、`fork/join`、外部 Token 和当前接口限制。
- [异步依赖说明](async-task-dependency-design.md)：前驱不再被依赖边持有，以及提交失败回滚。

仓库保留的[使用手册 PDF](TaskflowLite-Guide.zh-CN.pdf)和[架构图册 PDF](TaskflowLite-Architecture.zh-CN.pdf)
是 **3.2.0 历史快照**，用于查看原版排版及插图，不能作为 3.3.0 API 契约。
旧 PDF 中的前驱强引用、Future / TaskGroup 停止查询、提交失败行为等说明已经过时；
升级时以以上 Markdown 和源码为准。历史插图链接也不代表旧内部布局仍适用。

API HTML 文档可通过 `TFL_BUILD_DOCS=ON` 和 `GenerateDocs` 构建，详见
[构建配置](../cmake/README.md)。本仓库的文档链接检查：

```sh
python .github/scripts/check_docs.py
```
