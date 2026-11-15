# 异步任务依赖与提交失败

本文对应 TaskflowLite 3.3.0。`Executor::async`、`Runtime::async`、`TaskGroup::async`、
`AsyncTask::start` 和 `Coro::fork` 共用动态完成依赖协议。

## 依赖是完成顺序

```cpp
auto first = executor.async([] { return 21; });
auto second = executor.async([first] { return first.get() * 2; }, first);
int result = second.get();
```

后继在全部有效前驱完成后就绪。依赖不自动传递结果或异常；需要这些信息时，像上例一样
捕获前驱 Future 并调用 `get()`。只做清理的后继，即使前驱异常结束也会运行。

- 空句柄忽略；有效前驱必须已启动或完成。传入 Idle 前驱会在提交前抛出 `tfl::Exception`。
- `AsyncTask::start` 同时拒绝空任务、自依赖和再次启动；失败的参数校验不启动任务。
- 重复依赖按传入次数登记，不承诺去重；完成后继只执行一次。
- 依赖可跨 Executor；后继始终交给后继所属 Executor 调度。
- 提交期间不得并发重置或移动正在使用的句柄；执行器必须活到提交和执行结束。
- Coro 的 AsyncTask 依赖须先转为 `AsyncFuture<R>`，见[协程兼容说明](coroutines.md)。

## 生命周期：当前实现不保留前驱表

3.3.0 的异步 Work 只保存**非拥有的动态后继边**，不再保存前驱强引用。
提交期间由调用方句柄保护前驱；尚未完成的前驱由自身执行引用保护。依赖登记完成后，
调用方可以释放自己的前驱句柄，执行顺序仍有效。

如果后继需要读取前驱结果，则必须按值捕获 Future，或自行保证一个同源句柄存活。
不要用悬空引用捕获替代它。完成依赖本身不会保留前驱 callable、结果槽或捕获的业务资源。
重复依赖也不会增加持久引用计数。`Coro::ForkOperation` 在提交前按值保存依赖句柄；
操作对象销毁后，这些临时引用释放。

Future 的值结果以 `const R&` 返回，引用有效期依赖底层 Work；最后一份句柄释放后不能再用。
`Finished` 是结果可见，不等于 Work 已销毁：执行引用和应用持有的 Future 仍可能存在。

## 登记与回滚

提交使用 `有效前驱数 + 1` 的保护计数。全部依赖处理完之前，保护计数阻止后继提前调度。
登记时和前驱完成方通过 Topology 的 `LOCKED` 位协调：

1. 已完成前驱直接扣减计数，不建边，也不分配前驱表。
2. 运行中前驱在持锁期间追加后继，随后释放锁。
3. 分配失败时，先释放失败前驱的锁，再撤销已登记边；对已发布完成的前驱等待通知归还计数。
4. 撤销完成后，提交方恢复计数和执行引用；延迟任务保持 Idle，可修复后重新 `start()`。

这适用于依赖登记失败，不代表任意 API 都有强异常保证。例如 `Task::work/work_object`
替换时先销毁旧 callable；新对象构造失败后，必须重新绑定有效 callable 才能执行或导出该节点。
图的节点身份和连接保持不变。

普通构建默认启用独立分配失败测试；Sanitizer 构建跳过全局分配器替换。
见[测试指南](../test/README.md)中的 `TFL_TEST_CORE_ALLOCATION_FAILURE`。
