# C++ Runtime：优先级调度（Month 3 环节 5）

> 规格书 Month 3 · 项目对象 Queue 升级 · 2026-10-23
> 对应 408：操作系统 2.2 处理机调度——优先级调度算法（PS）

## 目标

TaskQueue 从 FIFO 升级为优先级队列：高优先级任务先出队，
同优先级保持 FIFO。这是规格书 Task 的 `Priority` 字段（Low/
Normal/High）第一次真正参与调度。

## 设计决策

### 1. 为什么用 std::priority_queue 而不是排序的 deque

| 方案 | 插入 | 出队 | 结论 |
| --- | --- | --- | --- |
| deque + 插入时线性扫描 | O(n) | O(1) | 队列小时可用，规模上去就退化 |
| priority_queue（二叉堆） | O(log n) | O(log n) | 标准答案：堆是"取最大"的天然结构 |

408 里"就绪队列"在教材上常画成链表，但工程实现（如 Linux
runqueue 的红黑树）本质是堆/树——需要"每次取最紧急的那个"，
二叉堆是复杂度最优解。

### 2. priority_queue 不稳定 → 必须加 seq 序号

`std::priority_queue` 只保证"堆顶是最大的"，不保证相等元素
的相对顺序。同优先级任务（Normal 和 Normal）谁先出？

解法：每个任务入队时在锁内分配单调递增的 `seq`，比较器
（Compare）两级排序：

```
1. priority 高者优先（High > Normal > Low）
2. 同 priority：seq 小者优先（先来先出）
```

这是"复合键"思想——排序规则不满足单一维度时，拼出稳定键。
408 里 FIFO/PS 的权衡（先来先服务 vs 优先级抢占）在这里是
一行比较器。

### 3. Compare 的写法（容易写反）

```cpp
struct Compare {
    bool operator()(const Entry& a, const Entry& b) const {
        if (a.task.priority() != b.task.priority())
            return a.task.priority() < b.task.priority(); // 高优先"更大"
        return a.seq > b.seq;   // seq 小"更大" → 先出
    }
};
```

priority_queue 用 less 语义：`Compare(a,b)==true` 表示 a 比 b
"小"、排在堆底；**堆顶永远是"最大"的**。所以高优先级要写成
"大于"，seq 小的要写成"大于"——两处都是反直觉的，注释里必须
写明。

### 4. top() 是 const 引用 → 只能拷贝

`priority_queue::top()` 返回 `const T&`，不能 move 出来。
pop 只能 `auto t = queue_.top().task; queue_.pop();`。
Task 内部是 `std::function<void()>`，小闭包拷贝开销可忽略，
V1 接受（README 记录为已知限制）。

## 与 FIFO 版的行为差异

- `push` 仍是 O(log n)（原 O(1) 尾部插入）——换取调度正确性
- `pop` 返回的不再是"最早来的"，而是"最紧急的"
- 关闭语义、条件变量唤醒、接口签名全部不变 →
  Worker/Executor/Telemetry 零改动，只有 Compare 换心

## 验证

- test_priority 5 用例：
  - 高优先先出（Low/Normal/High 混合，pop 顺序=优先级降序）
  - 同优先级 FIFO（20 个 Normal 严格按提交顺序）
  - 插队（先 5 个 Low 后 1 个 High，High 越过整队）
  - close 语义不回归（先取完存量再 nullopt）
  - Executor 集成（单 worker：先 6 Normal 后 2 High →
    执行顺序 h0 h1 n0 n1 ... n5）
- 全量回归：31 用例全过（queue 5 + task 4 + executor 5 +
  telemetry 12 + priority 5），CTest 5/5，编译零警告
- demo 实测（2 worker，先 6 Normal 后 2 High）：

```
[HIGH 1] 插队执行！
[HIGH 0] 插队执行！
[normal 1] 执行中
[normal 0] 执行中
...
```

High 全部先于 Normal 执行，Normal 之间 FIFO。

## 408 考点映射

| 本实现 | 408 概念 |
| --- | --- |
| priority_queue + Compare | 优先级调度算法 PS（就绪队列按优先级排队） |
| seq 稳定排序 | 同优先级先来先服务（FCFS 退化） |
| O(log n) 入队/出队 | 堆的复杂度（vs 顺序表 O(n) 插入） |
| 非抢占式出队（pop 拿最高） | 非抢占式调度；抢占需时间片/中断，V2 做 |
| 关闭后仍排空存量 | 优雅停机：先处理完高优先任务 |

## 下一步候选

- 抢占式调度：High 任务到达时打断正在执行的 Low（需要
  worker 侧检查 + 任务取消机制）——408 "可抢占 PS"
- 背压：积压峰值超阈值时 submit 拒绝/限速
- Linux 真实对照：CFS 红黑树 vs 本实现红黑树

## 补记（2026-10-25 环节 6）：老化防饥饿（Aging）

**需求**：纯优先级调度有致命缺陷——如果 High 持续到达，Low
永远排不上 = **饥饿（starvation）**。408 里 MLFQ 用"老化"
解决：等待越久的进程优先级越高，最终必然被调度。

**第一版踩坑（priority_queue 惰性重插）**：
pop 时只检查堆顶是否超龄，超龄则提升重插。但被压制的任务
永远到不了堆顶（High 每次都是 top）→ 只救堆顶，救不了深层
饥饿任务。**测试当场抓出来**：AgingBoostsWaitingLow 失败。
这正是"测试驱动设计"的价值——设计缺陷被单测暴露而非上线。

**正确设计（multiset 全量扫描）**：
- 容器从 `std::priority_queue` 换成 `std::multiset`（红黑树）
- 比较器语义反转：multiset 的 begin() 是"最小"，把高有效
  优先级定义为"更小" → begin() 恒为最优先任务
- pop 时**全量扫描**一次：等待 ≥ 老化间隔的任务，
  boost += 等待间隔数（封顶 aging_max_boost_），重置计时，重插
- 提升后重置计时：防止同一任务被连续提升霸占队首

**为什么非换容器不可**：
priority_queue 的比较器必须是固定键（堆序一旦建立，比较结果
随时间变化会破坏堆顶正确性）；且只能动堆顶。multiset 支持
遍历 + 任意删插——红黑树 O(log n) 插入删除，扫描本身 O(n)
（任务量小，换来了正确性）。

**三个参数化行为**：
- `aging_interval = 0`：禁用老化，纯优先级（环节 5 行为，回归安全）
- `aging_interval > 0`：老化开启
- `aging_max_boost`：提升上限（默认 2=最高与最低之差），
  防止无限抬升反向饿死 High

**验证**（+5 单测，test_priority 累计 10 用例）：
- 老化默认禁用：行为与环节 5 完全一致（回归）
- 老 Low 越过新 High（等 4 间隔 → boost 2 → 与 High 并列，
  seq 更小 → 先出）
- 上限封顶：a/b 等 50 间隔但 boost ≤1 → 新 High(2) 先出
  （若无上限，a/b 有效 50 会饿死 High——反向保护）
- 同优先级 FIFO 不破坏（老化后仍按 seq）
- Executor 集成：开启老化的池子不会饿死/卡死

**408 考点映射（补充）**：

| 本实现 | 408 概念 |
| --- | --- |
| 老化 boost（等待越久优先级越高） | MLFQ 防止低优先饿死 |
| boost 上限封顶 | 防"无限抬升反向饿死"，工程化约束 |
| multiset 红黑树 | 就绪队列的数据结构选型（Linux CFS 红黑树） |
| 全量扫描 O(n) | 权衡：正确性 > 理论最优，n 小可接受 |
