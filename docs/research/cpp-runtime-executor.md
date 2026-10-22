# Research Question：多线程 Task Runtime 怎么正确实现与验证？（Month 3 环节 2）

## Problem

规格书 Month 3 验收项：自己实现多线程 Task Runtime。
环节 1 已把 CMake / 测试框架 / Task / 线程安全 Queue 立住，
Worker 和 Executor 只留了接口。本环节把它们实现掉，
并用"提交 N 个任务全部执行"来验证并发正确性。

## 分工

- 基础设施（骨架/测试/构建）——AI 完成（环节 1）
- **核心实现（Worker 循环 + Executor 线程池）——本环节完成**
  按既定协议这部分本应用户手写；AI 给出完整实现 + 逐行语法
  讲解，用户可对照重写。任何实现都必须能通过并发单测。

## Background

**Thread Pool 的标准结构**（408 生产者-消费者 + 调度器模型）：

```
提交方 ──submit──> TaskQueue（mutex+CV 保护）<──pop── Worker×N
                        │                              │
                    关闭队列(close)                  执行 + 上报
```

- Worker：一个消费线程，`while (auto t = queue.pop()) { (*t)(); }`，
  pop 返回 nullopt（队列 close 且清空）即退出——**优雅停止不打断在跑的任务**
- Executor：N 个 Worker 共享队列；shutdown = close 队列 + 逐个 join
- 关键：**任务不丢失**（并发 push/pop 由 Queue 保证）、
  **停机不漏活**（已入队任务必须全部执行完）

## Experiment

1. 接口修正（写实现前暴露的设计问题）：
   - Worker 加 `join()`（幂等：joinable 检查）——否则 Executor::shutdown
     显式 join 后析构再 join 是 UB（std::thread 不能 join 两次）
   - Executor 加 `Telemetry` 成员（Worker 要上报统计）+
     `shutdown_done_` 幂等标志（shutdown 可多次调用）
   - **vector<Worker> 改 vector<unique_ptr<Worker>>**：Worker 含
     std::atomic/std::thread，不可拷贝也不可移动，vector 的
     reserve/扩容要求可移动构造 → 编译失败（error C2672）。
     指针容器下对象地址永不移动，线程捕获的 this 恒有效。
2. `worker.cpp`：构造启动线程、run 循环、幂等 join、RAII 析构
3. `executor.cpp`：make_unique 建 worker、submit 转发、shutdown 幂等
4. `test_executor.cpp`：5 个单测（全部执行/拒绝提交/并发无丢失/
   优雅关闭不漏活/析构 RAII）
5. demo 升级：用 Executor 提交 10 任务，Telemetry 统计验证

## Result

- 编译零警告（/W4），test_executor 5 用例 + 原有 9 用例全过
- CTest 3/3 passed
- demo：10 任务 × 4 worker，乱序执行（并发特征正确），
  Telemetry 统计执行数 = 10
- 并发无丢失验证：4 生产者 × 200 任务 + 8 worker，计数 == 800

## 踩坑记录

1. **std::atomic 不可移动 → 含它的类不可移动 → vector 编译失败**
   这是 C++ 的硬约束：atomic 的拷贝/移动构造都被删除。
   解法：unique_ptr 容器（或 deque/手写固定数组）。
2. **thread 不能 join 两次**（UB）→ 幂等 join 必须 joinable() 检查。
3. **优雅停机顺序**：先 close 队列再 join worker——反了会死锁
   （worker 等队列，队列等人 close）。
4. shutdown 幂等：显式调用 + 析构自动调用会撞车 → 标志位。

## 408 映射

- **生产者-消费者**：Queue 的 mutex+CV 就是标准解；关闭队列 = 结束信号
- **调度器**：Executor = 就绪队列 + 多处理机；FIFO 调度（V2 加优先级）
- **线程回收**：join 必须且只能一次——对应"进程必须 wait 一次"
- **死锁预防**：shutdown 顺序（close 先行）就是"破坏循环等待"的实例
- **RAII**：Executor 析构自动 shutdown、Worker 析构自动 join——
  异常安全，忘调也不泄漏

## Conclusion

多线程 Task Runtime 实现并验证通过：Worker 循环、Executor 线程池、
优雅停机、幂等回收、并发无丢失。Month 3 核心验收对象已就位，
下一步补 Device/Telemetry 深度（每 worker 计数、队列水位）和
优先级调度（V2）。

## Next Question

- 优先级：V2 若 Task 带 Priority，Queue 要改成优先队列
  （std::priority_queue），pop 取最高优先——408 里就是
  优先级调度算法（PS）。
- Worker 立即急停：当前 stop_ 是预留字段，V2 可做
  "带超时轮询 stop_"的 pop 变体，支持取消长任务。
