# C++ Runtime：动态缩容（Month 3 环节 10）

> 规格书 Month 3 · Executor · 2026-10-29
> 对应 408：资源回收（PCB/栈）、负载均衡对偶；工程 = Java 线程池 keepAliveTime

## 目标

环节 9 做了"忙时扩容"，但线程只增不减会浪费资源：
空闲线程占着栈（默认 1MB）和内核对象。环节 10 = **对称治理**：
worker 空闲超过 idle_timeout 且线程数 > 核心数 → 自我退役。

```
Executor(num_workers, ..., max_workers, auto_scale_threshold, idle_timeout)
  idle_timeout > 0 : worker 空闲超时后自动退役（0 = 关闭，默认）
  核心数 = num_workers（初始线程常驻，永不退役）
```

## 设计

```
队列：TaskQueue::pop_wait(timeout)     —— 带超时取任务
      TaskQueue::is_closed()           —— 区分"空闲"和"关闭"
Worker：run() 超时空闲时 try_retire()   —— CAS 抢退役名额
Executor：reap_retired()                —— join + 移除退役 worker
          free_ids_                     —— 退役 id 回收复用
```

**三个关键工程点**：

1. **CAS 抢名额，不缩到核心以下**：多个 worker 同时空闲时，
   若都"先读后减"会一起退到核心数以下。try_retire 用
   `compare_exchange_weak` 循环：原子地"读存活数 → 校验 > 核心 →
   减 1"，只有抢到名额的线程才退役。这是无锁算法"读取-校验-更新"
   的标准套路（408 并发章节 + 环节 4 的 watermark CAS 复习）。
2. **退役 id 回收复用（踩坑记录）**：第一版 next_worker_id_ 只增，
   缩容-扩容循环后新 worker id 越出 telemetry 槽位（=max_workers），
   越界写内存直接崩溃（0xC0000409 栈损坏）。修复：reap_retired 把
   退役 worker 的 id 归还 free_ids_，扩容时优先复用——id 永远落在
   [0, max_workers) 内。**顺序也关键：先 reap（收集 id）再 grow（复用）**。
3. **存活计数 live_workers_ = 真实服役线程数**：构造 +1、退出 -1
   （原子），active_workers() 直接读它，无需锁。shutdown 后归 0
   （语义变化：之前是数组大小，现在语义更真实）。

## 408 考点映射

| 本实现 | 408 概念 |
| --- | --- |
| 空闲超时退役 | 进程结束 → 回收 PCB/栈（资源回收） |
| 核心线程常驻 | 常驻资源池（Java 核心线程） |
| CAS 抢退役名额 | 并发资源分配的无锁同步 |
| 忙扩闲缩 | 负载均衡/弹性资源管理 |

## 验证

- test_executor +4 用例（累计 13）：
  - 空闲超时自动退役（扩到 3 → 缩回核心 1）
  - 不缩到核心数以下（CAS 保证）
  - 默认关闭（idle_timeout=0 回归：老行为不变）
  - 缩容后再扩容（弹性闭环）
- 全量回归：54 用例全过（queue 15 + task 4 + executor 13 +
  telemetry 12 + priority 10），CTest 5/5，编译零警告
- demo 实测：

```
=== 动态缩容演示 ===
高峰 worker=4
空闲后 worker=1（缩回核心）
再入高峰 worker=4（又扩回）
```

## 踩坑清单（本环节新增）

1. `vector<atomic>` 的槽位必须定大小 → telemetry 槽位 = max_workers
2. worker id 不能只增 → free_ids_ 复用（越界写 = 栈损坏崩溃）
3. 先 reap 再 grow（顺序错了 id 池是空的）
4. active_workers() 语义变化：shutdown 后归 0（旧测试同步更新）
5. 测试里不小心制造"双份 shutdown"→ 断言夹在中间失败（编辑要小心）

## 下一步候选

- Work-Stealing：worker 空闲时从别家队列偷任务
  （408 负载均衡高级形式，Go/Java 线程池常见策略）
- 队列容量动态调整：capacity 随负载变化
- 急停（stop_ 字段启用）：不等队列清空，立即中断