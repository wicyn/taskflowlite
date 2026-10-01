# TaskflowLite Core Architecture — Complete Reference Edition

**执行模型 · 多线程调度 · 并发算法 · 生命周期 · Executor 编排 · Algorithm Visual Atlas**

- TaskflowLite: **3.2.0**
- 基线分支: `main`
- 基线提交: `58eedba09f4e7a9cec5304008007777947efbb59`
- 提交时间: 2026-09-28 14:51:14 UTC
- 范围: `taskflowlite/core`
- 文档原则: 正文以该提交实际参与编译的 Core 为准；旧版页面只复用仍与当前实现一致的图形表达。
- Edition: **Polished Technical Reference Edition v2** — 合并最新版正确性、组件细节、Executor-Centric 页面与 Algorithm Visual Atlas，并统一全书图文系统。

- Visual baseline: **2026-09-29 Polished Technical Diagram System**
- 前 82 张基础/并发图重新裁切、放大，统一卡片、留白、图例与读图要点；后 49 张成熟算法板保持大图结构。
- 代码、伪代码、状态摘写全部改为浅色技术框，取消黑底代码块。
- 核心图恢复标准工程图表达：同层流程用直线，分叉与跨层关系仅使用短弧线，回路才使用曲线；图内只保留对象名、关键原子操作和状态转换。
- 对原先没有图的二级主题补充简洁视觉摘要；摘要图采用固定三段式结构，不为装饰增加多余连线。

> 这份完整版本围绕一条主线组织：**Work 怎样被创建、发布、取得执行权、推进依赖、挂起/恢复、发布完成并最终回收。** 每个并发算法都明确线程角色、共享状态、线性化点和生命周期边界。




## 0. 阅读约定与正确性边界

![0. 阅读约定与正确性边界 - 视觉摘要](assets/supp_000.png)

*图：0. 阅读约定与正确性边界 的关键对象、核心规则与边界关系*


本文把内容分为三类：**源码事实**、**由协议直接推导的设计解释**、**仍需形式化证明或压力测试覆盖的边界**。不会把“用了 atomic”自动写成 lock-free，也不会把一次运行测试写成内存模型证明。

图中约定：蓝色表示 Executor/Worker，绿色表示 Ready/执行权，橙色表示共享发布或竞争点，紫色表示 Topology/Async 生命周期，红色表示失败、竞态或回收边界。`rlx/acq/rel/ar/sc` 分别表示 relaxed/acquire/release/acq_rel/seq_cst。

本次重新核对后明确采用的当前事实：

- Worker 本地容器是 `BoundedQueue<Work*>`，**唯一 Owner push/pop，多 Stealer steal**；
- 跨线程提交与本地溢出进入 **`SharedWorkStack[]`**，不是 `UnboundedQueue`；
- `Semaphore` 当前实现是**按全局指针顺序同时锁住全部 acquire Semaphore，先只检查，再一次性提交扣减**；
- `AsyncTask::start` 和立即 `async()` 都使用 **`N + 1` submission guard** 防止依赖尚未登记完就提前 Ready；
- 动态依赖插入与 Async 完成通过 predecessor `Topology::Control::LOCKED` 协调；
- `Finished` 只表示完成已发布，**不等于 Work 已销毁**。

当前 `notifier.hpp` 的 `notify_*` CAS 成功序是 acquire，源码中还保留 REVIEW 注释。本文只描述“当前实现怎么做”，不会把这部分写成已经完成弱内存平台的穷尽证明。




## 1. 从四条主线理解 Core

![TaskflowLite Core 的四层结构](assets/01_overall.png)

*图：TaskflowLite Core 的四层结构*

整个运行时可以压缩成四条并行但互相衔接的协议：

1. **结构**：`Graph` 拥有静态 Work；Work 的 `m_edges` 保存 `[后继 | 前驱]`；
2. **调度**：Ready Work 优先 cache 接力，其次 Worker 本地有界队列，再进入共享分片；
3. **同步**：strong join 用 `m_join_counter`，休眠用 Notifier，资源约束用 Semaphore；
4. **生命周期**：Topology 的 Finished、强引用和 Executor 顶层 topology 计数共同决定等待和回收。

这四条线不能混为一谈。队列中“有 Work*”只表示调度可见；是否满足依赖由 join 决定；是否能销毁则由 Graph 所有权或 Async 强引用决定。




## 2. 多线程下，类之间到底怎样关联

![Executor、Worker、本地队列、共享栈和 Notifier 的线程角色](assets/02_threads_std.png)

*图：Executor、Worker、本地队列、共享栈和 Notifier 的线程角色*

这是阅读后续所有算法的入口图。`Worker i` 是 `BoundedQueue i` 唯一 Owner；其他 Worker 只能 steal。外部线程不能成为任何本地队列 Owner，因此只能通过 `_schedule(work)` 进入共享分片。

`SharedWorkStack` 允许多个 producer 并发 push；所有 Worker 都可尝试 steal。Notifier 不保存 Work，也不决定任务归属，它只解决“没有工作时如何安全睡眠、生产者如何唤醒”的问题。




## 3. Work 的物理结构

![Work 的物理字段与外部对象](assets/03_work_layout.png)

*图：Work 的物理字段与外部对象*

Work 是调度器真正传递的对象。用户侧 `Task` 只是句柄。关键字段中，`m_next` 是运行期侵入式链接槽；`m_join_counter` 是执行期计数器；`m_edges` 同时保存静态边和异步动态关系；`m_topology` 与 `m_parent` 都是非拥有指针。

最重要的不变量是：**同一时刻一个 Work 只能属于一条会复用 `m_next` 的运行期链。** SharedWorkStack、Semaphore waiter 都依赖这一点。




## 4. 三种“关系”必须分开

![依赖边、Work 父链、Topology 父链是三种不同关系](assets/04_relations.png)

*图：依赖边、Work 父链、Topology 父链是三种不同关系*

- `A -> B` 的依赖边决定 B 的 strong join；
- `m_parent` 决定 child 完成时把 slot 归还给谁，也用于异常向上寻找锚点；
- `Topology::m_parent` 用于停止请求向后代传播，并由强引用协议保证父 Topology 的可访问期。

Runtime 子任务可以不继承父 Topology 的停止域，但仍通过 Work parent 计入父完成计数。因此“父 Work”和“父 Topology”不是同一概念。




## 5. Topology：状态、锁、停止和引用都在一个控制字

![Topology::Control 位域和状态机](assets/05_topology_word.png)

*图：Topology::Control 位域和状态机*

Topology 的原子控制字打包 `STOP_REQUESTED | LOCKED | Status | use_count`。`LOCKED` 不是一个长期 mutex，而是动态依赖插入期间的短独占位；`Status` 只有 Idle、Running、Finished 三个公开状态。

`_wait()` 用 acquire load + `atomic::wait`，完成方以 release 发布 Finished 再 `notify_all()`。强引用 `fetch_add(relaxed)`，最后一次释放使用 `fetch_sub(acq_rel)`，使最后释放者在回收前观察到此前持有者的写入。




## 6. 原子操作分别解决什么

![6. 原子操作分别解决什么 - 视觉摘要](assets/supp_006.png)

*图：6. 原子操作分别解决什么 的关键对象、核心规则与边界关系*


| 问题 | 典型机制 | 线性化/发布位置 |
|---|---|---|
| 新 Work 对 thief 可见 | slot 写入 + bottom release | `m_bottom.store(..., release)` |
| 多 thief 谁拿到本地队列头 | top CAS | 成功 `compare_exchange` |
| SharedWorkStack 谁能操作 `m_head` | `m_state` LOCKED gate | `fetch_or(LOCKED, acquire)` |
| strong predecessor 最后谁激活后继 | join RMW | `fetch_sub(..., acq_rel) == 1` |
| Async link 与 finish 谁先 | Topology LOCKED + Finished CAS | predecessor control word |
| Future 何时可读结果 | Finished release / wait acquire | Topology control |

原子槽位、执行权 CAS 和生命周期引用是三个不同层次。比如 Stealer 可以合法地原子读取一个后来会被证明“不是它的”候选指针；只有 top CAS 成功后才取得执行权。




# Part 0-A — 构建层、句柄与 Invoker 模型

这部分回答“一个用户 callable 最终怎样变成 Executor 能调度的 Work”。它只描述构建与类型模型，不重复后面的运行时算法。



## 7. Flow / FlowBuilder：静态 Graph 的构建入口

![从 emplace 到 Graph 接管 Work 的完整创建链](assets/06a_build_chain.png)

*图：从 emplace 到 Graph 接管 Work 的完整创建链*

`Flow` 独占一个 `Graph`，并通过 `FlowBuilder` 暴露构建接口。`FlowBuilder` 自身只借用 Graph；节点由 WorkFactory 创建，成功后交给 `Graph::emplace()` 接管。返回的普通 `Task`/`TaskObject` 都不会延长静态节点寿命。

图结构修改没有内部并发保护：**构建、erase、clear、move 不得与 Executor 正在执行同一 Graph 并发发生。** Flow 移动时 Graph 会重绑各 Work 的 `m_graph` 回指。



## 8. WorkFactory / Invoker：把 callable 类型映射成统一 Work

![WorkFactory、Concrete Invoker 与 Payload 类型擦除](assets/06b_factory_invoker.png)

*图：WorkFactory、Concrete Invoker 与 Payload 类型擦除*

工厂族按 concept 选择 `Basic/Branch/MultiBranch/Jump/MultiJump/Runtime/SubFlow/Module` Invoker。`create_work()` 只负责获得 Work 存储；具体 callable、Graph holder、predicate、callback 与异步 storage 都由 Invoker 保存。

Executor 最终只看到统一的 `Work::invoke(worker, executor, cache)`。任务类型差异被压在 Payload 的具体 Invoker 中，tear-down 再由 Invoker 选择对应 Executor 路径。



## 9. Task / TaskView：句柄不是所有权

![静态句柄、视图和异步强引用的所有权差异](assets/06c_handle_ownership.png)

*图：静态句柄、视图和异步强引用的所有权差异*

`Task` 是 `Work*` 的非拥有句柄；`TaskView` 用于回调/遍历时提供受限观察。静态 Work 的寿命由 Graph 决定，所以 Graph erase/clear 后旧句柄全部悬空。

AsyncFuture/AsyncTask 不同：它们通过 Topology `use_count` 持有强引用。相同的“看起来是句柄”接口背后有完全不同的生命周期责任，不能混为一谈。



## 10. TaskObject / AsyncTaskObject：对象引用实际指向哪里

![对象型句柄只保存 Invoker 内部业务对象地址](assets/06d_object_handle.png)

*图：对象型句柄只保存 Invoker 内部业务对象地址*

`emplace_object<T>` 在 Invoker 内直接原地构造 T，返回 `TaskObject<T>{Task, T&}`；它不额外复制/移动业务对象。`AsyncTaskObject<R,T>` 则在 AsyncTask 强引用之外再保存一个非拥有 `T*`。

`object()` 不提供任何同步。即使句柄本身 const，也采用指针式 const 语义返回可修改对象；调用者仍必须保证不与任务执行对同一业务对象发生数据竞争。



## 11. TaskType：八种可调度语义最终仍落到同一 Work

![当前 TaskType 家族与统一执行内核](assets/06e_task_types.png)

*图：当前 TaskType 家族与统一执行内核*

| TaskType | 核心差异 |
|---|---|
| Placeholder | 无用户 callable，只传播普通依赖 |
| Basic | 普通同步 callable |
| Branch / MultiBranch | 运行期筛选普通 strong 后继 |
| Jump / MultiJump | 绕过普通 strong join，显式激活目标 |
| Runtime | callable 内动态派生 child，可 PREEMPT/恢复 |
| Graph | SubFlow / Module 等子图型 Invoker |

`SubFlow` 不是独立 TaskType；其 Invoker 归到 `TaskType::Graph`。Async 也不是 TaskType，它是 Work storage、Topology 和 tear-down 生命周期语义的另一维。



## 12. Context：运行期注入对象只在当前 callable 内有效

![Context 家族的栈绑定生命周期](assets/06f_context_lifetime.png)

*图：Context 家族的栈绑定生命周期*

Context 统一借用当前 `Work& / Worker& / Executor&`。Runtime、SubFlow、Branch、Jump 等对象在 Invoker 执行栈上临时创建，用户可以在回调期间查询 Worker、Executor、名称与停止状态，但不能保存这些 Context 引用到回调结束以后，也不能传到其他线程。

注意：Runtime Work 后续可能在另一 Worker 恢复，但**旧那次 callable 调用里的 Context 对象并不会跨恢复阶段继续存活**。



## 13. Async storage：Topology 与 ResultSlot 跟着 Invoker 按值存在

![ResultStorage 把 Topology 和结果槽放进 Async Invoker 生命周期](assets/06g_async_storage.png)

*图：ResultStorage 把 Topology 和结果槽放进 Async Invoker 生命周期*

`TopologyStorage` 按值拥有 Topology；`ResultStorage<R>` 再按值拥有 ResultSlot。AsyncFuture 保存 Work 和 ResultSlot 的地址，真正维持存储寿命的是 Work/Topology 强引用，而不是 ResultSlot 指针本身。

ResultSlot 不做同步：任务线程写结果，Future 只能在 Finished 的 release/acquire 完成关系之后读取。值类型根据 trait 选择“预构造后赋值”或 `optional` 延迟构造；引用结果保存指针；void 无实际结果对象。



## 14. Core 类的并发访问边界总表

![14. Core 类的并发访问边界总表 - 视觉摘要](assets/supp_014.png)

*图：14. Core 类的并发访问边界总表 的关键对象、核心规则与边界关系*


