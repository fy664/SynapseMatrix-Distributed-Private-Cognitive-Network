# C++ Runtime：Work-Stealing（Month 3 环节 11）

> 规格书 Month 3 · WorkStealingPool · 2026-10-30
> 对应 408：多处理机负载均衡；工程 = Go runtime / Java ForkJoinPool 核心策略

## 目标

环节 9/10 做了单队列线程池的弹性伸缩；但单队列在多核下是**热点**：
所有 Worker 抢同一把锁，吞吐上不去。环节 11 引入多队列 Work-Stealing：
每个 Worker 吃自己的队列，空了偷别人的——锁分片 + 负载均衡。

## 设计

```
WorkStealingPool(num_workers)
  ├─ 每个 Worker 一个本地队列（mutex + deque）
  ├─ submit(task)     : 轮询分发（负载均匀）
  ├─ submit_to(w, t)  : 亲和性提交（模拟"提交方偏爱某队列"）
  ├─ worker 循环      : 取自己的队头 → 空则按偏移偷别人队尾 → 全空 sleep
  └─ executed/stolen  : per-worker 执行/偷取计数（观察 Work-Stealing 是否发生）
```

**三个关键工程点**：

1. **偷队尾不偷队头**：本地取队头（最近提交、cache 友好）；
   偷取取队尾（最旧的）——被偷方刚准备好的新任务不受影响，
   整体延迟更小（ForkJoinPool 经典做法）。
2. **无死锁论证**：Worker 每次只持一把锁（取自己的 → 解锁 →
   偷别人的 → 解锁），绝不嵌套持锁 → 无锁等待环。
3. **先建全部队列再启动线程（踩坑记录）**：第一版"建一个队列
   启一个线程"，线程 i 启动后立即去偷别的队列，撞上还没赋值的
   空 unique_ptr → 解引用空指针崩溃（0xC0000005）。修复：两个
   循环——先建完所有本地队列，再启动全部线程。

## 408 考点映射

| 本实现 | 408 概念 |
| --- | --- |
| 每 Worker 本地队列 | 每 CPU 的就绪队列 |
| 空则偷别人队尾 | 多处理机负载均衡（任务迁移） |
| 锁分片（各自队列各自锁） | 减少临界区竞争（性能优化） |
| 亲和性提交 | 数据亲和性（NUMA 友好调度） |

## 验证

- test_work_stealing +7 用例：
  - 100 任务全部执行（无丢失）
  - 亲和性提交 → 偷取必然发生（stolen 总和 > 0，确定性断言）
  - 耗时任务轮询分发 → 各 Worker 都有活（并行窗口）
  - shutdown 后提交拒绝（普通 + 亲和性）
  - shutdown 幂等 / 空转退出 / 并发提交无丢失
- 全量回归：61 用例全过（queue 15 + task 4 + executor 13 +
  telemetry 12 + priority 10 + work_stealing 7），CTest 6/6，
  编译零警告
- demo 实测（30 个任务全塞 worker0）：

```
worker0: 执行 7，偷取 0
worker1: 执行 7，偷取 7
worker2: 执行 8，偷取 8
worker3: 执行 8，偷取 8
总执行 30，总偷取 23（负载均衡生效）
```

## 踩坑清单（本环节新增）

1. 队列数组未填完就启动线程 → 空 unique_ptr 解引用崩溃
   （先建队列、后启线程）
2. 无耗时任务时"分布"断言不可靠：一个 worker 可能一轮循环
   偷光所有队列，其他 worker 醒来时全空——那是正常行为
   （测试用 sleep 任务制造并行窗口）
3. Task 无默认构造函数 → 取任务用 `std::optional<Task>` 表达"没取到"

## 下一步候选

- 无锁本地队列（deque + atomic head/tail，去 mutex）
- Work-Stealing + 优先级：本地队列也用优先结构
- 把 WorkStealingPool 并入 Executor 接口（统一线程池 API）