# Research Question：运行时怎么被"看见"？Telemetry 深化（Month 3 环节 3）

## Problem

环节 2 的多线程 Task Runtime 只有"总执行数"一个计数，
看不到谁在干活、干得多快、有没有任务被拒。规格书核心原则
"先可观测，后优化"——没有 per-worker 视角，负载不均衡、
任务被拒这类问题完全不可见。本环节把 Telemetry 深化为
三维度可观测：per-worker 分布 + 拒绝计数 + 执行耗时。

## Background

**可观测性三件套**（工程通用，408 也考"进程状态观测"）：
1. 计数（Counter）：谁干了多少——per-worker 计数可发现热点/闲置
2. 耗时（Timer）：干得多快——平均耗时是吞吐的倒数，优化标尺
3. 事件（Event）：出什么事了——拒绝计数暴露"提交到已关闭池"

**为什么用 atomic 不用锁**：每个 worker 只写自己那一格
（per-worker 计数天然无竞争），fetch_add 是硬件级的
read-modify-write，无锁无阻塞；总耗时是"多个 worker 同时写
同一个原子量"——fetch_add 保证原子累加。这对应 408 里
"无锁同步"（Lock-free）的思想：能无锁就无锁。

## Experiment

1. `telemetry.hpp` 重写：
   - `vector<atomic<size_t>> done_per_worker_`：构造定大小，
     每 worker 一格，独立原子自增
   - `rejected_` / `total_elapsed_us_`：拒绝数、总耗时（微秒）
   - 查询：`tasks_done()`（各 worker 求和）、`task_count(w)`、
     `rejected()`、`avg_task_ms()`（耗时/任务数）
2. `worker.cpp`：执行前后用 steady_clock 计时，上报耗时
3. `executor.cpp`：submit 被拒时 record_rejected()
4. `test_telemetry.cpp`：7 个单测
5. demo：打印 per-worker 分布 + 平均耗时 + 拒绝数

## Result

- 编译零警告；test_telemetry 7 用例全过；CTest 4/4（21 个 C++ 单测累计）
- demo 输出（10 任务 × 4 worker，每任务 sleep 5ms）：

```
总执行数        : 10
平均耗时/任务   : 13.54 ms
拒绝数          : 0
per-worker 分布 :  w0=2  w1=2  w2=3  w3=3
```

- 分布 w0=2 w1=2 w2=3 w3=3：10 个任务大致均分到 4 个 worker，
  说明 FIFO 调度下负载均衡正常（任务短、无优先级时天然均衡）

## 关键设计决策

1. **vector<atomic> 必须构造定大小**：atomic 不可拷贝/移动，
   vector 的 resize/扩容需要移动构造 → 编译失败。
   Executor 构造时已知 worker 数，一次定死。
2. **per-worker 计数 vs 全局锁**：每 worker 一格 = 无竞争写；
   全局锁（一个 mutex 包所有计数）在高并发下是争用热点。
   数据量小时两者差不多，per-worker 是"可扩展计数"的标准形态
   （408 里就是"分片/局部计数再归并"）。
3. **steady_clock 测耗时**：单调时钟，不受系统改时间/校时影响；
   system_clock 会跳变，测耗时禁用。
4. **avg_task_ms 除零保护**：无任务时返回 0，不 NaN。

## 408 映射

- **锁 vs 原子**：临界区多条语句用 mutex；单条读改写用 atomic
  （fetch_add）。对应互斥与硬件同步指令（x86 lock 前缀）。
- **分片计数**：per-worker 计数 = 数据库分片/归并的并发版——
  各分片独立写，读时归并。
- **时钟**：steady_clock 对应"相对时间"（单调），
  system_clock 对应"墙上时间"（可调）——操作系统里
  CLOCK_MONOTONIC vs CLOCK_REALTIME。

## Conclusion

Telemetry 从"一个总数"升级为"分布 + 耗时 + 事件"三维度，
且全部无锁。per-worker 分布可用于负载均衡观察（哪个 worker
空闲、哪个热点），平均耗时是后续 Benchmark（吞吐/P95）的基础
数据源。Month 3 核心对象全部深化完毕。

## Next Question

- 优先级调度：Task 已带 Priority 字段但队列是 FIFO。下一步
  把 TaskQueue 换成优先队列（std::priority_queue + mutex/CV），
  高优先级先出队——408 优先级调度（PS，可抢占/非抢占）。
- ~~队列水位峰值~~：已实现（2026-10-22，见下方补记）。

## 补记（2026-10-22 环节 4）：队列水位峰值采样

**需求**：可观测"高峰期队列有多深"——背压设计（队列满时
拒绝/降速）的依据，也是容量规划的输入。

**实现**：
- `Telemetry::record_watermark(current)`：CAS 循环更新峰值
  （compare_exchange_weak，无锁 max）
- 调用点：`Executor::submit` 成功后采样 `queue_.size()`
  （入队使队列变长，只有此时可能创新高；pop 只会变短）
- `peak_pending()` 查询（Telemetry + Executor 两层暴露）

**CAS 为什么是"无锁 max"**：
```
load 峰值 → 若 current > 峰值，CAS(旧峰值→current)
CAS 失败 → 别的线程已把峰值提得更高，用新峰值重试
```
对应 408 硬件同步指令（x86 lock cmpxchg）——原子比较交换，
失败自旋重试，无锁但无忙等浪费。

**验证**（+5 单测，Telemetry 累计 12 用例）：
- 单调不减（升序记录取最大）
- 降序记录不回退
- 初始 0
- 并发 CAS 无丢失：4 线程 × 1000 次各自报水位，峰值 = 最大线程号
- Executor 集成：单 worker + 50 个慢任务 → 峰值 > 1 且全部执行完

**demo 实测**：10 任务 × 4 worker → 队列积压峰值 10
（提交快于消费时队列短暂堆满，之后 worker 清空）。
