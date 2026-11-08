// SynapseMatrix — WorkStealingPool：多队列 Work-Stealing 线程池
// =============================================================
// 规格书 Month 3 环节 11（2026-10-30）：Work-Stealing。
// 环节 13（2026-11-08）：本地队列无锁化（mutex+deque → Vyukov
//   MPMC 无锁环形队列），提交路径同步去锁。
//
// 与 Executor（单共享队列）的区别：
//   Executor        : N 个 Worker 抢同一个队列（所有线程同吃一锅饭）
//   WorkStealingPool: 每个 Worker 一个"本地队列"，先吃自己的；
//                     自己的吃完了就去偷别人的（负载均衡）
//
// 为什么需要 Work-Stealing（408 + 工程）：
//   单队列在多核下是热点——所有 Worker 抢同一把锁，吞吐上不去。
//   各吃各的队列能并行入队/出队（锁分片），只有"偷"才跨队列。
//   真实世界的例子：
//     - Go runtime 的 G 调度器：每个 P 有本地 runqueue，
//       空了就偷别的 P 的任务
//     - Java ForkJoinPool：每个工作线程一个本地队列，窃取对端端头
//     - 408 考点：多处理机负载均衡（把忙 CPU 的任务迁移给闲 CPU）
//
// 环节 13 为什么要把 mutex 换成无锁（408 + 工程）：
//   mutex 是"阻塞式互斥"——抢不到锁的线程被挂起/唤醒，涉及内核态
//   切换（几微秒级）；而任务本身可能只跑几百纳秒。锁的代价超过
//   任务本身 → 线程池"越忙越慢"。
//   无锁（lock-free）用一条 CAS 指令完成"占用"，失败就重试，
//   不发生上下文切换——对应 408 "同步与互斥"里硬件原子指令
//   与自旋锁的思想，也是 Disruptor/Go/Java 高性能队列的标准做法。
//
// 无锁队列结构（Vyukov MPMC bounded ring，工业界标准实现）：
//   环形数组 capacity=1024（2 的幂 → 索引用 & (cap-1) 代替 %）；
//   每槽一个 seq 版本号：
//     seq == pos          → 槽位空闲，可写（生产者）
//     seq == pos + 1      → 数据已写好，可读（消费者）
//     seq == pos + cap    → 已被消费，等待下一轮（可写）
//   生产者 CAS enqueue_pos 抢槽，消费者 CAS dequeue_pos 抢槽——
//   两者互不阻塞，不同槽位完全并行。
//
// 线程安全设计：
//   - 无锁：队列 push/pop 纯 CAS；enqueue/dequeue 位置各自原子
//   - 内存序：写槽位用 release（保证数据先写、版本号后发布）；
//     读槽位用 acquire（保证看到数据后再消费）——这是无锁编程
//     正确性的关键（错误的内存序 = 诡异崩溃）
//   - 无死锁：不存在锁，自然无死锁；不会"一个线程被挂起拖住全池"
//   - closed_ 是 atomic；shutdown 置位后 Worker 最多再睡 1ms
//     就会被唤醒检查退出（简单可靠，不需要条件变量）
//   - executed_/stolen_ 是 vector<atomic>，构造定大小
//     （atomic 不可移动，环节 3 踩坑教训）
//
// 408 映射：本地队列 = 每 CPU 的就绪队列；偷取 = 负载均衡
//   （多处理机系统中，把忙处理机队列的任务迁移到闲处理机）；
//   无锁 = 原子指令 + 自旋重试（忙等），比阻塞互斥轻量。

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "runtime/task.hpp"

namespace synapse::runtime {

class WorkStealingPool {
public:
    // 构造：创建 num_workers 个 Worker，每人一个本地队列
    explicit WorkStealingPool(std::size_t num_workers);

    // 析构：自动 shutdown（RAII）
    ~WorkStealingPool();

    // 不可复制：线程不可复制
    WorkStealingPool(const WorkStealingPool&) = delete;
    WorkStealingPool& operator=(const WorkStealingPool&) = delete;

    // 提交任务：轮询分发到下一个本地队列（负载均匀）。
    // 队列已关闭（shutdown 后）或目标队列已满（1024）返回 false。
    bool submit(Task task);

    // 亲和性提交：显式指定放进哪个 Worker 的本地队列
    // （模拟"提交方偏爱某队列"的真实场景；越界/已关闭/已满返回 false）
    bool submit_to(std::size_t worker, Task task);

    // 优雅停机：标记关闭 → 等全部 Worker 消费完本地任务并退出
    // 幂等：可多次调用
    void shutdown();

    // ---- 观察 ----
    [[nodiscard]] std::size_t num_workers() const noexcept {
        return queues_.size();
    }

    // 某 Worker 已执行的任务数（自己队列 + 偷来的）
    [[nodiscard]] std::size_t executed(std::size_t worker) const noexcept {
        return executed_[worker].load(std::memory_order_relaxed);
    }

    // 某 Worker 偷到的任务数（>0 说明偷取确实发生过）
    [[nodiscard]] std::size_t stolen(std::size_t worker) const noexcept {
        return stolen_[worker].load(std::memory_order_relaxed);
    }

    // 全池总执行数（调试/验收）
    [[nodiscard]] std::size_t total_executed() const noexcept;

private:
    // 每个 Worker 的本地队列：无锁有界环形队列（Vyukov MPMC）。
    // 容量固定 1024（2^10），满则 push 失败（背压语义，与
    // Executor 的有界队列一致）。
    class LocalQueue {
    public:
        static constexpr std::size_t kCapacity = 1024;  // 必须是 2 的幂
        LocalQueue();

        // 生产：抢一个空槽写入任务；队列满返回 false
        bool push(Task task);

        // 消费：抢一个有数据的槽取出任务；队列空返回 nullopt
        std::optional<Task> pop();

    private:
        // 槽位：版本号 seq + 任务本体。
        // seq 是无锁算法区分"空槽/满槽/下一轮"的唯一依据——
        // 只靠 head/tail 位置无法判断（会读到别人还没写完的槽）。
        struct Cell {
            std::atomic<std::size_t> seq;
            std::optional<Task> task;
        };
        std::vector<Cell> cells_;                  // 环形数组（定大小）
        std::atomic<std::size_t> enqueue_pos_{0};  // 下一个可写位置
        std::atomic<std::size_t> dequeue_pos_{0};  // 下一个可读位置
    };

    // 每个 Worker 的主循环（实现见 work_stealing.cpp）：
    //   取自己的队头 → 空则按偏移顺序偷别人的队头 → 全空检查关闭
    void worker_loop(std::size_t id);

    std::vector<std::unique_ptr<LocalQueue>> queues_; // 本地队列（每 worker 一个）
    std::vector<std::thread> threads_;                // worker 线程
    std::atomic<bool> closed_{false};                 // 关闭标志
    std::vector<std::atomic<std::size_t>> executed_;  // per-worker 执行计数
    std::vector<std::atomic<std::size_t>> stolen_;    // per-worker 偷取计数
    // 轮询指针：原子 fetch_add 取号（多线程 submit 不需要锁——
    // 环节 13：提交路径同步去锁）
    std::atomic<std::size_t> next_queue_{0};
};

}  // namespace synapse::runtime