| 对象 | 主要写线程 | 并发读/竞争方 | 保护方式 |
|---|---|---|---|
| Graph / FlowBuilder | 构建线程 | 执行期只读 | 构建与执行阶段隔离 |
| Work static edges | 构建线程；Async 动态后继例外 | Worker | 静态只读；动态插入用 predecessor Topology LOCKED |
| BoundedQueue | 唯一 Owner | 多 Stealer | top/bottom/atomic slots |
| SharedWorkStack | 多 Producer | 多 thief | incoming CAS + consumer LOCKED gate |
| Notifier Waiter[i] | 绑定 Worker | notify 线程 | packed state + per-Waiter state |
| Semaphore | 任意执行/释放线程 | 任意 Worker | SpinMutex；多锁固定顺序 |
| Topology | start/link/finish/ref holders | Future/child | packed atomic control |
| ResultSlot | 执行 Work 的 Worker | 完成后的 Future reader | 本身无锁；依赖 Finished 发布关系 |
| ObjectPool FreeStack | 任意 create/destroy 线程 | 任意线程 | tagged atomic head；refill 冷路径 mutex |
| Runtime/Branch/Jump Context | 当前 Worker 栈 | 无合法跨线程访问 | 生命周期约束，不靠内部锁 |



# Part I — BoundedQueue：Worker 本地有界 Work-Stealing 队列

下面连续从数据布局、Owner push/pop、Stealer steal、最后一项竞争、槽位复用和 Executor 溢出路径解释当前实现。




## 15. BoundedQueue 的线程契约

![15. BoundedQueue 的线程契约 - 视觉摘要](assets/supp_015.png)

*图：15. BoundedQueue 的线程契约 的关键对象、核心规则与边界关系*


`BoundedQueue<Work*, cap>` 是固定容量的 Chase-Lev 风格本地双端队列：

- **唯一 Owner**：所属 Worker，可 `push/pop`；
- **任意数量 Stealer**：其他 Worker，只能 `steal`；
- `nullptr` 保留为失败返回值；
- 不拥有 Work，不管理 Work 生命周期；
- `size/empty` 是两个 relaxed load 得到的瞬时估计，不是事务条件。




## 16. BoundedQueue 内存布局

![BoundedQueue 的槽位、top、bottom 与线程写权限](assets/10_bq_layout.png)

*图：BoundedQueue 的槽位、top、bottom 与线程写权限*

`m_buf`、`m_top`、`m_bottom` 分别做缓存行对齐，降低 Owner 对 bottom 与多个 thief 对 top 的伪共享。槽位仍然是普通数组中的多个 atomic pointer，并不是每个槽位独占缓存行。




## 17. 逻辑索引与物理环

![逻辑索引映射到固定物理槽](assets/11_bq_ring.png)

*图：逻辑索引映射到固定物理槽*

有效逻辑区间是 `[top,bottom)`，物理位置为 `index & (cap-1)`，因此容量必须是 2 的幂。物理槽会复用，但逻辑索引当前不支持有符号溢出回绕；这是长期运行必须承认的前提。




## 18. 单元素 push：先写槽，再发布 bottom

![Owner push 的发布顺序与 release/acquire 关系](assets/12_bq_push.png)

*图：Owner push 的发布顺序与 release/acquire 关系*

当前单元素 push 读取 `top` 使用 acquire；若满则直接调用溢出回调。未满时先 `slot.store(relaxed)`，再通过 `bottom.store(release)` 把该槽纳入有效逻辑区间。

关键不是“slot 是 atomic 就够了”，而是**发布顺序**：Stealer 必须先看到新的 bottom，之后才可以依赖此前的槽位写入已经发布。




## 19. 批量 push：一次 release 发布整个前缀

![批量 push 的本地前缀与共享溢出](assets/13_bq_batch.png)

*图：批量 push 的本地前缀与共享溢出*

批量版本先计算剩余容量，只写能容纳的前缀，最后一次性 release 更新 bottom。当前代码这里读取 top 是 relaxed，与单元素 push 的 acquire 不同；它依赖“旧 top 只会保守低估空间”这一 Owner-only 容量判断属性。

剩余区间交给 Executor 的 overflow 回调，进入 SharedWorkStack 分片。




## 20. Owner pop：为什么先缩 bottom

![Owner pop 的三种分支](assets/14_bq_pop.png)

*图：Owner pop 的三种分支*

Owner 先把 bottom 减一，相当于预占尾部候选；随后 seq_cst fence 再读 top。多元素时可直接取尾；空队列时恢复 bottom；只有 `top == bottom` 的最后一项需要与 thief 争夺 top。




## 21. 最后一项竞争：两个线程都可能先读到同一 Work*

![Owner 与 Stealer 对最后一个逻辑槽的 CAS 竞争](assets/15_last_item_race.png)

*图：Owner 与 Stealer 对最后一个逻辑槽的 CAS 竞争*

候选指针可被两方都读到，但执行权只能由 top CAS 的一个成功者取得。失败方必须丢弃候选，不得触发 Observer、执行 callable，也不得解引用那些可能已经随正确消费者完成而销毁的数据。




## 22. Steal：一次尝试，不在容器内部自旋

![Stealer 从逻辑头部取得执行权](assets/16_bq_steal.png)

*图：Stealer 从逻辑头部取得执行权*

Stealer 读取 top，经过 seq_cst fence 再 acquire 读取 bottom；观察到元素后先读 atomic slot，再用 top 的 seq_cst CAS 争夺。CAS 失败直接返回 nullptr，把“换 victim / 重试”的策略留给 Executor。




## 23. 多 thief 为什么不会执行同一项两次

![23. 多 thief 为什么不会执行同一项两次 - 视觉摘要](assets/supp_023.png)

*图：23. 多 thief 为什么不会执行同一项两次 的关键对象、核心规则与边界关系*


多个 thief 可以同时读到相同 `top` 和相同槽位指针，但它们竞争的是同一个 `top -> top+1` CAS。成功者唯一。失败者返回空。

这里的“每项唯一取得”依赖两个条件：

1. 队列内部 top CAS 只允许一个 winner；
2. 调用层不能把同一个 Work 重复发布到另一队列或共享栈。

容器只解决同一逻辑位置的竞争，不负责整个系统的去重。




## 24. 为什么槽位本身也必须 atomic

![落后 thief 与环槽复用](assets/17_slot_reuse.png)

*图：落后 thief 与环槽复用*

落后的 thief 可能在 CAS 之前读到已经被新逻辑索引复用的同一物理槽。最终 top CAS 会失败，保证它不执行误读 Work；但若槽位不是 atomic，先前的并发读写本身已经形成 C++ 数据竞争。因此：**atomic slot 保证“读是合法的”，top CAS 决定“读到的值是否属于我”。**




## 25. BoundedQueue memory-order 表

![25. BoundedQueue memory-order 表 - 视觉摘要](assets/supp_025.png)

*图：25. BoundedQueue memory-order 表 的关键对象、核心规则与边界关系*


| 路径 | 操作 | 当前顺序 | 作用 |
|---|---|---|---|
| single push | top load | acquire | 容量判断；保守观察 thief 进度 |
| single/batch push | slot store | relaxed | 填槽，尚未发布 |
| push | bottom store | release | 发布新增逻辑区间 |
| pop | bottom store | relaxed | Owner 预占尾部 |
| pop | fence | seq_cst | 与 steal 边界观察协议配对 |
| pop last | top CAS | seq_cst | 最后一项唯一执行权 |
| steal | top load | acquire | 读取逻辑头 |
| steal | fence | seq_cst | 与 Owner 缩尾协调 |
| steal | bottom load | acquire | 观察 push 发布 |
| steal | top CAS | seq_cst | 窃取线性化点 |




## 26. BoundedQueue 在 Executor 中不是孤立容器

![Ready Work 从 cache 到本地队列再到共享层](assets/18_bq_integration.png)

*图：Ready Work 从 cache 到本地队列再到共享层*

第一 Ready 后继通常不进队列，而是直接放 `cache`；额外 Ready 后继才进入本地 BoundedQueue。队列满时溢出到 SharedWorkStack。其他 Worker 对本地队列和共享栈都只做 steal。




# Part II — SharedWorkStack：跨线程发布与本地溢出的共享层

SharedWorkStack 使用 Work::m_next 组成 intrusive 链，生产者与消费者并不共享同一个普通链头。




## 27. SharedWorkStack 的三个字段

![incoming、state、head 的线程访问域](assets/21_sws_layout.png)

*图：incoming、state、head 的线程访问域*

`m_incoming` 是 producer 共享的原子发布头；`m_head` 是消费者私有链，只有取得 gate 的 thief 可访问；`m_state` 高位是 LOCKED，低 63 位是“已计数但尚未完成 steal”的 Work 数量。

这不是经典的“所有消费者直接对同一个 Treiber head 做 pop”。它先争夺消费 gate，再在 gate 内操作私有 `m_head`。




## 28. push：计数先加，再发布 incoming

![SharedWorkStack push 的两阶段发布](assets/22_sws_push.png)

*图：SharedWorkStack push 的两阶段发布*

当前 push 先 `m_state.fetch_add(n, relaxed)`，再连接 `m_next`，最后通过 `m_incoming` 的 release CAS 发布整条链。因此 count 可能短暂包含“已经记账但还没挂到 incoming”的 Work。

这也是为什么 `size()` / `empty()` 只用于调度观察，而不能理解为精确可消费数量。




## 29. 批量 push：链只在发布前遍历一次

![29. 批量 push：链只在发布前遍历一次 - 视觉摘要](assets/supp_029.png)

*图：29. 批量 push：链只在发布前遍历一次 的关键对象、核心规则与边界关系*


批量接口先在 producer 线程内把 `W0 -> W1 -> ... -> Wn` 连好；CAS 重试时只需重新把 `last->m_next` 指向新的 observed incoming，无需重新遍历整个链。

对于 Executor 的链式发布接口，Work 链可能先在 tear-down 或 Semaphore release 中形成，然后按 shard 切段，再使用 `push(first,last,n)` 整段发布。




## 30. steal：先竞争 consumer gate，再碰 m_head

![SharedWorkStack steal 完整路径](assets/23_sws_steal.png)

*图：SharedWorkStack steal 完整路径*

第一次 relaxed 预检查只用于快速失败。真正执行权来自 `fetch_or(LOCKED, acquire)`；若旧值已经有 LOCKED，本 thief 立即返回，不等待当前消费者。

取得 gate 后，如果 `m_head` 为空，就用一次 acquire `exchange(nullptr)` 接管整个 incoming 链；摘一个 Work 后清空它的 `m_next`，最后 release 地同时 `count--` 并清 LOCKED。




## 31. 两个 thief 同时到达时发生什么

![多个 thief 对消费 gate 的竞争](assets/24_sws_two_thief.png)

*图：多个 thief 对消费 gate 的竞争*

只允许一个 thief 访问非原子的 `m_head`。输掉 gate 的线程不在共享栈内部自旋，而是回到 Executor 换 victim。这把“等待一个热点共享结构”的策略放到更高层。

因此 SharedWorkStack 的消费进度依赖持有 gate 的线程最终完成操作，**整个消费过程不能称为 lock-free**。




## 32. incoming 与 m_head 为什么分开

![消费者一次接管整条 incoming 链](assets/25_sws_handoff.png)

*图：消费者一次接管整条 incoming 链*

消费者可以持续从已接管的 `m_head` 取 Work，同时新的 producer 继续向原子 `m_incoming` 发布，不需要等待当前消费链清空。代价是全局顺序既不是严格 FIFO，也不是严格 LIFO。




## 33. SharedWorkStack 的 count 语义

![33. SharedWorkStack 的 count 语义 - 视觉摘要](assets/supp_033.png)

*图：33. SharedWorkStack 的 count 语义 的关键对象、核心规则与边界关系*


低 63 位 count 包括：

- 已经发布在 `m_incoming` 的 Work；
- 已经被某个 thief 接管到 `m_head`、但尚未 steal 完的 Work；
- 已经 `fetch_add` 记账、但 producer 还没完成 release CAS 的 Work。

所以 `count != 0` 仍可能在一次 steal 中拿不到节点；反过来一次 steal 返回 nullptr 也不证明系统全局无任务。Executor 必须继续探索或进入 Notifier 的双检查协议。




## 34. Executor 如何把共享任务分片

![_push_shared 的分片策略](assets/26_shared_shards.png)

*图：_push_shared 的分片策略*

单 Work 使用 Work 地址混合后 `mulhi64(..., shard_count)` 选 shard。小批次 `n <= shard_count` 从一个哈希起点轮转逐个发布；大批次按 `base` 与 `extra` 切分，使各分片数量差不超过 1，再用批量 push 发布。

目标是降低所有外部提交集中打同一个原子头的热点，不提供顺序稳定性保证。




# Part III — Executor / Worker / Notifier：调度算法如何串起来

这一部分从 Executor 视角连接本地队列、共享栈、随机 victim、cache 接力、休眠和唤醒。




## 35. Executor 持有哪些并发核心对象

![Executor 的运行期对象图](assets/30_executor_graph.png)

*图：Executor 的运行期对象图*

每个 Worker 对应一个本地队列；共享栈数量与 Worker 数一起组成 `num_queues()` 的 victim 空间。`m_num_topologies` 不是 Ready 任务数，而是顶层异步执行链的生命周期计数。




## 36. Worker 主循环

![Worker 从 cache/local 到 wait_for_work 的循环](assets/31_worker_loop.png)

*图：Worker 从 cache/local 到 wait_for_work 的循环*

`_invoke` 本身会沿 cache 连续执行，返回后 Worker 再 `pop()` 自己本地尾部。只有本地为空才进入 `_wait_for_work`。这使“当前任务刚刚解锁的后继”优先留在当前执行核心上。




## 37. `_wait_for_work` 是三阶段退避

