# C++ Runtime：协作式中断（Month 3 环节 14）

> 规格书 Month 3 · Task 生命周期治理 · 2026-11-10
> 对应 408：进程撤销的安全点 / 中断 vs 轮询

## 目标

环节 12 的 `stop()` 解决了"丢弃排队任务"，但留下一个明确的物理下限：
**正在执行的任务打不断**。C++（以及 Java、.NET）都没有安全的"外部杀线程"
手段——Java 的 `Thread.stop()` 早已废弃，因为它可能在任意指令处撕开栈，
让锁、堆、文件句柄留在不一致状态。

环节 14 = **协作式中断**：运行时只广播一个停止令牌，任务在**安全点**
（循环体、阶段之间）自查后主动 return。这是唯一安全的取消方向。

## 设计

### StopToken（新组件）

```
class StopToken {
  atomic<bool> stop_requested_   // 唯一状态
  request_stop()                 // store(true, release)，幂等
  is_stop_requested()            // load(acquire)
};
using StopTokenPtr = shared_ptr<StopToken>;
```

- 用 `shared_ptr` 共享：Executor、每个 Task、外部发起者同时持有，
  最后一个持有者释放时令牌才销毁，不会悬垂。
- release/acquire 内存序：置位前写入的状态对看到置位的线程可见。

### Task 的两层取消语义

| 时机 | 行为 | 统计 |
| --- | --- | --- |
| 执行前（排队中被取到） | `run()` 检查令牌，已置位 → 直接跳过任务体 | `telemetry.skipped() +1` |
| 执行中（任务体内） | 长循环调用 `should_stop()` → 自查后 return | 按正常执行计 |

`Task::run()` 返回 `bool`（是否真的执行了），`operator()` 保留兼容。
跳过判断放在 Task 而不是 Worker：任何持有 Task 的执行器
（Executor / WorkStealingPool / 未来的自定义执行器）自动获得同一套语义。

**为什么"执行前跳过"而不是"清队列"**：排队任务可能散落在优先队列/
环形队列里，清队列要全表扫描且与生产者竞争；惰性跳过零额外成本，
也天然覆盖"提交晚于 request_stop"的任务。

### Executor 的接口

```cpp
StopTokenPtr token() const;      // 共享令牌（任务体可持有自查）
void request_stop();             // 只广播：不清队列、不 join、立即返回
void stop();                     // 环节 12：先 request_stop() 再丢队列 + join
```

`submit` / `push_wait` 对**未携带令牌**的任务自动补挂本池令牌——
调用方不需要每次手动传 token。

### 与 stop() 的分工（关键语义）

| | request_stop() | stop() |
| --- | --- | --- |
| 广播令牌 | 是 | 是 |
| 丢弃排队任务 | 否（执行前惰性跳过） | 是（queue_.stop 清空） |
| 阻塞等待线程退出 | 否（立即返回） | 是（join） |
| 用途 | 给任务体面的收尾时间 | 宽限期到后的强制收摊 |

典型用法：先 `request_stop()` → 等宽限期（任务自查退出）→ 仍未结束
再 `stop()`。环节 12 的 `StopAllowsRunningTaskToFinish` 仍然成立：
**没自查令牌的任务依旧只能等它跑完**（测试未改动，继续通过）。

## 408 考点映射

| 本实现 | 408 概念 |
| --- | --- |
| request_stop 广播标志 | 软件中断/信号（异步通知，不强制） |
| 任务在安全点自查 | 进程撤销需运行到安全点；轮询（polling）机制 |
| 不可从外部杀线程 | 进程/线程撤销必须先回收资源，CPU 上的指令流只能等它结束 |
| release/acquire | 内存屏障（标志位可见性） |
| 两层（执行前跳过 + 执行中自查） | 就绪态（直接撤销）vs 运行态（等安全点） |

## 验证

- test_executor 22 用例（+5，原 17 个全部通过）：
  - TokenNotRequestedInitially：令牌初始未置位
  - RequestStopSkipsPendingTasks：先广播再提交 20 个任务 →
    submit 全部成功、执行 0、skipped 20（确定性：令牌必然已置位）
  - RunningTaskObservesStopAndExitsEarly：满载 2000ms 的任务，
    中途 request_stop 后 < 500ms 收尾（实测 ~60ms）
  - RequestStopIsIdempotentAndNonBlocking：重复调用无副作用、立即返回
  - StopSignalsRunningTaskToExitEarly：`stop()` 也广播令牌，自查任务
    提前退出（满载 1000ms，实测 < 300ms）
- 全量 71 用例全过（queue 15 + task 4 + executor 22 + telemetry 12
  + priority 10 + work_stealing 8），CTest 6/6，零警告
- demo 实测：

```
=== 协作式中断演示 ===
8 个长任务（满载各 1000ms），250ms 后广播停止：
  广播后耗时 61ms，完整跑完 0，自查提前退出 4，排队被跳过 4
```

对比同场 demo 的急停（未自查任务）：排队 20 个、耗时 311ms（等慢任务跑完）。

## 踩坑 / 设计取舍清单

1. **执行前跳过 vs 执行中自查是两件事**：只做前者，长任务仍要跑完；
   只做后者，排队任务会白占 CPU 时间逐个体面退出。两者都要。
2. **跳过不能计入执行数**：否则吞吐/平均耗时指标失真。Telemetry 新增
   `skipped_` 独立计数（`record_skipped()`）。
3. **shutdown 不广播令牌**：优雅停机语义是"干完活再走"，66 个存量
   用例依赖这一点；只有 `stop()` 才广播。
4. **令牌自动补挂**：submit/push_wait 统一补挂，避免调用方漏传导致
   "取消对这任务不生效"的隐性 bug。

## 下一步候选

- 双端无锁本地队列（owner LIFO + 偷取 FIFO，ForkJoinPool 双索引模式）
- 统一线程池 API（Executor 与 WorkStealingPool 合并，策略可配）
- 任务依赖图 / Future 语义（submit 返回句柄，支持 wait/get）