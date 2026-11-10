# C++ Runtime：任务句柄与异常安全（Month 3 环节 15）

> 规格书 Month 3 · Task 生命周期治理 · 2026-11-10
> 对应 408：条件变量 / 信号量 P-V / 带超时等待

## 目标

到环节 14 为止，`submit` 只是"扔进去"：调用方拿不回结果，也不知道任务
何时干完。真实场景需要三件事：

1. **等任务完成**（依赖关系：B 依赖 A 的结果）
2. **带超时地等**（不能永远卡住）
3. **知道结果类型**（正常完成 / 被取消 / 抛异常 / 被拒绝）

顺带补上一个真 bug：此前任务体抛异常会穿出线程入口 → `std::terminate`
→ 整个进程退出（线程池一碰异常就崩）。

## 设计

### TaskCompletion：共享完成状态

```
mutex m_ + condition_variable cv_ + TaskOutcome outcome_ = Pending
complete(o)   : 锁内置结果（幂等，只有第一次生效）→ 解锁 notify_all
state()       : 锁内读（不阻塞）
wait()        : 锁内 cv.wait(..., 谓词 "outcome != Pending")
wait_for(ms)  : cv.wait_for → bool（超时 false）
```

`complete()` 幂等很重要：执行完成路径与"提交被拒"路径可能先后写同一个
状态，先到的赢，后到的直接返回，不会把 Done 覆盖成 Rejected。

### TaskHandle：调用方持有的轻量句柄

- 默认构造 = 无效句柄（`valid() == false`），`wait()` 直接返回 Pending 不卡死
- 复制句柄共享同一完成状态（底层 `shared_ptr<TaskCompletion>`）

### 结果枚举（TaskOutcome）

| 值 | 含义 | 产生位置 |
| --- | --- | --- |
| Pending | 未结束（排队/执行中） | 初始值 |
| Done | 正常执行完毕（含空任务） | `Task::run()` 末尾 |
| Skipped | 协作式取消，执行前跳过 | 环节 14 令牌路径 |
| Failed | 任务体抛异常（已捕获） | `catch (...)` |
| Rejected | 提交时未入队（关闭/满） | `submit_handle` |

### Executor::submit_handle

```cpp
TaskHandle submit_handle(Task task);
```

流程：new TaskCompletion → `task.attach_completion()` → 复用 `submit()`
（令牌补挂 / 水位 / 扩容 / 拒绝计数全部继承，不重复实现）→ 若 `submit`
返回 false，**立刻把句柄置为 Rejected**。这一步不能省：否则调用方
`wait()` 会永远等一个从未入队的任务。

### Task::run() 的三分支

```
should_stop()           → complete(Skipped)，返回 false
!work_                  → complete(Done)，返回 true（空任务视为完成）
try { work_(); }        → complete(Done)，返回 true
catch (...)             → complete(Failed)，返回 false（异常不外泄）
```

`last_outcome_` 同步记录本地结果，Worker 据此区分 Skipped / Failed
（两者 run() 都返回 false）并分别计入 `telemetry.skipped()` /
`telemetry.failed()`。

### Worker / WorkStealingPool 口径统一

两个执行器都改为 `if (!task->run()) continue;` —— 取消/失败的任务
**不执行、不计执行数、不计耗时**，避免吞吐指标被污染。
（WorkStealingPool 的 stolen 计数也只在真正执行时 +1。）

## 408 考点映射

| 本实现 | 408 概念 |
| --- | --- |
| `cv.wait(lock, 谓词)` | 条件变量 + while 循环检查（防虚假唤醒/丢失唤醒） |
| `complete()` 置结果 + `notify_all` | 信号量 V 操作（唤醒全部等待者） |
| `wait()` | P 操作（无资源则阻塞） |
| `wait_for(timeout)` | 带超时的 P 操作（超时不阻塞，返回失败） |
| 完成状态 + 句柄 | 进程同步：父进程 wait 子进程结束、回收结果 |
| 异常在任务边界被捕获 | 进程/线程边界的错误隔离（一个任务失败不拖垮整个池） |

## 验证

- test_executor 27 用例（+5，原 22 个全部通过）：
  - HandleDoneAfterExecution：`wait()` → Done，且任务副作用生效
  - HandleSkippedAfterRequestStop：取消跳过的任务句柄 → Skipped（不挂死）
  - HandleRejectedAfterShutdown：提交到已关闭池 → Rejected（立刻就绪）
  - HandleWaitForTimeout：`wait_for(30ms)` 返回 false、状态仍 Pending，
    完成后再 wait → Done，且 `wait_for` 立即 true
  - ExceptionTaskMarkedFailedAndPoolSurvives：抛异常任务 → Failed、
    `telemetry.failed()==1`、`tasks_done()==0`，随后提交的正常任务照常完成
- 全量 **76 用例全过**（queue 15 + task 4 + executor 27 + telemetry 12
  + priority 10 + work_stealing 8），CTest 6/6，零警告
- demo 实测（新增"任务句柄演示"段）：

```
  h1 求和任务：wait() → Done（完成），结果 sum=5050
  h2 慢任务：wait_for(50ms) 超时（判未完成）；最终 → Done（完成）
  h3 抛异常任务：→ Failed（抛异常）（异常被捕获，线程存活）
  统计：完成 2、失败 1、拒绝 0
```

## 踩坑清单

1. **const 方法里调 `cv_.wait_for` 编译不过**（"转换丢失限定符"）：
   条件变量的 wait 系列是非 const 成员函数，`cv_` 必须声明 `mutable`
   才能在 `wait() const` 里使用。mutex 同理。
2. **被拒任务必须补写 Rejected**：否则句柄永远 Pending，调用方
   `wait()` 死等——"句柄假活"是这类 API 最典型的坑。
3. **异常不能让 Worker 记成 Skipped**：两者 run() 都返回 false，
   必须靠 `last_outcome()` 区分，否则失败率统计失真。
4. `complete()` 幂等：否则"被拒后又被执行"的极端交错会覆盖真实结果。

## 下一步候选

- 双端无锁本地队列（owner LIFO + 偷取 FIFO，ForkJoinPool 双索引模式）
- 统一线程池 API（Executor 与 WorkStealingPool 合并，策略可配）
- 任务依赖图（DAG 调度：句柄作为后继任务的输入）