![快速 steal → yield → Notifier 的三阶段搜索](assets/32_wait_for_work.png)

*图：快速 steal → yield → Notifier 的三阶段搜索*

- victim 空间同时包含其他 Worker 的本地队列和 SharedWorkStack 分片；
- 成功后保存 `m_vtm`，下次从近期成功位置开始；
- 快速 steal 超预算后进入 steal+yield；
- 再超预算才 `prepare_wait`，随后重新扫描所有共享栈和其他 Worker 队列。

只有 double-check 仍无可见工作，才 `commit_wait` 真正进入等待。




## 38. Ready Work 的四种发布方式

![Executor 根据调用线程和批量大小选择发布路径](assets/33_schedule_paths.png)

*图：Executor 根据调用线程和批量大小选择发布路径*

1. `cache`：同一调用栈直接接力；
2. `_schedule(wr, work)`：当前 Executor Worker 的本地队列；
3. 本地满后的 overflow：进入 SharedWorkStack；
4. 外部线程或跨 Executor：直接 `_schedule(work)` 到目标 Executor 共享层。

所有正常发布路径都遵守“先 publish Work，后 notify”。




## 39. cache handoff 为什么重要

![第一 Ready 后继继承 parent slot 并直接接力](assets/34_cache_handoff.png)

*图：第一 Ready 后继继承 parent slot 并直接接力*

cache 不是一个第二队列，只是 `_invoke` 当前调用链的单个直达槽。tear-down 的第一个 Ready 后继使用它；如果 `_schedule_parent` 要恢复 PREEMPTED parent 而 cache 已占用，会先把旧 cache 正常排入本地队列，再让 parent 占据 cache。




## 40. Lost wake-up：为什么不能“看空后直接睡”

![没有两阶段协议时的危险窗口](assets/36_lost_wakeup.png)

*图：没有两阶段协议时的危险窗口*

Producer 的 notify 可能发生在 Worker 检查队列为空之后、实际 park 之前。如果没有预等待登记，这次通知找不到睡眠者，而 Worker 随后又真的睡下，造成已经有 Work 却无人醒来的死锁风险。




## 41. Notifier 状态字与 per-Worker Waiter

![Notifier 64 位状态字与 Waiter 三态机](assets/37_notifier_word.png)

*图：Notifier 64 位状态字与 Waiter 三态机*

状态字把 epoch、prewaiter 数量和 waiter 栈顶索引打包在一个原子里。每个 Worker 固定绑定一个 Waiter，Waiter 自己又有 `NotSignaled / Waiting / Signaled` 三态，用来封闭“已经入栈但还没进入 OS wait”的窗口。




## 42. prepare → double-check → cancel/commit

![两阶段等待协议](assets/38_notifier_protocol.png)

*图：两阶段等待协议*

`prepare_wait` 先登记 prewaiter，再执行 seq_cst fence。Executor 随后重新检查共享分片和其他本地队列：看到工作就 `cancel_wait`；仍然没有工作才 `commit_wait`。

commit/cancel 用 prepare 时记录的 epoch + 当时 prewaiter 个数恢复“轮到我的票号”，把多个同时准备睡眠的 Worker 按票号顺序结算。




## 43. notify 为什么优先消费 prewaiter

![43. notify 为什么优先消费 prewaiter - 视觉摘要](assets/supp_043.png)

*图：43. notify 为什么优先消费 prewaiter 的关键对象、核心规则与边界关系*


`notify_one()` 先做 seq_cst fence 再读取状态。如果存在 prewaiter，它直接做 `epoch++ / prewaiter--`，不触发 futex；对应 Worker 在 commit 中发现自己的票号已经被越过，直接放弃睡眠。

只有没有 prewaiter、但栈里已有真正等待者时，notify 才弹 waiter 栈并 `_unpark()`。这避免刚准备睡的线程做一次无意义 wait/wake 系统调用。




## 44. park / unpark 三态机

![Waiter::state 关闭“入栈到 park”窗口](assets/39_park_unpark.png)

*图：Waiter::state 关闭“入栈到 park”窗口*

如果 notify 先到，`_unpark` 把状态改成 Signaled，随后 `_park` 的 `NotSignaled -> Waiting` CAS 失败，于是根本不睡；如果 park 已经把状态改为 Waiting 并进入 `atomic::wait`，unpark 的 exchange 观察到 Waiting 后再 `notify_one()`。




## 45. Notifier 正确性边界

![45. Notifier 正确性边界 - 视觉摘要](assets/supp_045.png)

*图：45. Notifier 正确性边界 的关键对象、核心规则与边界关系*


当前 `prepare_wait` 与 `notify_*` 的 seq_cst fence 构成 lost-wakeup 关闭协议；commit 的入栈 CAS 是 release。源码当前 `notify_one/all/n` 对 `m_state` 的成功 CAS 使用 acquire，并保留 REVIEW 注释。

因此本文只做两件事：

- 精确描述当前实现的指令顺序与意图；
- 把“弱内存平台上所有链遍历可见性是否已经形式化闭合”列为审查边界，而不是自行宣布已证明。




## 46. Executor shutdown

![Executor 析构的顺序](assets/80_shutdown.png)

*图：Executor 析构的顺序*

析构先等所有顶层 topology 结束，再 release 设置 Worker terminate 位，`notify_all()` 唤醒可能 park 的线程，Worker 在 `_wait_for_work` 中 acquire 观察终止请求并返回 nullptr，最后 Executor join 全部线程。




# Part IV — Graph / Work / Join：Ready 是怎样计算出来的

队列只保存已经 Ready 的 Work；真正的依赖条件由 Graph、join weight 和 tear-down 推进。




## 47. Graph 的所有权模型

![47. Graph 的所有权模型 - 视觉摘要](assets/supp_047.png)

*图：47. Graph 的所有权模型 的关键对象、核心规则与边界关系*


`Graph` 独占 `std::vector<Work*> m_works` 中静态节点的物理生命周期。边是非拥有指针。`erase()` 先断开双向边，再 swap-with-last 从物理数组移除，最后 `destroy_work()`。

Graph 的结构修改不得与执行并发。Executor 在运行期会原地把 source 节点交换到 `m_works[0,n)` 前缀，所以**物理顺序不是用户创建顺序的稳定语义**。




## 48. Work 边表为什么是 `[successors | predecessors]`

![48. Work 边表为什么是 `[successors | predecessors]` - 视觉摘要](assets/supp_048.png)

*图：48. Work 边表为什么是 `[successors | predecessors]` 的关键对象、核心规则与边界关系*


`m_num_successors` 是分割点：前半段连续扫描后继，后半段连续扫描前驱。建边时 A 和 B 两侧都更新，保持 A 的 successor 与 B 的 predecessor 对称。

静态图执行期边表只读；Async 动态依赖会在 predecessor Topology LOCKED 下向 predecessor 的 successor 前缀追加后继，因此边表并不总是“纯静态”。




## 49. `_set_up_graph`：运行前重新绑定所有状态

![Graph 每轮运行的 setup](assets/43_setup_graph.png)

*图：Graph 每轮运行的 setup*

每轮执行都重新绑定 `m_parent/m_topology`，清上一轮异常位，按前驱区间重新计算 strong predecessor 数，并把 join weight 编码到 `m_properties` 低位。

**source 使用物理入度==0 判断，而不是 join_weight==0。** weak predecessor 虽不参与 strong join，仍然是物理前驱，因此有 weak 前驱的节点不能成为初始 source。




## 50. strong join 的执行权只有最后一个到达者拿到

![多个 strong predecessor 汇聚到一个后继](assets/44_join_counter.png)

*图：多个 strong predecessor 汇聚到一个后继*

每个 strong predecessor 完成一次 `fetch_sub(1, acq_rel)`。旧值为 1 的线程把计数变为 0，并且唯一取得该后继本轮的执行权。

acq_rel RMW 还把多个 predecessor 的完成写入汇合到最后到达线程，使其在发布 successor 前建立完成链。




## 51. `_tear_down_task`：先恢复自己，再推进别人

![普通 Work tear-down 的顺序](assets/45_teardown.png)

*图：普通 Work tear-down 的顺序*

当前 Work 执行前其 join_counter 已经被前驱减到 0；执行期间同一计数器还可能被 Runtime/SubFlow 复用为 child slot。最终 tear-down 必须先 `fetch_add(static join_weight)` 恢复下一轮，再扫描后继。

不能用 store 恢复：循环控制中下一轮前驱可能已经提前递减了当前计数，store 会覆盖这些已经发生的到达。




## 52. fan-out：第一 Ready 后继与额外 Ready 后继待遇不同

![52. fan-out：第一 Ready 后继与额外 Ready 后继待遇不同 - 视觉摘要](assets/supp_052.png)

*图：52. fan-out：第一 Ready 后继与额外 Ready 后继待遇不同 的关键对象、核心规则与边界关系*


扫描后继时，第一项 `old==1` 的 successor 进入 `cache` 并**继承当前 Work 已经占用的 parent slot**。后续 Ready successor 被原地聚集到边表前缀；在发布它们之前必须先 `parent.join_counter += num_ready`。

这个“先计数、后发布”的顺序防止后继在新 slot 还没建立完整时快速执行完成，把 parent 提前减到零。




## 53. parent slot 守恒

![每个活动 child 对 parent join_counter 的责任](assets/46_parent_slots.png)

*图：每个活动 child 对 parent join_counter 的责任*

把 parent join_counter 理解为“尚未结束的活动责任”最容易检查正确性：

- 当前 Work 已经占一个 slot；
- 第一 Ready successor 可继承，不改变计数；
- 每个额外 Ready successor 需要新增一个 slot；
- 无 successor 继承时 `_schedule_parent` 归还当前 slot。

Runtime / TaskGroup / Graph source 发布也遵守同一个“先建立 slot，再 publish child”规则。




## 54. `_schedule_parent` 为什么先缓存 PREEMPTED

![54. `_schedule_parent` 为什么先缓存 PREEMPTED - 视觉摘要](assets/supp_054.png)

*图：54. `_schedule_parent` 为什么先缓存 PREEMPTED 的关键对象、核心规则与边界关系*


`_schedule_parent` 先读 `parent->m_properties & PREEMPTED`，再 `parent.join_counter.fetch_sub(1, acq_rel)`。只有旧值为 1 且 parent 正在 PREEMPTED 状态，才恢复 parent。

减到零后执行权可能已经转交，继续读取 parent 的可变字段会扩大生命周期竞态，因此需要在 decrement 前缓存恢复条件。




## 55. Branch 与 Jump 的本质区别

![Branch 仍走 strong join；Jump 强制激活](assets/48_branch_jump.png)

*图：Branch 仍走 strong join；Jump 强制激活*

Branch 只是在运行期选择“哪些普通依赖边要传播”，目标仍然执行 `fetch_sub` strong join；Jump 则是控制流激活，直接把目标 `join_counter` 置零，绕过普通依赖屏障。

因此 Jump/MultiJump 可以参与显式循环控制，而普通 strong edge 仍保持依赖语义。




## 56. Basic Work 完整执行管线

![从 Ready 到 tear-down 的同步节点路径](assets/49_basic_pipeline.png)

*图：从 Ready 到 tear-down 的同步节点路径*

Basic/Branch/Jump 等具体 Invoker 的外围协议高度一致：先处理 Semaphore，进入执行检查，Observer before，调用用户函数并捕获异常，Observer after，release Semaphore，最后由对应 tear-down 推进依赖。

Semaphore 获取失败时 Invoker 直接 return：Work 没执行 callable，也没有完成 tear-down；它已经进入 blocker waiter 链，等待未来 release 后重新调度。




## 57. Observer 为什么不能依赖 thread_local “同一任务同一线程”

![57. Observer 为什么不能依赖 thread_local “同一任务同一线程” - 视觉摘要](assets/supp_057.png)

*图：57. Observer 为什么不能依赖 thread_local “同一任务同一线程” 的关键对象、核心规则与边界关系*


普通 Basic 通常 before/after 在同一 Worker，但 Runtime/Graph-family 任务可以挂起并由最后一个 child 在另一 Worker 上恢复。`_schedule_parent` 可以把 parent 直接放入恢复线程的 cache。

因此 Observer 的 before 与 after 可能发生在不同 Worker；统计状态应按 Work/Observer 实例绑定，而不是假设 thread_local 槽天然配对。




## 58. 异常沿 Work parent 链，停止沿 Topology parent 链

![异常归档锚点选择](assets/74_exception.png)

*图：异常归档锚点选择*

用户异常被 Invoker 捕获后进入 `_process_exception`：先沿 Work parent 链传播 EXCEPTION，显式锚点优先，其次首个隐式锚点，最后当前 Work 兜底。归档位置通过 `EXCEPTION_CAUGHT` 原子位竞争“首个异常写入权”。

停止请求是另一条链：`Topology::_stop_requested()` 沿 Topology parent 向上查询，并把祖先停止惰性缓存到当前 Topology。




# Part V — Runtime / SubFlow / TaskGroup / Semaphore：动态并行如何复用同一内核






## 59. Runtime 的 PREEMPTED 状态机

![Runtime child slot 与 parent 恢复](assets/56_runtime_preempt.png)

*图：Runtime child slot 与 parent 恢复*

Runtime 首次进入时置 PREEMPTED，并先加一个“body 基准 slot”。用户 callable 可以继续派生 child；每个 child 在发布前再占一个 slot。body 返回时减掉基准 slot，若仍有 child 则当前 Work 返回调度器，不做最终 tear-down。

最后一个 child 归还 slot 时 `_schedule_parent` 观察 old==1 且 PREEMPTED，于是 parent 进入当前 Worker 的 cache 恢复最终阶段。




## 60. SubFlow 与 Runtime 的共同点和差异

![60. SubFlow 与 Runtime 的共同点和差异 - 视觉摘要](assets/supp_060.png)

