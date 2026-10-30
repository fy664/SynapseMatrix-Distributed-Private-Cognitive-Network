// SynapseMatrix — WorkStealingPool：多队列 Work-Stealing 线程池
// =============================================================
// 规格书 Month 3 环节 11（2026-10-30）：Work-Stealing。
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
//     - Java ForkJoinPool：每个工作线程一个本地队列，窃取对端队尾
//     - 408 考点：多处理机负载均衡（把忙 CPU 的任务移给闲 CPU）
//
// 偷的方向为什么是"别人的队尾"：
//   本地取队头（最近提交的，cache 友好，LIFO）；
//   偷取取队尾（最旧的）——偷走最旧的任务，被偷方（拥塞方）
//   手头的新任务不受影响，整体延迟更小。这是 ForkJoinPool
//   的经典做法：减少被偷方"刚准备好就被抢"的挫败感。
//
// 亲和性提交（submit_to）：
//   轮询分发（submit）让队列均匀，偷取几乎不发生；
//   但真实场景提交方常偏爱某 Worker（数据亲和/分区）。
//   submit_to(worker, task) 显式指定队列 → 制造不均 →
//   Work-Stealing 才有用武之地（也是测试能确定性地验证偷取）。
//
// 线程安全设计（无死锁论证）：
//   - 每队列一把 mutex；Worker 从不"同时持有两把锁"：
//     取自己的 → 锁A → 取走 → 解锁；偷别人的 → 锁B → 取走 → 解锁。
//     一次只持一把锁 → 不可能形成锁等待环 → 无死锁。
//   - closed_ 是 atomic；shutdown 置位后 Worker 最多再睡 1ms
//     就会被唤醒检查退出（简单可靠，不需要条件变量）。
//   - executed_/stolen_ 是 vector<atomic>，构造定大小
//     （atomic 不可移动，环节 3 踩坑教训）。
//
// 408 映射：本地队列 = 每 CPU 的就绪队列；偷取 = 负载均衡
//   （多处理机系统中，把忙处理机队列的任务迁移到闲处理机）。

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
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

    // 不可复制：线程/互斥锁不可复制
    WorkStealingPool(const WorkStealingPool&) = delete;
    WorkStealingPool& operator=(const WorkStealingPool&) = delete;

    // 提交任务：轮询分发到下一个本地队列（负载均匀）
    // 队列已关闭（shutdown 后）返回 false
    bool submit(Task task);

    // 亲和性提交：显式指定放进哪个 Worker 的本地队列
    // （模拟"提交方偏爱某队列"的真实场景；越界或已关闭返回 false）
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
    // 每个 Worker 的本地队列：mutex + deque（队头取、队尾偷）
    struct LocalQueue {
        std::mutex mutex;
        std::deque<Task> tasks;
    };

    // 每个 Worker 的主循环（实现见 work_stealing.cpp）：
    //   取自己的队头 → 空则按偏移顺序偷别人队尾 → 全空检查关闭
    void worker_loop(std::size_t id);

    std::vector<std::unique_ptr<LocalQueue>> queues_; // 本地队列（每 worker 一个）
    std::vector<std::thread> threads_;                // worker 线程
    std::atomic<bool> closed_{false};                 // 关闭标志
    std::vector<std::atomic<std::size_t>> executed_;  // per-worker 执行计数
    std::vector<std::atomic<std::size_t>> stolen_;    // per-worker 偷取计数
    std::mutex submit_mutex_;   // 保护 next_queue_（多线程 submit 竞争）
    std::size_t next_queue_ = 0; // 轮询指针（下一个分发目标）
};

}  // namespace synapse::runtime
