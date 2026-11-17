# Coroutine 使用与生命周期

本文按本仓库的 `core` 实现说明协程接口，完整可运行程序见
[基础调用](../examples/40_coroutine.cpp)、[派生与汇合](../examples/41_coroutine_fork.cpp)、
[外部完成与停止](../examples/42_coroutine_external.cpp)。包含 `<taskflowlite/taskflowlite.hpp>` 即可。

## 入口与结果

协程 callable 接收 `tfl::Coro&`，按值返回 `tfl::CoroTask<R>`。
`Flow::emplace`、`Executor::async/silent_async/defer_async`、Runtime / TaskGroup 的
异步提交接口及对象任务接口均可使用该 callable。节点类型为 `TaskType::Coroutine`。

```cpp
tfl::Executor executor(1);
auto answer = executor.async([](tfl::Coro&) -> tfl::CoroTask<int> {
    co_return 42;
});
int value = answer.get();
```

`R` 支持值、左值引用和 `void`，不支持右值引用。`AsyncFuture<R>::get()` 仍是非消费式的：
值结果返回 `const R&`；引用结果返回原引用。引用对象必须独立保证生命周期。
`CoroTask` 由调度器管理，不提供公开默认构造、移动、`get()` 或 `operator co_await`。

## call：同一 Work 内的顺序子协程

```cpp
auto answer = executor.async([](tfl::Coro& coro) -> tfl::CoroTask<int> {
    auto operation = coro.call([](tfl::Coro&) -> tfl::CoroTask<int> {
        co_return 21;
    });
    const int left = co_await operation;
    co_return left + co_await operation;
});
```

`call` 按值保存 callable，等待时创建子帧并注入当前 `Coro&`。父子共享同一 Work、
Topology 和 Coro；通过对称转移执行。返回值或异常交回父协程后立即销毁子帧。
同一操作支持顺序重复等待，不允许并发等待。使用 `co_await coro.call(callable)`，
不要直接 `co_await child(coro)`，也不要自行保存、恢复或销毁协程 handle。

## fork / silent_fork / join：派生独立子 Work

```cpp
auto answer = executor.async([](tfl::Coro& coro) -> tfl::CoroTask<int> {
    auto left = co_await coro.fork([] { return 20; });
    auto sum = co_await coro.fork([left] { return left.get() + 22; }, left);
    co_await coro.join();
    co_return sum.get();
});
```

- `fork` 的等待结果是 `AsyncFuture<R>`，表示已提交；它不保证子任务已经完成。
- `silent_fork` 不返回 Future。两种派生都计入父 Work 的完成范围。
- `co_await coro.join()` 等待已派生子 Work，恢复父 Work 的基准计数后，重抛当前 Work 已归档的异常。
  例如，`silent_fork` 的异常归档到当前作用域后，可在 join 处捕获；捕获后可以继续派生和 join。
  join 不消费子 Future，也不提取其结果异常；`fork` 返回的 Future 仍通过 `get()` 读取结果或异常。
- fork 支持 Basic、Runtime、SubFlow、Coroutine callable，以及 Graph 的单次、定次和谓词重载；
  Graph 的 fork 还支持完成回调。图和被借用数据须活到所有子任务结束。
- 操作对象必须 `co_await` 才会提交；未等待的操作销毁时释放尚未提交的子任务。
- 在协程帧局部变量销毁之前显式 join 所有借用这些变量的子任务；父任务的自动收尾不能延长局部变量寿命。
- `AsyncFuture::wait/get` 会阻塞 Worker，不能代替 `co_await coro.join()`。

动态依赖只保证完成顺序，完整规则见[异步依赖说明](async-task-dependency-design.md)。
当前 core 的 `Coro::fork(..., AsyncTask)` 实例化会遇到私有成员访问错误；不修改 core 时，
先转换为公开基类句柄再传入：

```cpp
auto deferred = executor.defer_async([] { return 21; });
deferred.start();
tfl::AsyncFuture<int> dependency = deferred;
auto answer = executor.async([dependency](tfl::Coro& coro) -> tfl::CoroTask<int> {
    auto child = co_await coro.fork([dependency] { return dependency.get() * 2; }, dependency);
    co_await coro.join();
    co_return child.get();
});
```

## 外部操作与 CoroToken

自定义操作满足 `tfl::CoroOperation`：

```cpp
bool await_ready();
bool await_suspend(tfl::CoroToken token);
R await_resume();
```

`await_ready() == true` 时直接取结果，不登记挂起。
否则框架先登记恢复点，再调用 `await_suspend(token)`：

| 操作结果 | 责任与行为 |
| --- | --- |
| 返回 false | 当前协程继续；不得已通知恢复，也不得遗留有效回调 |
| 返回 true | 外部完成源最终调用一次 `token.resume()`；允许注册期间同步完成 |
| 抛异常 | 撤销登记并向协程传播；不得已通知恢复，也不得遗留有效回调 |

三个接口均允许抛异常，可在协程内捕获。操作 `await_resume()` 的返回类型原样转发。
外部恢复仅通过 Token 进入 Executor，不能在 I/O 线程直接调用 coroutine handle。

Token 可复制、移动，但不可默认构造。`resume()` 返回是否接收本次恢复请求，
不代表协程已经执行完。当前等待结束后，该等待的 Token 失效：`resume()` 返回 false，
`stop_requested()` 返回 true，不能用于下一次等待。

Token 使用弱控制块，**不拥有 Coro 对象本身**。I/O 适配器仍须同步回调的完成/注销，
尤其不能让已经锁定控制块的回调与 Coro 销毁并发访问。重复通知不是回调生命周期管理方案。

## 停止、线程与内存

通过 Future / AsyncTask / TaskGroup 发出 `request_stop()`；执行中通过 `Context::stop_requested()`
查询，外部操作通过 `CoroToken::stop_requested()` 查询。停止不会主动完成外部 I/O，也不会唤醒
其挂起的协程；操作拥有者必须取消或完成 I/O，并按协议通知恢复，才能执行协程中的清理逻辑。

同一协程可在不同 Worker 上恢复。`coro.worker()` 只表示本次实际执行线程，不能跨挂起点
保存 Worker 引用；外部线程只使用 Token。所属 Executor、借用图和业务对象必须保持有效。

协程帧来自 Coro 的 Stacklet，按 LIFO 创建与回收；Worker 缓存默认大小的空段，大段归还时释放。
当前只保证 `alignof(std::max_align_t)` 对齐，不应依赖过对齐帧。`call` 的即时回收使嵌套调用
满足该顺序。不要将绑定 Worker 的 TaskGroup / ScopedExceptionAnchor 跨挂起点保存。

回归覆盖见 `test_coro.cpp`、`test_coro_awaiter.cpp`、`test_stack.cpp`；无异常模式由
`no_exceptions.cpp` 单独实例化。运行方式见[测试指南](../test/README.md)。