*图：60. SubFlow 与 Runtime 的共同点和差异 的关键对象、核心规则与边界关系*


二者都在当前 Worker 执行期动态产生子工作并使用 parent slot 协议。SubFlow 额外按值拥有一个内部 Graph，并在 callable 内构建/运行该 Graph；Runtime 更偏向直接提交 async/silent_async、TaskGroup 或协作等待。

它们注入给用户的 `Runtime&` / `SubFlow&` 都是栈绑定上下文，只在当前回调有效，不能保存到回调外。




## 61. 协作式等待 `_corun_until`

![61. 协作式等待 `_corun_until` - 视觉摘要](assets/supp_061.png)

*图：61. 协作式等待 `_corun_until` 的关键对象、核心规则与边界关系*


Worker 线程等待 child 时不会直接阻塞 OS 线程。`_corun_until` 反复：

1. pop 本地队列；
2. 若空，按 victim 空间 steal 其他 Worker 或 SharedWorkStack；
3. 超预算时 yield；
4. 每轮都重新检查用户 predicate。

它没有进入 Notifier park，目的是让已经被“同步等待”占住的 Worker 仍然帮助系统推进。




## 62. TaskGroup：一个栈绑定 AnchorWork 聚合动态任务

![TaskGroup 的 AnchorWork 完成域](assets/58_taskgroup.png)

*图：TaskGroup 的 AnchorWork 完成域*

TaskGroup 从当前 Context 借用 Worker/Executor，并在栈上构造 `AnchorWork<false>`。每个组内 child 发布前先 `anchor.join_counter++`；`wait()` 和析构都通过 `_corun_until(anchor.join==0)` 协作等待。

AnchorWork 同时是 EXPLICIT_ANCHOR，因此组内异常有稳定的归档位置。




## 63. Semaphore 数据结构

![63. Semaphore 数据结构 - 视觉摘要](assets/supp_063.png)

*图：63. Semaphore 数据结构 的关键对象、核心规则与边界关系*


每个 Semaphore 保存 `m_max_value / m_value / waiter_head / waiter_tail` 和一把 SpinMutex。等待链是 FIFO intrusive Work 链，复用 `Work::m_next`。

Work 的 acquire 列表在配置时按 `Semaphore*` 全局顺序排序并去重；release 列表不需要同时持有全部锁，因此不要求相同排序。




## 64. 多 Semaphore acquire：锁全部，再先检查、后提交

![当前 Semaphore 的 all-check-then-commit 事务](assets/60_semaphore_acquire.png)

*图：当前 Semaphore 的 all-check-then-commit 事务*

`SemaphoreLock` 按 acquire 列表的固定全局顺序锁住全部目标，避免多个 Work 形成循环等待。随后第一阶段只检查，不修改任何 `m_value`；所有请求都满足后，第二阶段才统一扣减。

若有 blocker，当前 Work 追加到 blocker waiter 链，并要求 blocker 的锁最后释放。blocker 一旦解锁，其他线程可能立刻 release 并重新调度当前 Work，因此当前路径之后不能再访问其 acquire 数据。




## 65. release 只提供“重试机会”

![Semaphore release 与 waiter 重调度](assets/61_semaphore_release.png)

*图：Semaphore release 与 waiter 重调度*

release 增加 quota 后，会把**当前整条 waiter 链**一次摘下并追加到调用方输出链，而不是只挑当前 quota 足够的若干任务。Executor 清理每个 Work 的 `m_next` 后重新发布。

被唤醒的 Work 会从头重新执行完整 acquire 事务，所以“被唤醒”不等于“已经拿到资源”；这可能产生重试和惊群，但避免在 release 时做跨多个 Semaphore 的复杂预分配。




# Part VI — AsyncTask / AsyncFuture：动态依赖与生命周期






## 66. AsyncFuture / ResultSlot 只负责结果与寿命，不负责调度 Ready

![66. AsyncFuture / ResultSlot 只负责结果与寿命，不负责调度 Ready - 视觉摘要](assets/supp_066.png)

*图：66. AsyncFuture / ResultSlot 只负责结果与寿命，不负责调度 Ready 的关键对象、核心规则与边界关系*


`AsyncFuture<R>` 保存 Work 与 ResultSlot 指针，并持有一份 Work/Topology 强引用。ResultSlot 本身不提供同步；结果可见性来自 Async completion 发布 Finished，以及 wait/get 的 acquire 完成关系。

`ResultSlot<R>` 对适合复用的类型预构造并赋值，否则用 `optional<R>` 延迟构造；`R&` 保存指针，`void` 无结果对象。




## 67. AsyncTask::start 是一次提交事务

![AsyncTask::start 的八个阶段](assets/63_async_start.png)

*图：AsyncTask::start 的八个阶段*

公开 AsyncTask 先校验所有 dependency，再在自身 Topology 仍为 Idle 时 CAS 取得 LOCKED，保证并发 start 只有一个线程进入准备区。

保存 predecessor edge 时同时给每个 predecessor 增一份强引用。`join_counter = N + 1` 中的 `+1` 是 submission guard：依赖登记全部结束之前，无论多少 predecessor 提前完成，都不能让当前任务 Ready。




## 68. 为什么必须有 submission guard

![68. 为什么必须有 submission guard - 视觉摘要](assets/supp_068.png)

*图：68. 为什么必须有 submission guard 的关键对象、核心规则与边界关系*


假设两个 predecessor 都已经完成。如果直接把 `join_counter=N`，链接第一个 Finished predecessor 时就减一，链接第二个时可能减到零；此刻 start 线程还没完成剩余发布状态和依赖处理，完成方就可能调度当前 Work。

Guard 让最终 `1 -> 0` 只能发生在两种安全时机之一：

- start 线程在全部 link 完成后释放 guard；
- 或最后一个 predecessor 在 guard 已经释放后完成。

取得 `old==1` 的线程就是唯一调度者。




## 69. `_link_predecessors` 与 predecessor 完成竞争

![动态后继登记与 Finished 发布的互斥窗口](assets/64_async_link_finish_race.png)

*图：动态后继登记与 Finished 发布的互斥窗口*

Linker 先看 predecessor 的 Topology control：若已经 Finished，直接给 successor `join--`；否则 CAS 获取 predecessor LOCKED，向 predecessor 的 successor 前缀插入当前 Work，再 release 解锁。

Finisher 要从 Running CAS 到 Finished，也必须在控制字未 LOCKED 时成功。因此“登记成功”和“Finished 先发布”二者不会都漏掉该 successor。




## 70. Async 完成：先发布 Finished，再冻结后继

![Async tear-down 的完成顺序](assets/65_async_finish.png)

*图：Async tear-down 的完成顺序*

完成方 CAS Running→Finished，成功 CAS 的 release 部分发布结果和此前普通写；随后 `notify_all()` Future waiter。Finished 之后新的 `_link_predecessors` 会直接走“已完成”分支，不再向当前 Work 追加后继，因此 `m_num_successors` 可以冻结并顺序传播。

传播 Ready successor 时，如果 successor 属于另一个 Executor，就调用目标 Executor 的 `_schedule(successor)`，从其共享分片发布。




## 71. Finished 与 Destroyed 是两个事件

![Async Work 的强引用来源](assets/66_async_lifetime.png)

*图：Async Work 的强引用来源*

一个 Async Work 可能同时被外部句柄、执行生命周期、后继的 predecessor 引用持有。Finished 只说明结果已经发布，Future 可观察；只有最后一份强引用释放，才由 `_destroy_async()` 真正销毁。




## 72. `_destroy_async` 为什么先析构自己，再释放前驱

![异步依赖链的迭代销毁](assets/67_destroy_async.png)

*图：异步依赖链的迭代销毁*

销毁路径先交换出边表，保存 successor 分割点，随后 `destroy_work(current)`，确保当前 callable、ResultSlot 和 Topology storage 先结束生命周期；之后才对 predecessor 引用逐一 `ref--`。

如果 predecessor 也归零，不递归调用销毁，而是借用已经死亡执行关系中的 `m_parent` 临时串入 pending 链，继续迭代回收，避免深依赖链造成 C++ 调用栈增长。




## 73. Async 目前最值得继续审查的异常边界

![73. Async 目前最值得继续审查的异常边界 - 视觉摘要](assets/supp_073.png)

*图：73. Async 目前最值得继续审查的异常边界 的关键对象、核心规则与边界关系*


当前 `AsyncTask::start` 在取得自身 LOCKED 后仍可能执行 `vector::reserve/push_back`；`_link_predecessors` 在 predecessor LOCKED 区域内也可能 `vector::push_back`。如果这些分配抛异常，强异常回滚需要非常谨慎地恢复锁、引用、部分插边与自身状态。

这不是普通用户 callable 异常路径，不能由 Future 的异常归档机制自动兜底。文档把它列为实现审查点，而不是假定已经完全事务化。




# Part VII — Payload / ObjectPool / 辅助容器：运行时内存如何工作






## 74. Payload：Work 内部类型擦除 + SBO

![74. Payload：Work 内部类型擦除 + SBO - 视觉摘要](assets/supp_074.png)

*图：74. Payload：Work 内部类型擦除 + SBO 的关键对象、核心规则与边界关系*


Work 按值拥有 `Payload`。Payload 保存 invoke 函数指针、Operations 指针和固定大小 buffer；小 Invoker 原地构造，大/过对齐 Invoker 走 heap。执行时 `Work::invoke()` 只通过 Payload 的统一入口分派，不需要 Work 自身形成一棵虚类层次。

`Payload::emplace` 会先 reset 旧 payload，再构造新对象；新构造失败时旧 callable 已经不存在，因此载荷替换**不是强异常保证**。




## 75. ObjectPool 总体结构

![Work 对象池的 Bucket / Slab / ObjectBlock](assets/70_pool.png)

*图：Work 对象池的 Bucket / Slab / ObjectBlock*

当前 Work 池默认是分桶并发池。每个 Bucket 有 tagged FreeStack、refill mutex 和 Slab 链；每个 ObjectBlock 永久记录所属 FreeStack 地址，因此对象可以在任意线程销毁并直接回到原桶。

每个 Bucket 构造时就预分配首个 Slab，正常 create 优先走原子 free-stack 热路径。




## 76. Tagged head 解决的是 ABA 版本，不是对象寿命本身

![指针地址恢复但 tag 已改变](assets/71_aba.png)

*图：指针地址恢复但 tag 已改变*

FreeStack head 比较 `(pointer,tag)`。地址 A 被 pop、经过其他操作又 push 回来时，tag 已推进，落后线程的旧 expected 不再匹配。

64 位压缩版本默认 48 位指针 + 16 位 tag，tag 有限并会回绕；128 位版本空间更大，但仍不是“无限时间绝不回绕”的数学证明。Slab 长期存活和 atomic `next_free` 另外解决节点地址/链接并发访问问题。




## 77. create 热路径与 refill 冷路径

![对象池从原子 pop 到补充 Slab](assets/72_pool_refill.png)

*图：对象池从原子 pop 到补充 Slab*

create 先用 thread_local 计数器选择 Bucket，从 tagged FreeStack pop。空栈才进入 `refill_mutex`；拿锁后再次 pop，避免等待期间别人已经补货或 destroy 归还，却仍然多分配 Slab。

若仍为空，新 Slab 的第一个 Block 直接给当前线程，其余 Block 预先连接后一次 CAS 整链发布。




## 78. 跨线程 destroy 为什么不需要找到“分配线程”

![78. 跨线程 destroy 为什么不需要找到“分配线程” - 视觉摘要](assets/supp_078.png)

*图：78. 跨线程 destroy 为什么不需要找到“分配线程” 的关键对象、核心规则与边界关系*


ObjectBlock 的 `free_stack` 在 Slab 创建时永久绑定。destroy 从对象地址恢复 ObjectBlock，先析构 T，再直接 push 回该 FreeStack。回收线程与创建线程可以不同，不需要 thread_local owner map。

对象池整体并不是 lock-free：free-stack 热路径可以使用 lock-free atomic，但 refill 明确有 mutex 和系统分配。




## 79. SmallVector 和 SplitMix64 的角色

![79. SmallVector 和 SplitMix64 的角色 - 视觉摘要](assets/supp_079.png)

*图：79. SmallVector 和 SplitMix64 的角色 的关键对象、核心规则与边界关系*


`SmallVector` 用于 MultiBranch/MultiJump 等预期较小的目标集合；小集合直接使用 inline storage，超出后才分配。它不是线程安全容器，安全来自当前 Work 执行期间的独占访问。

每个 Worker 有自己的 SplitMix64 状态，用于 victim 选择和分片缩放；随机数只服务调度，不承担密码学用途。




# Part VIII — 三条端到端执行轨迹






## 80. 静态菱形图：A → {B,C} → D

![静态 fan-out / fan-in 的一次合法执行](assets/78_static_trace.png)

*图：静态 fan-out / fan-in 的一次合法执行*

初始化时 B/C join=1，D join=2。A 完成后 B/C 都 Ready：第一个继承 A 的 parent slot 进 cache，另一个先让 parent slot+1 再发布。B/C 各完成一次使 D 2→1→0，最后到达者取得 D 执行权并继承该 slot。

D 完成且无后继后归还最终 slot，parent 计数归零。若 B/C 的实际执行顺序相反，slot 守恒仍然成立。




## 81. Runtime：body 已返回，但 Work 还没有完成

![81. Runtime：body 已返回，但 Work 还没有完成 - 视觉摘要](assets/supp_081.png)

*图：81. Runtime：body 已返回，但 Work 还没有完成 的关键对象、核心规则与边界关系*


Runtime body 内派生 child 后，当前 Work 的 PREEMPTED 仍然保持，join_counter 还包含 child slot。body 返回只释放基准 slot；如果计数不为零，Invoker 直接 return，不执行 Observer after、Semaphore release 或最终 tear-down。

