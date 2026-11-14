# C++ Runtime：关键路径与耗时分析（Month 3 环节 17）

> 规格书 Month 3 · Task 生命周期治理（图级观测）· 2026-11-14
> 对应 408：AOE 网 / 关键路径 / ve-vl 求解 / 松弛时间

## 目标

环节 16 的图能跑起来，但看不出"慢在哪"：改哪个任务的耗时能真正缩短总工期？
把每个节点的实际耗时记下来，按 AOE 网的口径算出：

- 每个任务的**最早开始时间 ve**（前驱全完成后才能开始）
- 每个任务的**最晚开始时间 vl**（再晚就拖累总工期）
- **松弛时间** = vl − ve，松弛为 0 的就是**关键活动**
- 首尾相连的那串关键活动 = **关键路径**，其总耗时 = **项目最短完成时间**

## 算法（408 AOE 标准求解，两遍正反扫描）

顶点为任务、耗时作权（节点加权 DAG，等价于 AOE 的活动时间 a）。

**① 正向扫描求 ve（最早）**

```
ve[入口] = 0
ve[j] = max( ve[i] + duration[i] )   // i 是 j 的所有前驱
```

因为任务本身要占时间，最早开始 = 所有前驱**结束时刻**的最大值。

**② 反向扫描求 vl（最晚）**

```
vl[出口] = 总工期(makespan)
vl[i] = min( vl[j] - duration[i] )   // j 是 i 的所有后继
```

逆拓扑序推进：一个任务的最晚结束不能晚于它任一后继的最晚开始。

**③ 松弛与关键路径**

```
slack[i] = vl[i] - ve[i]
slack == 0  →  关键活动（复制到路径上）
```

回溯：从"汇点里 EF 最大的节点"开始，每次找「关键 + 直接前驱 +
EF == 我的 ES」的那个，倒推到入口。

## 实现要点

- 起止时刻用 `steady_clock`（单调时钟，不受系统校时影响），存微秒整数：
  `atomic<long long> begin_us / end_us`，在包装任务体执行前后各打一次点。
- 只统计 `Done` 节点：跳过/失败的任务没有可比语义，不进表、不参与路径。
  因此在失败图上 `timings()` 为空、`critical_path()` 返回 `std::nullopt`。
- 拓扑序用 Kahn 重新推（Done 子图），不复用运行时入度计数器——
  运行时计数器在跑完后已经归零，不等于原始拓扑关系。
- 终点必须选**汇点**（没有 Done 后继的节点）。这点踩了坑，见下。

## 408 映射

| 本实现 | 408 概念 |
| --- | --- |
| `earliest_start / earliest_finish` | ve / e（活动最早开始） |
| `latest_start / latest_finish` | vl / l（活动最晚开始） |
| `slack = latest − earliest` | 时间余量 d = l − e |
| `slack == 0` | 关键活动 |
| `critical_path().total` | 项目最短工期（关键路径长度） |
| 正向/反向两遍扫描 | AOE 网 ve/vl 求解 |
| 微秒整数运算 | 避免浮点误差导致 slack 出现 ±1e-9 误判 |

## 验证

- `test_task_graph` 新增 4 用例（原 6 个全部保留，共 10 个）：
  - `ChainTimingsHaveZeroSlack`：A(10ms)→B(20ms)→C(5ms) 链上
    三节点 slack 全为 0、全部 critical；`ES(B) ≥ 10000us`、
    `ES(C) ≥ 30000us`；关键路径恰好 3 个节点且总耗时 ≥ 33ms
  - `DiamondSlackOnShortBranch`：A(10)→B(40)、C(10)→D(5)，
    短分支 C 的 slack > 20ms、`critical == false`；
    关键路径为 3 节点且**不含 C**、终点是 D
  - `NoCriticalPathWithoutDoneNodes`：A 抛异常 → B Skipped，
    `timings()` 为空、`critical_path()` 为 `nullopt`（没有编造一条假路径）
  - `TimingArithmeticConsistent`：任何图都满足 `EF = ES + duration`、
    `LF = LS + duration`、`EF/LF ≤ 总工期`（会计恒等式，防算错）
- 全量 **86 用例全过**（queue 15 + task 4 + executor 27 + telemetry 12
  + priority 10 + work_stealing 8 + task_graph 10），**CTest 7/7**，零警告
- demo 在依赖图段末尾追加关键路径与时序表，实测输出：

```
  管线总耗时 110ms（串行需 120ms+，并行省掉 checksum 与 parse 的差）
  关键路径：fetch → checksum → merge → report（总工期 110ms）
  时序表（ES 最早开始 / EF 最早结束 / 松弛 = 最晚-最早）：
    fetch     耗时    46ms  ES     0ms  EF    46ms  松弛     0ms  [关键]
    parse     耗时    31ms  ES    46ms  EF    77ms  松弛    32ms
    checksum  耗时    63ms  ES    46ms  EF   110ms  松弛     0ms  [关键]
    merge     耗时     0ms  ES   110ms  EF   110ms  松弛     0ms  [关键]
    report    耗时     0ms  ES   110ms  EF   110ms  松弛     0ms  [关键]
```

`parse` 松弛 32ms ≈ `checksum` 比它多花的 30ms —— 这就是"改 parse 的
耗时对总工期毫无影响、改 checksum 才有效"的定量依据。

（实测耗时比设定的 sleep 略大 5~6ms，这是"入队 → 线程被调度 → 开始打点"
的等待时间；AOE 分析用的是任务实际占用时间，口径正确。）

## 踩坑清单

1. **终点不能取"全表 EF 最大"**：菱形/汇合结构下中间节点与末端节点
   EF 相同（都是总工期），取到中间节点会把路径截成半截。
   第一版 demo 就打印出 `fetch → checksum` 而不是完整四节点路径。
   修复：只在**没有 Done 后继的汇点**里选 EF 最大者。
2. **不能复用运行时入度计数器**：它跑完已归零，拓扑关系丢失，
   必须按 succ 邻接表重新做 Kahn。
3. **零耗时的末段节点**（merge/report 各 0ms）slack 也是 0，会一并进
   关键路径——这符合 AOE 定义（耗时 0 的活动不占用时间，但仍是必经的
   结构节点），demo 输出因此是 4 个节点而非 2 个。
4. **失败/跳过节点必须排除**：把 Skipped 节点的 0 耗时算进去会凭空
   造出一条"最短路径"，结论完全失真。语义统一为"只分析成功节点"。

## 下一步候选

- 双端无锁本地队列（owner LIFO + 偷取 FIFO，ForkJoinPool 双索引模式）
- 统一线程池 API（Executor 与 WorkStealingPool 合并，策略可配）
- 关键路径的"优化倒推"：给定目标工期，自动找出需要提速的任务组合
