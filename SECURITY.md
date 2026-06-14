# Security Policy

## Supported Versions

| Version | Supported          | Notes                          |
| ------- | ------------------ | ------------------------------ |
| 2.0.x   | :white_check_mark: | 当前稳定版,接受安全修复       |
| 1.0.x   | :warning:          | 仅关键安全修复,建议升级到 2.x |
| < 1.0   | :x:                | 不再维护                       |

## Scope

TaskflowLite 是一个 header-only 的并发任务调度库,没有运行时服务或网络接口。
本策略关注的"安全问题"主要指可能导致**未定义行为或内存损坏**的缺陷:

- 无锁数据结构(Chase-Lev deque、`FreeStack48/128` 等)的正确性缺陷,
  例如 ABA、释放后使用、错误的内存序导致的数据竞争。
- 内存池(`SegmentedPool` / `ObjectPool`)的越界、双重释放、对齐错误。
- 任务图执行中导致悬垂引用或异常传播失控的逻辑缺陷。
- 构建配置(CMake / 宏)在受支持平台上触发 UB 的问题。

## Reporting a Vulnerability

请**不要**直接公开 issue。可通过以下任一私密渠道上报:

1. **GitHub 私密上报**(推荐):仓库 → Security → "Report a vulnerability"。
2. **邮件**:qq978358810@gmail.com

上报时请尽量包含:

- 受影响的版本与平台(OS / 编译器 / 标准库)。
- 最小可复现示例,以及触发所需的线程数 / 调度场景。
- 若由 sanitizer 发现,请附上 ASan / TSan / UBSan 报告。

## Response Process

| 阶段       | 目标时限       |
| ---------- | -------------- |
| 确认收到   | 3 个工作日内   |
| 初步评估   | 7 个工作日内   |
| 修复并发布 | 视严重程度而定 |

采用**协调披露**:修复发布前请勿公开细节。修复合入后会在 release notes 与
本仓库的 Security Advisory 中致谢上报者(除非你要求匿名)。