最后 child 完成才恢复 Runtime Work；因此“用户函数已经 return”和“Task 节点完成”不是同一个时刻。




## 82. Async dependency + Semaphore 资源等待

![Async Ready 后仍可能因资源不足暂停](assets/79_async_sem_trace.png)

*图：Async Ready 后仍可能因资源不足暂停*

Async C 先通过 dependency + guard 协议取得 Ready。Worker 第一次执行 C 时若 Semaphore quota 不足，C 进入 waiter 链并返回，Topology 仍是 Running，执行引用也仍存在。

未来其他 Work release quota 后，C 被重新发布，再从头竞争全部 acquire；成功后才执行 callable、发布 Finished、传播动态后继并释放引用。




## 83. Executor 销毁前为什么必须先 `wait_for_all`

![83. Executor 销毁前为什么必须先 `wait_for_all` - 视觉摘要](assets/supp_083.png)

*图：83. Executor 销毁前为什么必须先 `wait_for_all` 的关键对象、核心规则与边界关系*


顶层 `silent_async/async` 启动时增加 `m_num_topologies`；最终异步 tear-down 或 silent_async tear-down 才减少。Executor 析构先等该计数为零，避免 Work 仍可能把任务重新发布到已经开始销毁的 Worker/共享栈/Notifier。




# Part IX — 多线程交互逐帧图谱

这一部分不再重复定义类，而把关键并发协议按“某一时刻哪些线程看到什么状态”展开。每页只回答一个竞态问题。



## 84. BoundedQueue：发布前后四个状态快照

![Owner push 与 Stealer 可见性的四帧状态](assets/80_bq_frames.png)

*图：Owner push 与 Stealer 可见性的四帧状态*

新元素真正进入逻辑区间的时刻是 `bottom.store(release)`。在此之前 slot 虽已经写入，但 Stealer 不能把它当成已发布元素；在 acquire 观察到新的 bottom 之后，之前的 slot 写才属于该发布链。

```text
Owner:   slot.store(W, relaxed)  ->  bottom.store(b+1, release)
Stealer:                              bottom.load(acquire)  ->  slot.load(relaxed)
```



## 85. BoundedQueue：多个 thief 读到同一候选并不等于重复执行

![两个 thief 对同一 top 的 CAS 仲裁](assets/81_bq_multi_thief.png)

*图：两个 thief 对同一 top 的 CAS 仲裁*

多个 thief 可以推测性读取相同 `slot[top]`。执行权只在线性化点 `CAS(top, top+1)` 产生；失败者必须完全丢弃候选指针，不能触发 Observer、访问 callable 或继续使用 Work 内部状态。



## 86. BoundedQueue：本地容量不足时怎样进入共享层

![批量 Ready 从本地前缀溢出到共享分片](assets/82_bq_overflow_executor.png)

*图：批量 Ready 从本地前缀溢出到共享分片*

这条链说明 BoundedQueue 的“有界”并不等于 Executor 的全局背压。Owner 只把可容纳前缀留在本地，剩余 Work 通过 overflow 回调进入 SharedWorkStack；随后统一通知等待 Worker。



## 87. SharedWorkStack：两个 Producer 同时 CAS incoming

![多 Producer 只竞争原子 incoming 头](assets/83_sws_producer_race.png)

*图：多 Producer 只竞争原子 incoming 头*

每个 Producer 只修改自己尚未发布链的尾节点 `m_next`。CAS 失败后 expected 会更新为最新 incoming，Producer 重新把自己的尾连接到新头再试；它不会修改已经由其他 Producer 发布的链内部节点。



## 88. SharedWorkStack：count 非零但本次 steal 仍可能拿不到节点

![count-before-publish 的短暂窗口](assets/84_sws_publication_gap.png)

*图：count-before-publish 的短暂窗口*

`m_state.fetch_add()` 发生在 `m_incoming` release CAS 之前，所以调度观察中的 count 是保守的“未完成窃取责任数”，不是精确 `incoming + head` 长度。一次空 steal 只是本次尝试失败，Executor 不能据此进入睡眠，仍需完整探索/Notifier 协议。



## 89. Work::m_next：同一个字段如何在不同运行期链之间安全复用

![m_next 的链所有权状态机](assets/102_mnext_state_machine.png)

*图：m_next 的链所有权状态机*

安全条件不是“`m_next` 是原子”，它实际上是普通指针；安全来自阶段互斥。Work 从共享栈返回或从 Semaphore waiter 链重新发布前，都必须先清空 `m_next`，否则一个 Work 会同时属于两条 intrusive 链。



## 90. Executor：victim index 同时覆盖本地队列与共享分片

![Worker 的统一 victim index 空间](assets/86_executor_victim_space.png)

*图：Worker 的统一 victim index 空间*

`m_vtm` 保存最近成功位置以保留一点局部性；后续失败时再用 Worker 私有 SplitMix64 选择其他 index。一个 steal 返回 nullptr 不区分“空”和“竞争失败”，所以策略在 Executor 外层继续探索，而不是让容器内部无限重试。



## 91. Executor：真正睡眠前的完整时序

![从退避耗尽到 park 的 Executor 时序](assets/87_executor_sleep_timeline.png)

*图：从退避耗尽到 park 的 Executor 时序*

这是本地队列、共享栈与 Notifier 的结合点。`prepare_wait` 不是“准备好就睡”，而是先建立可被 notify 观察到的意图；随后必须再次扫描业务队列，只有仍然没有可见 Work 才 commit。



## 92. Notifier：两个 Worker 的票号怎样把物理乱序变成逻辑顺序

![T0/T1 prepare 后的 epoch 结算次序](assets/88_notifier_ticket_order.png)

*图：T0/T1 prepare 后的 epoch 结算次序*

prepare 保存“当时 epoch + 前面已有多少 prewaiter”。commit/cancel 都从这个快照恢复 target ticket；后来者即使更早到达结算代码，也必须等待前一个 ticket 推进 epoch，避免多个 prewaiter 同时把同一份状态算术当作自己的。



## 93. Notifier：notify 抢在 commit 之前为什么不需要 futex wake

![prewaiter 快速路径避免无效 park/wake](assets/89_notifier_fast_path.png)

*图：prewaiter 快速路径避免无效 park/wake*

notify 消耗 prewaiter 后，相当于已经替该 Worker 完成一次“不要睡”的结算。Worker 之后进入 commit 时发现 epoch 已越过自己的 target，直接返回；这就是 prepare 窗口中通知不会丢失的一个关键交错。



## 94. `_set_up_graph`：Graph 物理数组如何变成 source 前缀

![从构建态物理顺序到运行态 source prefix](assets/90_setup_graph_frames.png)

*图：从构建态物理顺序到运行态 source prefix*

source 的原地 swap 只改变 `m_works` 的物理排列，不改变依赖边。后续批量调度直接使用 `[begin, begin+num_sources)`；因此任何依赖“节点创建顺序等于 m_works 顺序”的外部假设都不成立。



## 95. `_tear_down_task`：fan-out 的逐帧状态

![A 完成后多个 successor Ready 的责任转移](assets/91_fanout_frames.png)

*图：A 完成后多个 successor Ready 的责任转移*

第一 Ready 后继不是“优先级更高”，而是刚好能复用当前 Work 已经占用的 parent slot 和 cache 直达槽；额外 Ready 必须先补 parent slot，再进入本地/共享调度层。



## 96. parent slot：跨 Worker 执行仍保持守恒

![B/C 分散到不同 Worker 后 parent 计数如何变化](assets/92_parent_slot_timeline.png)

*图：B/C 分散到不同 Worker 后 parent 计数如何变化*

parent 计数不依赖 child 最终在哪个 Worker 上运行。只要每个新发布 child 在 publish 前建立 slot、每个最终完成责任恰好归还一次，跨核 steal 不改变完成语义。



## 97. Runtime：最后一个 child 可以在另一 Worker 上恢复 parent

![PREEMPTED parent 的跨 Worker 恢复](assets/93_runtime_cross_worker.png)

*图：PREEMPTED parent 的跨 Worker 恢复*

这解释了为什么 Runtime/Graph-family 不能假设一次逻辑任务始终在同一 OS 线程上。最后一个 child 所在线程执行 `_schedule_parent`，可以把 PREEMPTED parent 直接放到自己的 cache 继续运行。



## 98. Semaphore：全局锁顺序解决的是什么问题

![两个 Work 的多 Semaphore 锁顺序统一](assets/94_semaphore_lock_order.png)

*图：两个 Work 的多 Semaphore 锁顺序统一*

排序不是为了决定业务优先级，而是消除锁顺序环。真正的资源事务仍在全部锁持有后执行 check-all/commit-all；因此“固定锁顺序”和“原子式配额提交”是两层不同保证。



## 99. Semaphore：为什么 blocker 的锁必须最后释放

![加入 waiter 后最后释放 blocker lock 的生命周期边界](assets/95_semaphore_blocker_unlock.png)

*图：加入 waiter 后最后释放 blocker lock 的生命周期边界*

一旦 blocker lock 解开，另一个线程可能立刻 release、摘下整条 waiter 链并把当前 Work 发布到任意 Worker。当前调用栈若随后继续读取 Work 的 acquire vector，就会与重新执行/销毁发生生命周期竞争，所以 blocker 解锁必须成为最后访问边界。



## 100. AsyncTask：N+1 guard 的实际计数演算

![一个 Finished 前驱 + 一个 Running 前驱的 guard 时间线](assets/96_async_guard_timeline.png)

*图：一个 Finished 前驱 + 一个 Running 前驱的 guard 时间线*

Guard 不是额外 dependency，而是提交线程自己的“登记尚未结束”责任。只有 guard 与所有 predecessor arrival 都释放后，`join_counter` 才能从 1 到 0；取得这次转换的线程负责唯一调度。



## 101. Async：link 与 Finished 的两种合法交错

![动态后继登记与 predecessor 完成的二选一协议](assets/97_async_two_interleavings.png)

*图：动态后继登记与 predecessor 完成的二选一协议*

关键不变量是：**每个 predecessor 对 successor 恰好贡献一次 arrival**。要么 successor 在 LOCKED 临界区被登记，随后完成方传播；要么 Finished 已先发布，linker 不再插边而直接抵消 join。



## 102. Async：Finished 之后为什么 Work 仍然可能存在

![执行引用、句柄引用和 predecessor 引用的生命周期](assets/98_async_reference_lifetime.png)

*图：执行引用、句柄引用和 predecessor 引用的生命周期*

完成状态只发布结果；内存回收由强引用独立决定。特别是一个已 Finished 的 predecessor 仍可能被多个未销毁 successor 的 predecessor edge 持有，因此不能把 `Finished` 当作 `delete` 时刻。



## 103. ObjectPool：分配线程和最终回收线程可以不同

![ObjectBlock 永久绑定 FreeStack 使跨线程 destroy 成立](assets/99_pool_cross_thread.png)

*图：ObjectBlock 永久绑定 FreeStack 使跨线程 destroy 成立*

这里的关键是 ObjectBlock 记录 FreeStack，而不是 Work 记录“原 Worker”。工作窃取导致执行线程变化不会改变内存归属；最后销毁线程直接根据 ObjectBlock 元数据归还原 Bucket。



## 104. Topology：状态发布与对象销毁是两条轴

![Topology 状态与 use_count 的正交关系](assets/100_topology_lifetime.png)

*图：Topology 状态与 use_count 的正交关系*

`Idle/Running/Finished` 回答执行状态；`use_count` 回答存储是否还能销毁；`LOCKED` 只是在动态依赖/启动中的短临界区。把三者打包进同一 atomic word 是为了原子协调，不代表三种语义可以互相替代。



## 105. 异常链与停止链不能画成同一条父关系

![Work parent 与 Topology parent 的不同用途](assets/101_exception_stop_chains.png)

*图：Work parent 与 Topology parent 的不同用途*

异常归档沿 `Work::m_parent` 找 explicit/implicit anchor；停止查询沿 `Topology::m_parent` 继承 STOP_REQUESTED。两条链通常相关但不是同一对象关系，文档和调试器都应该分别观察。



## 106. 静态菱形图：一次真实的双 Worker 交错

![A→{B,C}→D 在两个 Worker 上的责任转移](assets/103_diamond_multithread.png)

*图：A→{B,C}→D 在两个 Worker 上的责任转移*

这张图把 work-stealing 与 join/parent-slot 放在一起：W1 窃取 C 只改变“谁执行 C”，不会改变 C 对 D 的 dependency arrival，也不会改变 C 持有的 parent slot。最后到达 D 的线程取得 D 的执行权。



## 107. Async + Semaphore：三个线程域如何连续交接同一个 Work

![从外部 start 到依赖、资源等待、重新发布与完成](assets/104_async_semaphore_multithread.png)

*图：从外部 start 到依赖、资源等待、重新发布与完成*

同一个 C 可以依次经历外部提交线程、dependency 完成线程、Worker0 资源阻塞、Worker1 资源释放、Worker2 最终执行。正确性依靠的是 Topology/join/Semaphore/调度发布协议，而不是线程亲和性。



## 108. 全局不变量清单

![108. 全局不变量清单 - 视觉摘要](assets/supp_108.png)

*图：108. 全局不变量清单 的关键对象、核心规则与边界关系*

**调度**
- 本地队列只有唯一 Owner push/pop；
- SharedWorkStack `m_head` 只有 gate winner 访问；
- publish Work 必须早于 notify；
- `m_next` 同一时刻只属于一条 intrusive 链。

**依赖**
- strong predecessor 每轮只贡献一次 join 到达；
- `fetch_sub == 1` 的线程取得本轮执行权；
- tear-down 先恢复自身 join weight，再传播后继；
- extra child/ready Work 必须先增加 parent slot 再发布。

