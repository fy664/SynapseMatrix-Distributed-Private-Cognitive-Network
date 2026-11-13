# C++ Runtime：任务依赖图（DAG 调度，Month 3 环节 16）

> 规格书 Month 3 · Task 生命周期治理（依赖编排）· 2026-11-13
> 对应 408：AOV 网 / 拓扑排序 / 信号量 P-V / 死锁（循环等待）

## 目标

到环节 15 为止，提交出去的任务彼此独立——谁先谁后由调度器决定。
真实流水线有先后约束：`merge` 必须等 `parse` 和 `checksum` **都**完成。
本环节把「任务 + 依赖」编排成一张图，按拓扑序并行推进。

## 核心概念（408 对应）

- **AOV 网**（Activity On Vertex）：顶点 = 任务，有向边 = 先后约束
- **入度**：某任务还有几个前驱没完成。入度 0 = 可以立即执行
- **拓扑排序**（Kahn 算法）：每次取入度 0 的顶点，删掉它的出边并让后继入度 -1
- 本实现 = **并行版拓扑排序**：不排队依次跑，而是"谁就绪谁立刻上处理机"
- **有环 = 死锁**：环上的节点入度永远不可能归零，互相等（循环等待），
  所以必须在启动前用拓扑检查识别并拒绝

## 设计

### 数据结构

```
Node { Task task; atomic<int> deps; vector<NodeId> succ;
       TaskCompletionPtr completion; atomic<bool> settled; }
State { vector<unique_ptr<Node>> nodes; atomic<size_t> settled_count;
        StopTokenPtr token; }        // 被包装任务 shared_ptr 共享
```

- `deps` = 剩余前驱数（入度），`succ` = 后继邻接表
- `Node` 含 atomic 且 `Task` 无默认构造函数 → 必须用 `unique_ptr` 存 + 显式构造
- `State` 继承 `enable_shared_from_this`：提交出去的包装任务能用
  `shared_from_this()` 拿到强引用，TaskGraph 对象先析构也安全

### 生命周期

| 阶段 | 动作 |
| --- | --- |
| `add(task, deps)` | 登记节点、建 succ 边；依赖必须是已登记节点（`dep < id`），重复/越界抛 `invalid_argument`；`start()` 之后添加抛 `logic_error` |
| `start()` | Kahn 拓扑检查（有环抛异常）→ 提交全部入度 0 的节点；重复调用无副作用 |
| 节点执行 | 包装任务：执行本体 → `settle` 记录结果 → 成功则 `release` 后继 |
| `release` | 后继 `deps.fetch_sub(1)`，旧值为 1（减到 0）时**立刻提交**该后继 |
| 失败/取消 | `skip`：后继递归标记 Skipped（传递性） |
| `handle(id)` | 节点句柄，`wait()` 返回 Done / Skipped / Failed |
| `request_stop()` | 图级取消：置位图令牌 + 全部未落定节点结算为 Skipped |

### settle 幂等

`settled.exchange(true)` 决定"谁先落定谁生效"：
- 节点执行完写 Done
- `request_stop()` 抢先写 Skipped
两者竞争时只有一方成功，另一方的递归传播也被截断，不会重复计数。

### 包装任务的"哑令牌"

`Executor::submit` 会给没有令牌的任务补挂执行器自己的令牌。
若底层 `request_stop()`/`stop()` 触发，包装任务会被**跳过** →
后继永远等不到入度 -1 → **整图卡死**（等待者永久阻塞）。

所以包装任务预先挂一个**谁也不置位**的独立 `StopToken`，保证包装体
一定执行；取消语义统一由 `TaskGraph::request_stop()` 控制。

同理：对底层 Executor 调 `stop()`（清空队列）会断链，
必须先 `graph.request_stop()`。

## 验证

- 新增 `test_task_graph` 6 用例：
  - `LinearChainRunsInOrder`：A→B→C，执行序号严格 0/1/2
  - `DiamondHonorsDependencies`：A→(B,C)→D，A 必第 0、D 必第 3，
    B/C 顺序自由；三节点副作用求和 = 60
  - `FailurePropagatesToDependents`：A 抛异常 → B、C 均 Skipped，
    后继一次都没执行，图仍能收尾（不卡死）
  - `CancelBeforeStartSkipsAll`：启动前 `request_stop` → 全图 Skipped，
    `start()` 后包装体看到令牌自行收尾，任务体执行 0 次
  - `FanOutAllRunAfterRoot`：一父五子，5 个子全部 Done 且都在根之后
  - `InvalidDependencyRejected`：越界依赖 / 重复依赖 / start 后添加，
    三种非法用法各自抛对应异常
- 全量 **82 用例全过**（queue 15 + task 4 + executor 27 + telemetry 12
  + priority 10 + work_stealing 8 + task_graph 6），**CTest 7/7**，零警告
- demo 新增"任务依赖图演示"段（真实流水线形态）：

```
  5 个节点：fetch → (parse ‖ checksum) → merge → report
  执行序号：fetch=0 parse=1 checksum=2 merge=3 report=4
  图表已全部落定：是；末端节点 wait() → Done（完成）
  管线总耗时 108ms（串行需 120ms+，并行省掉 checksum 与 parse 的差）
```

串行耗时 40+20+60 = 120ms，实际 108ms —— parse 与 checksum 在
fetch 之后并行，只等较慢的 checksum。

## 踩坑清单

1. **`Task` 没有默认构造函数** → `struct Node { Task task; }` 的隐式默认构造
   被删除，`make_unique<Node>()` 直接编译失败（C2280 "尝试引用已删除的函数"）。
   修复：`explicit Node(Task t) : task(std::move(t)) {}`。
2. **包装任务被底层令牌跳过 = 依赖断链**：即使任务没执行，也必须有人
   把入度减掉，否则下游永久等待。用哑令牌兜住包装体。
3. **`run()` 是 `[[nodiscard]]`**：包装体里只关心 `last_outcome()`，
   要 `static_cast<void>(...)` 显式丢弃，否则 /W4 报 C4834。
4. **`wait_for` 只接受 `chrono::milliseconds`**（非模板）：
   传字面量 `2000` 编译失败，需 `2000ms` 或 `std::chrono::milliseconds(2000)`。
5. **依赖只允许指向已登记节点**（`dep < id`）→ 图的边恒为"旧 → 新"，
   **构造上不可能成环**；`start()` 里的 Kahn 检查因此是防御性断言，
   单测无法构造出环（这一点已在文档中如实标注，不假装测过）。

## 下一步候选

- 双端无锁本地队列（owner LIFO + 偷取 FIFO，ForkJoinPool 双索引模式）
- 统一线程池 API（Executor 与 WorkStealingPool 合并，策略可配）
- 图级观测：节点耗时分布 / 关键路径（最长路径 = 项目完成时间）
