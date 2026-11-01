# C++ Runtime：急停（Month 3 环节 12）

> 规格书 Month 3 · Executor · 2026-11-01
> 对应 408：进程撤销（异常终止）；工程 = 线程池的"立即停止"语义

## 目标

环节 2 起 Executor 只有**优雅停机**（shutdown：close 队列 → 已入队任务
全部执行完 → Worker 自然退出）。但真实系统有时需要**立即停止**：
任务队列已无意义（比如后端要下线、数据源已断），留着慢慢消费是浪费。

环节 12 = `Executor::stop()`：**丢弃所有未执行任务，立即停止**。

## 设计

```
TaskQueue::stop()    : close + 清空队列（multiset::clear 整体丢弃）
Executor::stop()     : queue_.stop() → join 全部 Worker
幂等互斥            : 与 shutdown 共用 shutdown_done_ 标志
```

**为什么 stop 是"丢弃 + 等正在执行的跑完"，而不是"杀掉一切"**：

1. **C++ 线程不可抢占**：没有协作式中断的话，无法从外部打断一个正在
   执行的函数（没有 Thread.stop()，Java 的 stop() 也已废弃）。
   所以正在执行的任务必须让它跑完——这是急停的**物理下限**。
2. **可丢弃的是队列**：还没开始执行的任务都在队列里，stop 时
   `queue_.clear()` 一次清空，Worker 被唤醒后发现"队列空 + 已关闭"
   → pop 返回 nullopt → 退出。
3. **408 映射**：进程的**正常终止**（执行完退出，OS 清收资源）vs
   **撤销/异常终止**（立即回收 PCB、丢弃未完成工作）——stop 就是
   后者的线程池版。

**正确性要点**：
- 与 shutdown 幂等互斥：先 stop 后 shutdown / 先 shutdown 后 stop，
  第二次都是无操作（共用 shutdown_done_ 标志，队列只关一次）。
- stop 后 submit 被拒（closed_ = true，push 返回 false，且拒绝被
  Telemetry 记录——可观测性闭环）。
- 空转 worker（在 pop 里睡着）被 cv_.notify_all() 唤醒 → 退出，
  无需额外唤醒机制。

## 408 考点映射

| 本实现 | 408 概念 |
| --- | --- |
| shutdown（优雅） | 进程正常终止：执行完、OS 回收资源 |
| stop（急停） | 进程撤销：立即回收、丢弃未完成工作 |
| 正在执行任务不可打断 | 线程不可抢占（非抢占式调度） |
| 丢弃队列任务 | 撤销时清空未完成工作 |

## 验证

- test_executor +4 用例（累计 17）：
  - 急停丢弃排队任务（单 worker 占住 + started 事件同步，确定性地
    验证"正在执行的跑完、排队的一个不跑"）
  - stop/shutdown 幂等互斥（两种顺序都测）
  - 急停后 submit 拒绝（rejected 计数 +1）
  - 急停不打断正在执行的任务（100ms 慢任务跑完）
- 全量回归：65 用例全过（queue 15 + task 4 + executor 17 +
  telemetry 12 + priority 10 + work_stealing 7），CTest 6/6，
  编译零警告
- demo 实测：

```
排队 20 个任务，急停耗时 311ms，执行 1 个（丢弃 19）
（正在执行的慢任务不可抢占，必须等它跑完）
```

## 踩坑清单（本环节新增）

1. 测试竞态：stop 可能在任务还没被 Worker 取走时就执行，任务被
   误当作"排队任务"丢弃 → 用 started 事件（任务体里置位）保证
   "确定正在执行"后再 stop，断言才确定。

## 下一步候选

- 无锁本地队列（WorkStealingPool 的 LocalQueue 去 mutex，
  ForkJoinPool WorkQueue 双索引模式：owner 动 bottom、偷取者 CAS top）
- 协作式中断：给 Task 加"检查 stop 标志"的钩子（可抢占的妥协版）
- 统一线程池 API（Executor 与 WorkStealingPool 合并）