**Async**
- AsyncTask 只从 Idle 启动一次；
- 动态依赖登记与 Finished 由 predecessor Topology LOCKED 协调；
- submission guard 在全部依赖登记完成前禁止 Ready；
- Finished 不等于 ref==0。

**等待/资源**
- prepare_wait 后必须 double-check；
- Semaphore acquire 在全部锁保护下先检查、再提交；
- waiter 被 release 唤醒只得到重试机会。




## 109. 已知边界与不应过度宣称的性质

![109. 已知边界与不应过度宣称的性质 - 视觉摘要](assets/supp_109.png)

*图：109. 已知边界与不应过度宣称的性质 的关键对象、核心规则与边界关系*


- BoundedQueue 逻辑 `int64_t` 索引不支持无限回绕；
- SharedWorkStack 的消费 gate 使其消费进度不是 lock-free；
- Notifier 的序列号回绕和当前 notify CAS memory-order 仍值得做模型/弱内存平台验证；
- TaggedHead64 的有限 tag 会回绕；
- Async 动态插边的分配失败事务需要专门失败注入验证；
- Semaphore 会整链唤醒 waiter，可能产生重试和竞争；
- 调度器不提供实时性、严格公平性或全局 FIFO 保证。




## 110. 当前源码阅读路线

![110. 当前源码阅读路线 - 视觉摘要](assets/supp_110.png)

*图：110. 当前源码阅读路线 的关键对象、核心规则与边界关系*


| 问题 | 当前主入口 |
|---|---|
| 本地最后一项怎么争 | `bounded_queue.hpp::pop/steal` |
| 跨线程 Work 怎么发布 | `shared_work_stack.hpp::push/steal`，`executor.hpp::_push_shared` |
| Worker 如何找活/睡眠 | `executor.hpp::_wait_for_work`，`notifier.hpp` |
| source / join 怎么建立 | `executor.hpp::_set_up_graph` |
| 普通后继什么时候 Ready | `executor.hpp::_tear_down_task` |
| Branch 与 Jump 差异 | `_tear_down_branch_task` / `_tear_down_jump_task` |
| Runtime child 怎么恢复父 | `work_invokers.hpp` + `_schedule_parent` |
| Semaphore 怎么阻塞但不阻塞 Worker | `work.hpp::SemaphoreLock/_try_acquire_semaphores` + `semaphore.hpp::_release` |
| Async 依赖竞态 | `async_task.hpp::start` + `executor.hpp::_link_predecessors/_tear_down_async_task` |
| Async 怎么回收 | `work.hpp::_destroy_async` |
| Work 内存池 | `object_pool.hpp` + `work.hpp::create_work/destroy_work` |




## 111. 源码事实表：本版相对旧文档必须保持的纠正

![111. 源码事实表：本版相对旧文档必须保持的纠正 - 视觉摘要](assets/supp_111.png)

*图：111. 源码事实表：本版相对旧文档必须保持的纠正 的关键对象、核心规则与边界关系*


| 项目 | 当前 `main` 事实 |
|---|---|
| 共享调度容器 | `SharedWorkStack[]` |
| 本地队列 | `BoundedQueue<Work*, TFL_DEFAULT_QUEUE_SIZE>` |
| SharedWorkStack 消费 | LOCKED gate + private `m_head`，不是多消费者 lock-free pop |
| Batch local push | top 使用 relaxed 读取；一次 release 发布前缀 |
| Semaphore 多资源获取 | acquire 列表排序；锁全部；check-all；commit-all |
| AsyncTask start | 自身 LOCKED + `N+1` guard + predecessor 强引用 |
| 动态依赖完成竞态 | predecessor Topology LOCKED 与 Finished CAS |
| Topology | packed control word，而不是独立 state/use_count 原子 |
| Source | 物理前驱数为 0，不是 join_weight 为 0 |
| Jump | 目标 `join_counter.store(0)` 强制激活 |




## 112. 结论：从 Executor 视角把所有算法串起来

![112. 结论：从 Executor 视角把所有算法串起来 - 视觉摘要](assets/supp_112.png)

*图：112. 结论：从 Executor 视角把所有算法串起来 的关键对象、核心规则与边界关系*


一条 Work 的完整路径可以概括为：

**构建 Graph / Async state → 建立 parent/Topology/join → Ready → cache 或 local/shared 发布 → Worker pop/steal → Semaphore/Observer/callable → tear-down → successor join / parent slot → Finished → 引用归零后回收。**

BoundedQueue、SharedWorkStack、Notifier、join_counter、Topology 和 ObjectPool 各自只负责这条链上的一个问题。设计正确性的关键不是“某个类是否线程安全”，而是**在每个阶段明确谁拥有写权、谁取得执行权、哪一个原子操作发布可见性、最后一次访问发生在什么时候**。

# Part X — Core Component Deep Reference

这一部分补回以前版本中更细的组件页面，但全部放在最新版运行时主线之后阅读。这里不重新定义调度协议，只补充数据结构、具体任务类型和对象生命周期的局部细节。


## 113. SmallVector：内联存储、迁移与异常边界



`SmallVector<T, N>` 用于小规模集合，核心目标是减少小集合 heap allocation。

![SmallVector 内联与堆存储](assets/struct_smallvector.svg)

*图：SmallVector 内联与堆存储*

在 Core 中典型用途：

- Work 边表；
- MultiBranch / MultiJump 目标集合；
- Semaphore request；
- Observer 等小集合。

### 设计重点

1. 小规模情况直接使用对象内存；
2. 超过 inline capacity 后才进入堆；
3. 空容量特化减少无意义存储；
4. API 尽量接近标准容器；
5. 对 Work 这种高频对象尤其重要，因为边数通常较小。

---

## 114. SpinMutex：短临界区的自旋与让步



`SpinMutex` 使用 `atomic_flag`，适合极短临界区。

释放：

```text
m_flag.clear(memory_order_release)
```

主要用于 Semaphore 内部状态：

![Semaphore 内部组成](assets/struct_semaphore.svg)

*图：Semaphore 内部组成*

其目标不是通用公平锁，而是保护非常短的 quota / waiter 更新。

---

## 115. ObjectBlock：对象存储与空闲链元数据为什么分开



![ObjectBlock 结构](assets/struct_objectblock.svg)

*图：ObjectBlock 结构*

关键点：

- `free_stack` 永久绑定所属 Bucket；
- `next_free` 是空闲链元数据；
- `storage` 才承载 T 的对象生命周期；
- 元数据与 T 生命周期分离。

这样即使 block 的 `storage` 内正在构造 T，空闲链元数据仍有独立的 C++ 对象生命周期。

---

## 116. Slab / Bucket：对象池冷路径所有权



一个 Slab 固定包含多个长期存在的 `ObjectBlock`。

![Slab 与 Bucket](assets/struct_slab.svg)

*图：Slab 与 Bucket*

Slab 的作用：

- 一次批量获得多个 block；
- 地址稳定；
- refill 时只进行一次大分配；
- Slab 在 ObjectPool 销毁前保持存在。

---

## 117. TaggedHead64 / TaggedHead128：ABA 版本与平台前提



FreeStack head 不是单纯指针，而是：

```text
(pointer, tag)
```

### TaggedHead64

![TaggedHead64 位布局](assets/struct_tagged64.svg)

*图：TaggedHead64 位布局*

### TaggedHead128

![TaggedHead128 位布局](assets/struct_tagged128.svg)

*图：TaggedHead128 位布局*

每次 head 修改同时增加 tag：

```text
(P0, 10)
   ↓ pop / push
(P1, 11)
```

即使地址后来再次回到 `P0`：

```text
(P0, 12)
```

CAS 仍能通过 tag 区分不同历史状态。

---

## 118. ResultSlot：值、引用与 void 三种结果形态

![118. ResultSlot：值、引用与 void 三种结果形态 - 视觉摘要](assets/supp_118.png)

*图：118. ResultSlot：值、引用与 void 三种结果形态 的关键对象、核心规则与边界关系*


异步结果统一通过 `ResultSlot<R>` 表示。

三种形态：

```text
ResultSlot<R>
ResultSlot<T&>
ResultSlot<void>
```

### 值类型

根据类型特征选择：

### 左值引用

```text
ResultSlot<T&>
    ↓
T* m_value
```

### void

无存储，只统一接口。

---


## 119. TopologyStorage / ResultStorage：Async Invoker 的按值状态



异步 Invoker 通过 Storage 直接按值拥有 Topology。

![Async Invoker 状态存储](assets/struct_async_storage.svg)

*图：Async Invoker 状态存储*

因此异步任务的运行状态和结果槽跟随 Work Payload 生命周期存在。

---

## 120. Work 边表布局：`[successors | predecessors]`

![120. Work 边表布局：`[successors | predecessors]` - 视觉摘要](assets/supp_120.png)

*图：120. Work 边表布局：`[successors | predecessors]` 的关键对象、核心规则与边界关系*


`m_edges` 同时保存后继和前驱：

![Work 边表逻辑分区](assets/struct_edges.svg)

*图：Work 边表逻辑分区*

这种布局使：

- 后继扫描连续；
- 前驱扫描连续；
- 单个 SmallVector 即可表达双向边表。

---

## 121. `_precede`：一条静态依赖如何同时写入两端



建立：

![precede 双向写边](assets/struct_precede.svg)

*图：precede 双向写边*

需要同时更新：

```text
A.successors += B
B.predecessors += A
```

因此第二端分配失败不会留下单边图关系。

---

## 122. Graph 合法性：普通 strict cycle 与 Jump 控制循环



普通非 Jump 边建立时通过 DFS 检查是否形成不经过 Jump 的 strict cycle。

![普通 strict cycle](assets/struct_cycle_invalid.svg)

*图：普通 strict cycle*

Jump 控制节点允许表达循环控制：

![Jump 控制循环](assets/struct_cycle_jump.svg)

*图：Jump 控制循环*

DFS 遇到 Jump / MultiJump 不继续展开，因此显式控制循环与普通 DAG 依赖语义分离。

---

## 123. Work::Properties：静态 join weight 与运行属性



`m_properties` 包含运行期独占标志和静态 join weight。

| 字段 | 作用 |
|---|---|
| `IMPLICIT_ANCHOR` | 当前 Work 可作为隐式异常归档锚点 |
| `PREEMPTED` | Work 正在等待派生 child，后续需要恢复 |
| `STRONG` | 作为前驱时参与普通 strong join |
| `JOIN_WEIGHT` | 低位保存静态 strong predecessor 数量 |

![Work Properties 位布局](assets/struct_properties.svg)

*图：Work Properties 位布局*

---

## 124. Work::Control：并发异常位与执行检查

![124. Work::Control：并发异常位与执行检查 - 视觉摘要](assets/supp_124.png)

*图：124. Work::Control：并发异常位与执行检查 的关键对象、核心规则与边界关系*


并发访问的 Work 控制状态放在 `m_control`。

| 字段 | 作用 |
|---|---|
| `EXPLICIT_ANCHOR` | 显式异常锚点 |
| `EXCEPTION` | 当前执行链处于异常路径 |
| `EXCEPTION_CAUGHT` | 当前 Work 已取得异常归档权 |
| `EXECUTION` | 可选的一轮执行一致性检查 |

异常状态与 Properties 分开，是因为它们可能被多个线程并发传播和读取。

---


## 125. Payload / Invoker：类型擦除、SBO 与分派入口

![125. Payload / Invoker：类型擦除、SBO 与分派入口 - 视觉摘要](assets/supp_125.png)

*图：125. Payload / Invoker：类型擦除、SBO 与分派入口 的关键对象、核心规则与边界关系*


调度器通过 Work 的 Payload 做类型擦除：

Executor 不需要知道用户 callable 的具体类型，只调用统一：

```text
w->invoke(worker, executor, cache)
```

具体执行语义由 Invoker 决定。

---


## 126. Placeholder：只有依赖传播，没有用户 callable

![126. Placeholder：只有依赖传播，没有用户 callable - 视觉摘要](assets/supp_126.png)

*图：126. Placeholder：只有依赖传播，没有用户 callable 的关键对象、核心规则与边界关系*


Placeholder：

- 不保存用户 callable；
- 是 strong predecessor；
- 执行时直接走普通 `_tear_down_task`；
- 适合纯依赖结构节点。

---


## 127. Basic：统一执行管线的最小任务类型

![127. Basic：统一执行管线的最小任务类型 - 视觉摘要](assets/supp_127.png)

*图：127. Basic：统一执行管线的最小任务类型 的关键对象、核心规则与边界关系*


Basic 的标准执行链：

Basic 是其他同步任务类型理解的基准。

---


## 128. Branch：选择一个普通 strong 后继



Branch 在 callable 执行期间构造栈绑定 `Branch` Context：

![Branch 目标存储](assets/struct_branch.svg)

*图：Branch 目标存储*

用户可以：

```text
select(index)
select_if(pred)
reset()
branch[index] = true/false
```

每轮最多选择一个目标。

Branch 仍属于普通 strong dependency 语义。

---

## 129. MultiBranch：选择多个普通 strong 后继



MultiBranch 允许选择零到多个后继。

![MultiBranch 目标存储](assets/struct_multibranch.svg)

*图：MultiBranch 目标存储*

选择集合采用：

- 线性去重；
- swap-and-pop 删除；
- 小集合优先。

tear-down 只推进本轮选中的目标集合。

---

## 130. Jump：绕过普通 strong join 的强制激活

![130. Jump：绕过普通 strong join 的强制激活 - 视觉摘要](assets/supp_130.png)

*图：130. Jump：绕过普通 strong join 的强制激活 的关键对象、核心规则与边界关系*


Jump 与 Branch 的关键区别：

> Jump 不是普通 strong predecessor，而是显式激活控制目标。

