# C++ Runtime：动态扩容（Month 3 环节 9）

> 规格书 Month 3 · Executor · 2026-10-28
> 对应 408：多处理机调度、负载均衡；工程 = 弹性线程池

## 目标

环节 4 的 peak_pending（观察）、环节 7/8 的背压（控制）之后，
最后一块闭环：**积压持续超阈值 → 自动增加 worker**——把
"监控数据"变成"自动治理动作"，无需人工介入。

## 设计

```
Executor(num_workers, ..., max_capacity, max_workers, auto_scale_threshold)
  max_workers:           扩容上限（0 = 等于 num_workers，不扩容）
  auto_scale_threshold:  自动扩容阈值（0 = 关闭）
  maybe_grow(): 每次成功入队后调用——
    积压 >= 阈值 且 worker 数 < 上限 → 新增一个 Worker
```

**三个关键工程点**：

1. **Telemetry 槽位预分配**：`vector<atomic>` 不可扩容（atomic
   不可移动），所以构造时定大小为 `max_workers`——多出来的
   槽位留给未来扩容的 worker，Worker 计数天然越界安全。
2. **双重检查（double-checked）**：maybe_grow 先无锁看阈值
   （queue_.size() 近似即可），达标再加 `worker_mutex_` 复查——
   避免每次 submit 都抢锁，也避免并发重复扩容。
3. **worker_mutex_ 统一保护 workers_ 数组**：扩容 push_back、
   shutdown 遍历 join 都在同一把锁下，杜绝"遍历时数组被改"
   的迭代器失效问题。

## 为什么默认不扩容

线程不是免费的：创建线程有开销（内核对象 + 栈空间 1MB），
且多于硬件线程数时切换成本上升。所以扩容是**显式开启**的
能力（auto_scale_threshold > 0 才生效），默认保持固定池。
这正是"可观测 → 治理"的克制：先量化，再自动。

## 408 考点映射

| 本实现 | 408 概念 |
| --- | --- |
| 积压阈值触发扩 worker | 就绪队列变长 → 调度器增加处理机 |
| max_workers 上限 | 处理器数上限（硬件约束） |
| 双重检查锁 | 同步开销优化（先读后锁） |
| 默认不扩容 | 线程/进程创建成本权衡 |

## 验证

- test_executor +4 用例（累计 9）：
  - 积压超阈值自动扩容（1 → >1，任务全部执行）
  - 积压未超阈值不扩容
  - 默认关闭（回归：老行为不变）
  - 扩容到上限后停止（≤ max_workers）
- 全量回归：50 用例全过（queue 15 + task 4 + executor 9 +
  telemetry 12 + priority 10），CTest 5/5，编译零警告
- demo 实测：

```
初始 1 worker，高峰自动扩容到 4 个，24+1 任务全部执行
```

## 下一步候选

- 动态缩容：worker 空闲超时自动退出（只增不减是 V1 简化）
- 队列大小动态调整：capacity 随负载变化
- Work-Stealing：worker 空闲时从别家队列偷任务
  （408 负载均衡的高级形式，也是 Go/Java 线程池常见策略）