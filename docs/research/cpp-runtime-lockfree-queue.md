# C++ Runtime：无锁本地队列（Month 3 环节 13）

> 规格书 Month 3 · WorkStealingPool · 2026-11-08
> 对应 408：硬件原子指令 / 自旋重试；工程 = Vyukov MPMC bounded queue

## 目标

环节 11 的 Work-Stealing 用 mutex + std::deque 做本地队列。mutex 是
**阻塞式互斥**：抢不到锁的线程要被挂起/唤醒，涉及内核态切换（微秒级），
而任务本身可能只跑几百纳秒——**锁的开销比任务还大**，池子越忙越慢。

环节 13 = 本地队列**无锁化**：Vyukov MPMC 有界环形队列，push/pop 全靠
CAS 完成，抢不到就自旋重试，**不发生上下文切换**。

## 设计

### 无锁环形队列结构

```
cells_[1024]   每槽 = { atomic<size_t> seq, optional<Task> task }
enqueue_pos    生产者位置（原子）
dequeue_pos    消费者位置（原子）

seq 的三种状态（无锁算法的核心）：
  seq == pos            → 槽位空闲，可写
  seq == pos + 1        → 数据已写好，可读
  seq == pos + capacity → 已被消费，等待下一轮（可写）
```

**为什么必须用 seq 版本号**：环形数组会复用槽位。若只靠 head/tail 位置
判断，消费者可能读到一个"生产者刚抢到位置、还没写完数据"的槽——读到
半个 Task 就是未定义行为。版本号 seq 把"位置"和"数据就绪"绑在一起：
只有 seq 推进到 pos+1 才算数据可用。

### 生产者 / 消费者算法

```
push(task):
  pos = enqueue_pos
  loop:
    读 cell[pos & mask].seq
    dif = seq - pos
    dif == 0  → CAS(enqueue_pos: pos → pos+1)
                成功：写槽位数据 → seq.store(pos+1, release) → 返回 true
                失败：重试
    dif <  0  → 队列满 → 返回 false
    dif >  0  → 别人已推进 → 重读 pos
```

```
pop():
  pos = dequeue_pos
  loop:
    读 cell[pos & mask].seq
    dif = seq - (pos + 1)
    dif == 0  → CAS(dequeue_pos: pos → pos+1)
                成功：取走数据 → seq.store(pos + cap, release) → 返回任务
    dif <  0  → 队列空 → 返回 nullopt
    dif >  0  → 重读 pos
```

### 内存序（无锁正确性的关键）

- 生产者写槽位数据后，**release** 发布 seq：保证"数据写入"对消费者可见
- 消费者 **acquire** 读 seq：保证看到 seq 后才能看到数据
- 位置推进（enqueue_pos/dequeue_pos 的 CAS）用 relaxed：位置本身只是
  号码牌，不承载数据同步职责

用错内存序（全部 relaxed）= 诡异崩溃/读到半截数据，这是无锁编程
最常见的坑。

## 顺带去掉提交路径的锁

原来的 `submit` 用 `submit_mutex_` 保护轮询指针 `next_queue_`。
环节 13 把它改成 `std::atomic<size_t>` + `fetch_add`——提交路径
**完全无锁**（只剩 closed_ 原子检查）。整个池子现在只有 shutdown
的 join 是阻塞操作。

## 408 考点映射

| 本实现 | 408 概念 |
| --- | --- |
| CAS 抢位置 | 硬件原子指令（test-and-set / compare-and-swap） |
| 重试自旋 | 自旋锁（忙等）vs 阻塞锁（挂起/唤醒） |
| release/acquire 内存序 | 内存屏障（控制读写重排，保证可见性） |
| 每槽版本号 | 无锁算法避免 ABA 的通用手段 |

## 验证

- test_work_stealing 8 用例（+1）：
  - 原有 7 个全部通过（API 不变、语义不变）
  - 新增 NoLossUnderHeavyContention：8 worker × 8 生产者 × 100 任务
    = 800 个极短任务（几百纳秒）——制造"消费追不上生产"的高竞争
    窗口，压力集中在 CAS 上；断言"恰好执行 800 次"（丢失或重复
    都会暴露）
- 全量回归：66 用例全过（queue 15 + task 4 + executor 17 +
  telemetry 12 + priority 10 + work_stealing 8），CTest 6/6，
  编译零警告
- demo 实测（无锁版本）：

```
30 个任务全塞 worker0，各 worker 执行分布：
  worker0: 执行 7，偷取 0
  worker1: 执行 7，偷取 7
  worker2: 执行 7，偷取 7
  worker3: 执行 9，偷取 9
总执行 30，总偷取 23（负载均衡生效）
```

## 语义变化说明（必须记录）

1. **偷取方向**：环节 11 从 deque 的 back 取（push_back 进队 → back 是
   最新任务）；环节 13 统一从队头（FIFO，最旧任务）取。环形队列单端
   设计天然 FIFO；偷最旧任务与 408"最久等待者优先"更一致。
2. **队列有界**：容量固定 1024（mutex 版 deque 无界）。满则 push 返回
   false —— 与 Executor 有界队列的背压语义一致；提交方可判断 false
   做重试/降级。
3. **容量必须 2 的幂**：索引用 `& (cap-1)` 代替 `%`（更快，且无符号问题）。

## 踩坑清单（本环节新增）

1. `seq - pos` 必须转 `std::ptrdiff_t` 做**有符号**比较：无符号相减在
   "seq 落后一整轮"时会回绕成巨大正数，判断全错。
2. `Cell` 含 `atomic` → 不可拷贝/移动 → `std::vector<Cell>` 必须
   **构造时一次定大小**（`cells_(kCapacity)`），不能 push_back。
3. 消费者取走数据后要 `cell.task.reset()`：释放 moved-from 的
   optional，否则槽位复用时的赋值路径会持有已移动对象的旧状态。

## 下一步候选

- 本地队列升级为双端无锁（owner 端 LIFO + 偷取端 FIFO，ForkJoinPool
  WorkQueue 双索引模式）——需要更复杂的内存序论证
- 协作式中断：Task 里嵌入"检查停止标志"钩子
- 统一线程池 API（Executor 与 WorkStealingPool 合并，配置项选择策略）