因此 Jump 可以构建控制循环，而不会被普通 DAG strong join 规则限制。

---


## 131. MultiJump：多个目标的强制激活与 parent slot

![131. MultiJump：多个目标的强制激活与 parent slot - 视觉摘要](assets/supp_131.png)

*图：131. MultiJump：多个目标的强制激活与 parent slot 的关键对象、核心规则与边界关系*


MultiJump 是多目标显式激活：

目标由执行期 `MultiJump` 临时收集，完成后通过专用 jump tear-down 激活。

---


## 132. Branch vs Jump：两个控制流语义不能混用

![132. Branch vs Jump：两个控制流语义不能混用 - 视觉摘要](assets/supp_132.png)

*图：132. Branch vs Jump：两个控制流语义不能混用 的关键对象、核心规则与边界关系*


| | Branch | Jump |
|---|---|---|
| 运行期选目标 | 是 | 是 |
| 目标数量 | 0..1 | 0..1 |
| Multi 版本 | `MultiBranch` | `MultiJump` |
| 参与普通 strong join | 是 | 否 |
| 主要用途 | 条件依赖传播 | 显式控制流 / 循环 |
| 完成路径 | branch tear-down | jump tear-down |

可以把它理解为：

```text
Branch = dependency routing
Jump   = control-flow activation
```

---


## 133. SubFlow：执行期构图但仍复用 Graph/Work 内核

![133. SubFlow：执行期构图但仍复用 Graph/Work 内核 - 视觉摘要](assets/supp_133.png)

*图：133. SubFlow：执行期构图但仍复用 Graph/Work 内核 的关键对象、核心规则与边界关系*


SubFlow 节点内部按值保存一个 Graph。

执行期给 callable 注入 `SubFlow&`，用户在回调内动态构建内部图。

---


## 134. SubFlow::run：动态 Graph 如何接入 parent slot

![134. SubFlow::run：动态 Graph 如何接入 parent slot - 视觉摘要](assets/supp_134.png)

*图：134. SubFlow::run：动态 Graph 如何接入 parent slot 的关键对象、核心规则与边界关系*


Child Work 共用当前父 Topology，并以当前 SubFlow Work 为 parent。

---


## 135. SubFlow::wait：协作等待而不是阻塞 Worker

![135. SubFlow::wait：协作等待而不是阻塞 Worker - 视觉摘要](assets/supp_135.png)

*图：135. SubFlow::wait：协作等待而不是阻塞 Worker 的关键对象、核心规则与边界关系*


SubFlow wait 不让当前 Worker 单纯 OS 阻塞。

这属于协作式等待：

> 等待线程本身继续帮助执行任务。

---


## 136. Module / Graph Work：子图作为一个可调度 Work

![136. Module / Graph Work：子图作为一个可调度 Work - 视觉摘要](assets/supp_136.png)

*图：136. Module / Graph Work：子图作为一个可调度 Work 的关键对象、核心规则与边界关系*


`TaskType::Graph` 统一表示图型执行语义。

Module 通过 Graph holder 获取目标子图，并支持：

- 执行一次；
- 固定次数循环；
- predicate 控制循环；
- callback 完成处理。

核心模型：

Graph-family Work 使用 `PREEMPTED + join_counter` 管理内部图完成和父恢复。

---


## 137. Runtime：动态提交的执行上下文



Runtime 是当前 Work 执行期间的动态调度入口。

![Runtime 执行上下文](assets/struct_runtime.svg)

*图：Runtime 执行上下文*

可用于：

- run / corun Graph；
- silent_async；
- async；
- 动态派生 Runtime / SubFlow 任务；
- 与 TaskGroup 协作。

---

## 138. Runtime child：创建、发布与父计数

![138. Runtime child：创建、发布与父计数 - 视觉摘要](assets/supp_138.png)

*图：138. Runtime child：创建、发布与父计数 的关键对象、核心规则与边界关系*


提交 child 前必须先建立父完成计数：

```text
parent.join_counter += 1
        ↓
schedule child
```

不能反过来，否则 child 可能快速完成并让 parent 提前归零。

---


## 139. PREEMPTED：Runtime / Graph-family 的挂起与恢复

![139. PREEMPTED：Runtime / Graph-family 的挂起与恢复 - 视觉摘要](assets/supp_139.png)

*图：139. PREEMPTED：Runtime / Graph-family 的挂起与恢复 的关键对象、核心规则与边界关系*


恢复条件：

```text
parent.join_counter.fetch_sub(1) == 1
AND
PREEMPTED
```

满足后 parent 直接进入 cache。

---


## 140. Topology：一个异步/执行实例的控制块



Topology 是一次独立执行的运行控制块。

核心成员：

![Topology 控制块](assets/struct_topology.svg)

*图：Topology 控制块*

Control 同时保存：

```text
STOP_REQUESTED | LOCKED | STATUS | USE_COUNT
```

---

## 141. Topology 状态机：Idle → Running → Finished

![141. Topology 状态机：Idle → Running → Finished - 视觉摘要](assets/supp_141.png)

*图：141. Topology 状态机：Idle → Running → Finished 的关键对象、核心规则与边界关系*


`Finished` 表示：

> 本次执行结果和完成状态已经发布。

它不表示：

> Work 已销毁。

---


## 142. Topology::Control 位域：Status、LOCKED、stop、use_count



![Topology Control 位域](assets/struct_topology_control.svg)

*图：Topology Control 位域*

- `STOP_REQUESTED`：协作停止；
- `LOCKED`：动态依赖边表独占；
- `Status`：Idle / Running / Finished；
- `use_count`：强引用数量。

---

## 143. Topology::_wait：完成发布与 `atomic::wait`

![143. Topology::_wait：完成发布与 `atomic::wait` - 视觉摘要](assets/supp_143.png)

*图：143. Topology::_wait：完成发布与 `atomic::wait` 的关键对象、核心规则与边界关系*


同步等待：

```text
load control(acquire)

while status != Finished:
    atomic::wait(old)
    reload

Finished 发布:
store(..., release)
notify_all()
```

因此 Future 等待不需要额外 condition_variable。

---


## 144. Stop propagation：沿 Topology parent 链惰性传播

![144. Stop propagation：沿 Topology parent 链惰性传播 - 视觉摘要](assets/supp_144.png)

*图：144. Stop propagation：沿 Topology parent 链惰性传播 的关键对象、核心规则与边界关系*


Topology 可形成父链：

`_stop_requested()`：

1. 从当前 Topology 开始检查；
2. 沿 `m_parent` 向上；
3. 发现祖先停止后，把 STOP_REQUESTED 惰性缓存到当前 Topology。

停止是协作式的，不强制中断正在运行的 callable。

---


## 145. AsyncFuture：等待、结果与强引用



`AsyncFuture<R>` 保存：

![AsyncFuture 句柄结构](assets/struct_future.svg)

*图：AsyncFuture 句柄结构*

职责：

- 持有一份 Work 强引用；
- wait；
- get / result；
- 请求停止；
- 查询完成状态。

---

## 146. AsyncTaskObject：业务对象地址与任务强引用分离



![AsyncTaskObject 结构](assets/struct_async_object.svg)

*图：AsyncTaskObject 结构*

业务对象仍实际存放在 Work Payload 内。

---

## 147. Async 强引用来源：Handle / Execution / Dependency / Parent

![147. Async 强引用来源：Handle / Execution / Dependency / Parent - 视觉摘要](assets/supp_147.png)

*图：147. Async 强引用来源：Handle / Execution / Dependency / Parent 的关键对象、核心规则与边界关系*


一个 Async Work 可能同时被以下主体持有：

因此：

```text
Finished ≠ use_count == 0
Finished ≠ destroyed
```

---


## 148. `_destroy_async`：迭代释放前驱引用

![148. `_destroy_async`：迭代释放前驱引用 - 视觉摘要](assets/supp_148.png)

*图：148. `_destroy_async`：迭代释放前驱引用 的关键对象、核心规则与边界关系*


当最后一份强引用释放：

实现不递归，因此依赖链再深也不会按依赖深度增长 C++ 调用栈。

---


## 149. 为什么零引用回收阶段可以临时借用 `m_parent`

![149. 为什么零引用回收阶段可以临时借用 `m_parent` - 视觉摘要](assets/supp_149.png)

*图：149. 为什么零引用回收阶段可以临时借用 `m_parent` 的关键对象、核心规则与边界关系*


进入 `_destroy_async()` 的前提：

- Work 强引用已经归零；
- 当前线程拥有唯一销毁权；
- Work 不再执行；
- Work 不再调度；
- 原运行期 parent 关系不再被正常执行逻辑使用。

因此 `m_parent` 可临时作为：

```text
zero-ref recycle chain next
```

不需要再分配额外链表节点。

---


## 150. TaskGroup：栈绑定 AnchorWork 聚合动态任务

![150. TaskGroup：栈绑定 AnchorWork 聚合动态任务 - 视觉摘要](assets/supp_150.png)

*图：150. TaskGroup：栈绑定 AnchorWork 聚合动态任务 的关键对象、核心规则与边界关系*


TaskGroup 是当前 Context 中动态任务的作用域聚合器。

AnchorWork 汇总：

- 完成计数；
- 异常；
- 停止域；
- child parent 关系。

---


## 151. TaskGroup 提交顺序：先建 slot，再发布 Work

![151. TaskGroup 提交顺序：先建 slot，再发布 Work - 视觉摘要](assets/supp_151.png)

*图：151. TaskGroup 提交顺序：先建 slot，再发布 Work 的关键对象、核心规则与边界关系*


每个 child 发布前：

```text
anchor.join_counter += 1
        ↓
schedule child
```

这样即使 child 立即执行完成，也不会在 parent slot 尚未建立时错误归零。

---


## 152. TaskGroup::wait：当前 Worker 继续参与调度

![152. TaskGroup::wait：当前 Worker 继续参与调度 - 视觉摘要](assets/supp_152.png)

*图：152. TaskGroup::wait：当前 Worker 继续参与调度 的关键对象、核心规则与边界关系*


析构也使用协作等待，保证作用域离开前 child 已完成。

---


## 153. Implicit Anchor：异常归档的就近边界

![153. Implicit Anchor：异常归档的就近边界 - 视觉摘要](assets/supp_153.png)

*图：153. Implicit Anchor：异常归档的就近边界 的关键对象、核心规则与边界关系*


某些能承接内部子执行的 Work 具备：

```text
Properties::IMPLICIT_ANCHOR
```

当不存在更近的显式锚点时，异常可以在父链上的隐式锚点归档。

---


## 154. ScopedExceptionAnchor：显式异常作用域

![154. ScopedExceptionAnchor：显式异常作用域 - 视觉摘要](assets/supp_154.png)

*图：154. ScopedExceptionAnchor：显式异常作用域 的关键对象、核心规则与边界关系*


`ScopedExceptionAnchor` 在当前 Work 设置：

```text
Control::EXPLICIT_ANCHOR
```

生命周期是词法作用域。

严格嵌套的多个 anchor 不会提前清掉外层锚点。

---


## 155. Observer：before / after 可能跨 Worker

![155. Observer：before / after 可能跨 Worker - 视觉摘要](assets/supp_155.png)

*图：155. Observer：before / after 可能跨 Worker 的关键对象、核心规则与边界关系*


TaskObserver 提供：

```text
on_before(WorkerView)
on_after(WorkerView)
```

执行链：

对于 Runtime / Graph-family 的挂起恢复：

> `on_before` 与最终 `on_after` 可能发生在不同 Worker。

因此 Observer 不能假设整轮执行始终绑定同一 thread_local 状态。

---


## 156. D2Renderer：只负责可视化，不参与调度

![156. D2Renderer：只负责可视化，不参与调度 - 视觉摘要](assets/supp_156.png)

*图：156. D2Renderer：只负责可视化，不参与调度 的关键对象、核心规则与边界关系*


D2Renderer 只负责可视化导出：

它不参与：

- 调度；
- join；
- 生命周期；
- Work 所有权。

---


# Part XI — Executor-Centric Algorithm Orchestration

站在 `Executor` 视角，把各个局部算法按真实调用顺序重新编排。图页采用以前版本中效果较好的算法板式；正文下面的“当前实现校正”以 `main@58eedba...` 为准。


## 157. Executor：算法编排总览

Executor 不只是“线程池对象”，而是 Ready 发布、Worker 搜索、执行接力、完成传播和生命周期计数的算法编排器。

![Executor-Centric 01](assets/exec_01.png)


## 158. Executor 入口分类

先分清调用线程和执行类型，后续算法选择才不会混在一起。

![Executor-Centric 02](assets/exec_02.png)


## 159. Graph 提交事务

Graph 从静态定义进入运行态，需要先 setup、建立 parent 完成计数，再发布 source。

![Executor-Centric 03](assets/exec_03.png)


## 160. _set_up_graph：图初始化循环

一次线性扫描完成本轮 parent/topology 绑定、异常清理、join-weight 计算和 source 前缀聚集。

![Executor-Centric 04](assets/exec_04.png)


## 161. Ready Work 发布策略

Executor 按 cache → local queue → shared stack 的顺序尽量缩短 Ready 到执行的路径。

![Executor-Centric 05](assets/exec_05.png)


## 162. _schedule(Worker&, Work*)

Worker 内单任务发布优先进入本地队列；队列满才溢出到共享分片。

![Executor-Centric 06](assets/exec_06.png)


## 163. _schedule(Worker&, first, n)

批量发布保留批量语义：本地填满前缀，剩余区间一次转交共享调度。

![Executor-Centric 07](assets/exec_07.png)


## 164. _push_shared：共享分片选择

共享发布通过 Work 地址哈希选择 SharedWorkStack，降低所有 Producer 争用一个原子 head。

