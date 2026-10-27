# C++ Runtime：背压（有界队列）（Month 3 环节 7）

> 规格书 Month 3 · 项目对象 Queue/Executor · 2026-10-27
> 对应 408：操作系统 2.3 生产者-消费者问题（缓冲上限）、
> 计算机网络 5 传输层流控（TCP 接收窗口/滑动窗口）

## 目标

环节 4 实现了积压峰值统计（观察），现在把数据变成**控制**：
队列积压达到容量上限时，拒绝新任务——生产者被"压回去"，
系统不会无限堆积内存。

## 背压的两种语义

| 语义 | 行为 | 对应 |
| --- | --- | --- |
| 阻塞式 | 队列满时生产者等待（条件变量阻塞） | 408 生产者-消费者经典解法：缓冲满则等待 |
| 拒绝式 | 队列满时 push 返回 false（丢弃/拒绝） | 有界缓冲丢弃策略；TCP 拥塞控制丢包/限速 |

本实现选**拒绝式**：返回 false + rejected 计数（Telemetry 可观测），
不阻塞调用方——线程池语义里"投不进去就自己处理"更实用。

## 实现

```
TaskQueue(aging_interval, aging_max_boost, max_capacity)
  push(): 锁内检查 queue_.size() >= max_capacity_ → return false
  max_capacity_ = 0 → 无限制（默认，历史行为不变）
Executor(num_workers, aging_interval, aging_max_boost, max_capacity)
  submit() 失败时 record_rejected()（环节 3 已有计数）
```

容量检查在**锁内**做：size 读和 insert 在同一临界区，
杜绝"检查时没满、插入时已满"的竞态。

## 与水位峰值（环节 4）的关系

- 环节 4 的 peak_pending = 观察"曾经积压多深"（诊断用）
- 环节 7 的 max_capacity = 控制"最多允许积压多深"（防护用）
- 两者合起来 = 监控 + 治理闭环：
  peak_pending 接近 max_capacity → 需要扩容 worker 或限速

## 验证

- test_queue +4 用例（累计 9）：
  - 容量满时 push 被拒（拒绝式背压）
  - 默认无容量限制（回归）
  - pop 释放容量后可继续入队
  - Executor 集成：容量 3 时第 4 个 submit 被拒且 rejected==1
- 全量回归：40 用例全过（queue 9 + task 4 + executor 5 +
  telemetry 12 + priority 10），CTest 5/5，编译零警告
- demo 实测（容量 3，单 worker 被 30ms 任务占住）：

```
提交 6 个，入队 2 个，拒绝 4 个（积压峰值 3）
```

## 408 考点映射

| 本实现 | 408 概念 |
| --- | --- |
| 有界队列（容量上限） | 生产者-消费者：有限缓冲（bounded buffer） |
| 满则拒绝 | 缓冲满处理策略之一（丢弃）；TCP 接收窗口=0 时停发 |
| 锁内检查容量 | 临界区完整性：检查与操作原子化，避免 TOCTOU |
| rejected 计数 | 流控的丢弃统计；拥塞信号 |
| 监控（峰值）+ 治理（上限） | 反馈控制：测量 → 阈值 → 动作 |

## 下一步候选

- 阻塞式背压：队列满时 submit 阻塞等待（条件变量 + 容量条件）
  ——补全 408 生产者-消费者"满则等、空则等"的完整语义
- 动态扩容：peak_pending 持续超阈值时自动增加 worker
- 超时背压：阻塞等待最多 N ms，超时返回 false