![Executor-Centric 08](assets/exec_08.png)

**当前实现校正：** 当前实现中，单 Work `_push_shared(work)` 用 Work 地址哈希选择分片；批量路径不是简单重复哈希：`n <= shard_count` 时从哈希起点轮转分片，`n > shard_count` 时按 `base/extra` 均匀切块后批量发布。


## 165. Worker 主循环

Worker 热路径是 _invoke cache 链，然后 local pop；只有两者都空才进入 _wait_for_work。

![Executor-Centric 09](assets/exec_09.png)


## 166. _wait_for_work：搜索状态机

空闲 Worker 先积极窃取，再 yield，最后才进入 Notifier 两阶段等待。

![Executor-Centric 10](assets/exec_10.png)

**当前实现校正：** `vtm` 的索引空间同时覆盖 `m_workers[]` 与 `m_shared_stacks[]`。准备睡眠后会先扫描全部共享分片，再扫描除自身外的 Worker 本地队列；只有复查仍无工作才 `commit_wait`。


## 167. Executor × Notifier：休眠边界

Executor 负责业务条件复查；Notifier 只负责把“准备睡/已经睡/已经通知”的竞态关闭。

![Executor-Centric 11](assets/exec_11.png)


## 168. _invoke：cache handoff 执行链

第一个 Ready successor 可直接成为下一个 w，在同一 Worker、同一调用栈继续执行。

![Executor-Centric 12](assets/exec_12.png)


## 169. Basic Work：Executor 看到的 invoke 管线

Executor 不关心 callable 类型，但必须理解 Semaphore、Observer、Exception 和 tear-down 在执行链中的位置。

![Executor-Centric 13](assets/exec_13.png)


## 170. _tear_down_task：完成传播核心

完成传播的本质是：恢复自身下一轮 join 状态，再为后继竞争 Ready ownership。

![Executor-Centric 14](assets/exec_14.png)


## 171. 多 Ready 后继：parent slot 编排

cache successor 继承已有 slot；额外 Ready successor 必须先增加 parent 计数再发布。

![Executor-Centric 15](assets/exec_15.png)


## 172. 专用 tear-down 选择

Basic、Branch、Jump 的差异集中在目标集合和 Ready 规则，Executor 的后半段完成协议保持一致。

![Executor-Centric 16](assets/exec_16.png)


## 173. _schedule_parent：父完成权

child 无后继接力时归还 parent slot；旧值 1 的线程获得父继续执行或完成的唯一权。

![Executor-Centric 17](assets/exec_17.png)


## 174. _corun_until：协作等待

Worker 等待动态子执行完成时继续执行调度器中的 Work，而不是进入 Notifier park。

![Executor-Centric 18](assets/exec_18.png)


## 175. Semaphore：阻塞但不占 Worker

资源不足的 Work 进入 Semaphore waiter 链；Worker 立即去执行其他任务。

![Executor-Centric 19](assets/exec_19.png)


## 176. _launch_async：异步提交事务

异步提交把 Future 生命周期、dependency 引用、Running 状态、执行引用和 submission guard 一次建立完整。

![Executor-Centric 20](assets/exec_20.png)

**当前实现校正：** 当前立即 async、TaskGroup async 与 `AsyncTask::start` 都保留一份提交保护计数；依赖登记结束后最后释放 guard，取得 `1 -> 0` 的线程负责发布目标任务。


## 177. _link_predecessors：完成竞态

Executor 用 predecessor Topology 的 LOCKED，把“注册后继”和“已经完成”变成互斥的两种结果。

![Executor-Centric 21](assets/exec_21.png)


## 178. Async completion：Finished 与销毁

执行完成只发布 Finished；真正销毁必须等 Future、依赖和执行引用全部释放。

![Executor-Centric 22](assets/exec_22.png)

**当前实现校正：** Finished 的发布和 Work 的销毁是两条独立轴：完成线程先冻结动态后继并通知等待者，再释放 execution ref；只有最后一份强引用释放者才进入 `_destroy_async`。


## 179. Executor 全局 Topology 计数

m_num_topologies 跟踪顶层活动执行链，为 wait_for_all 和安全 shutdown 提供统一门槛。

![Executor-Centric 23](assets/exec_23.png)

**当前实现校正：** `m_num_topologies` 统计顶层活动执行链，用于 `wait_for_all()` 与 Executor 析构门槛；嵌套 child 的完成责任由 parent slot/AnchorWork 维护。


## 180. Executor shutdown：停止顺序

销毁 Executor 时必须先让所有活动 topology 完成，再终止 Worker。

![Executor-Centric 24](assets/exec_24.png)


## 181. Static Graph：端到端时间线

把静态 Graph 从调用者提交到 topology 完成按 Executor 的真实调用顺序串起来。

![Executor-Centric 25](assets/exec_25.png)


## 182. Runtime / SubFlow：端到端时间线

动态子执行增加 parent slot，必要时 PREEMPT 当前 Work，最后由 _schedule_parent 恢复。

![Executor-Centric 26](assets/exec_26.png)


## 183. Async dependencies：端到端时间线

异步依赖最终仍转换为 join_counter 到达和普通 Work schedule，只是依赖边在运行期连接。

![Executor-Centric 27](assets/exec_27.png)


## 184. Executor 算法选择矩阵

最后用场景表把 Executor 在不同状态下调用的核心算法与数据结构对应起来。

![Executor-Centric 28](assets/exec_28.png)


# Part XII — Algorithm Visual Atlas

这一部分不再按类组织，而是按“结构快照 → 操作步骤 → 并发交错 → 线性化点 / happens-before → 不变量”组织。它用于快速审查算法，而不是重复前文 API 说明。


## 185. 图形化算法文档标准

每个核心算法采用 4 层 zoom：架构边界、数据结构快照、动态协作、并发正确性。关系必须有标签，操作必须编号，关键原子操作直接标 memory order。

![Algorithm Visual Atlas 01](assets/atlas_02.png)


## 186. BoundedQueue：角色、环形布局与访问权

先固定 Owner/Stealer 权限，再解释 top/bottom 和物理槽位。这样后续 pop/steal 的 CAS 竞争不会失去上下文。

![Algorithm Visual Atlas 02](assets/atlas_03.png)


## 187. BoundedQueue::push：发布与可见性

把状态快照和 happens-before 放在同一页：slot 的 relaxed 写不是独立同步点，真正的发布点是 bottom 的 release store。

![Algorithm Visual Atlas 03](assets/atlas_04.png)


## 188. BoundedQueue：最后一项竞争与线性化点

Owner pop 和 Stealer steal 在只剩一个元素时竞争同一个 m_top；CAS 是唯一所有权的线性化点。

![Algorithm Visual Atlas 04](assets/atlas_05.png)


## 189. SharedWorkStack：Producer/Consumer 双域

把 m_incoming、m_state、m_head 分成两个所有权域，明确 producer 不获取消费锁，只有 LOCKED owner 可以访问 m_head。

![Algorithm Visual Atlas 05](assets/atlas_06.png)


## 190. SharedWorkStack::push：计数与原子发布

当前实现先增加 size，再用 release CAS 发布 incoming，因此 size 是调度观察计数，不等同于“此刻一定可成功 steal 的节点数”。

![Algorithm Visual Atlas 06](assets/atlas_07.png)


## 191. SharedWorkStack::steal：消费端独占竞争

两个 thief 同时看到非空也只有一个能拿到 LOCKED；失败 thief 立即返回，不在一个共享分片上等待。

![Algorithm Visual Atlas 07](assets/atlas_08.png)


## 192. Executor::_wait_for_work：三阶段搜索算法

将快速窃取、steal+yield、Notifier prepare/commit 三阶段拆开，明确预算、victim 空间和休眠前 double-check。

![Algorithm Visual Atlas 08](assets/atlas_09.png)


## 193. Cache Handoff：调度器旁路

第一个 Ready successor 直接写入 cache，绕过 push/pop round-trip；这是 TaskflowLite 保持当前 Worker locality 的关键热路径。

![Algorithm Visual Atlas 09](assets/atlas_10.png)


## 194. _tear_down_task：完整数据流

同时展示自身 join 恢复、successor join 到达、Ready ownership、ready-prefix 聚集和 parent slot 建立顺序。

![Algorithm Visual Atlas 10](assets/atlas_11.png)


## 195. Parent Slot Conservation：0 / 1 / N 三种情况

用三个状态快照解释为什么一个 cache successor 能继承当前 slot，而额外 successor 必须先增加 parent 计数再发布。

![Algorithm Visual Atlas 11](assets/atlas_12.png)


## 196. Notifier：Lost Wake-up 与两阶段关闭窗口

先画错误 interleaving，再画 prepare -> double-check -> cancel/commit，读者可以直接看到通知为什么不会被永久错过。

![Algorithm Visual Atlas 12](assets/atlas_13.png)


## 197. Notifier：全局状态字与 Waiter 三态机

把全局 64-bit eventcount 状态与单 Worker Waiter 的 park/unpark 状态分开，不混在一张“等待流程图”里。

![Algorithm Visual Atlas 13](assets/atlas_14.png)


## 198. Semaphore：Multi-acquire 事务

在统一锁顺序下先检查全部请求，再统一扣减；失败路径不需要回滚已检查 quota，并只挂到第一个 blocker。

![Algorithm Visual Atlas 14](assets/atlas_15.png)


## 199. ObjectPool：热/冷路径与 ABA

把 Bucket/FreeStack/Slab 的所有权和 tagged-head 的时间线放在一起，说明 tag 为什么必须参与 CAS。

![Algorithm Visual Atlas 15](assets/atlas_16.png)


## 200. AsyncTask::start：提交事务与发布边界

把 start 拆成校验、准备、guard、Running 发布、连接依赖、释放 guard 六阶段，并标出发布边界前后的异常语义。

![Algorithm Visual Atlas 16](assets/atlas_17.png)


## 201. _link_predecessors：两种合法并发交错

Link First 和 Finish First 并排展示；无论哪种先发生，一个 dependency 到达都只能计数一次。

![Algorithm Visual Atlas 17](assets/atlas_18.png)


## 202. _destroy_async：迭代引用回收

图中区分当前对象销毁和前驱引用释放；零引用前驱挂入临时 recycle chain，从而避免递归释放。

![Algorithm Visual Atlas 18](assets/atlas_19.png)


## 203. Runtime / Graph-family：PREEMPTED 恢复

从建立 child slot、置 PREEMPTED、child 完成，到 _schedule_parent 将 parent 放入 cache，完整画出一次挂起/恢复。

![Algorithm Visual Atlas 19](assets/atlas_20.png)


## 204. Topology：Finished 与 Destroyed 的正交关系

执行状态与强引用计数是两个维度。Finished 只发布完成，最终销毁还要等待全部 handle/execution/dependency 引用释放。

![Algorithm Visual Atlas 20](assets/atlas_21.png)


## 205. End-to-End Synchronization Map

最后把 publish、steal、invoke、successor join、cache handoff、parent slot、Topology finish 串成一张跨组件同步地图。

![Algorithm Visual Atlas 21](assets/atlas_22.png)


# Appendix A — Core Source Map

| 主题 | 当前主要文件 |
|---|---|
| TaskType / Direction | `enums.hpp` |
| Task / TaskObject | `task.hpp`, `task_object.hpp` |
| Flow / FlowBuilder | `flow.hpp`, `flow_builder.hpp` |
| Graph / Work | `graph.hpp`, `work.hpp` |
| Work factories / Invokers | `work_factory.hpp`, `work_invokers.hpp` |
| Executor / Worker | `executor.hpp`, `worker.hpp` |
| Local work stealing | `bounded_queue.hpp` |
| Shared scheduling | `shared_work_stack.hpp` |
| Sleep / wake | `notifier.hpp` |
| Topology / Results | `topology.hpp`, `result_slot.hpp`, `work_storage.hpp` |
| Semaphore | `semaphore.hpp` |
| Async | `async_future.hpp`, `async_task.hpp`, `async_task_object.hpp` |
| Runtime / SubFlow / TaskGroup | `runtime.hpp`, `subflow.hpp`, `task_group.hpp` |
| Branch / Jump | `branch.hpp`, `jump.hpp` |
| Object pool / SmallVector / SpinMutex | `object_pool.hpp`, `small_vector.hpp`, `spin_mutex.hpp` |
| Observer / Exception anchor / D2 | `observer.hpp`, `scoped_exception_anchor.hpp`, `d2_render.hpp` |


# Appendix B — Correctness Checklist

审查任何调度优化时至少逐项回答：

1. **谁拥有写权限？** 是否仍满足 BoundedQueue 单 Owner、`m_head` gate owner、Work 执行期唯一所有者等阶段约束；
2. **发布点在哪里？** 普通写入由哪一个 release/acquire 或更强操作向另一线程公开；
3. **线性化点在哪里？** 最后一项 CAS、successor `fetch_sub==1`、Async Finished CAS 等是否仍唯一决定执行权；
4. **生命周期由谁托底？** Graph 所有权、Topology 强引用、parent slot、Executor 顶层计数是否在发布前已经建立；
5. **失败路径是否闭合？** 分配异常、动态插边异常、信号量等待、stop/exception 是否会遗留锁、引用或计数；
6. **睡眠前是否复查？** Notifier prepare 后必须重新检查可见工作；
7. **完成与销毁是否分离？** Finished 不能替代最后引用释放。


# Appendix C — Final Reading Map

- 想理解**线程间怎么抢任务**：Part I–III + Part IX；
- 想理解**Executor 为什么这样编排**：Part XI；
- 想检查**原子操作与并发竞态**：Part XII；
- 想理解**Work 从创建到回收**：Part 0-A、IV–VII、X；
- 想理解**Runtime / Async / Semaphore 为什么都能复用同一内核**：Part V–VI + Part XI–XII。

