// SynapseMatrix — Executor 实现（Month 3 环节 2）
// ==============================================
// Executor 是"线程池调度器"：N 个 Worker 共享一个队列。
//   - submit   : 生产者入口（转发给 queue_.push）
//   - shutdown : 优雅停机（close 队列 → 等全部 Worker 退出）
//   - 析构     : 自动 shutdown（RAII）
//
// 为什么 shutdown 是"优雅"的：
//   先 close 队列（不再收新活），已入队的任务仍会被 Worker 取走执行，
//   队列清空后 pop 返回 nullopt，Worker 自然退出。
//   不会出现"任务没干完就被杀"——这是线程池停机的基本原则。
//
// 408 映射：Executor = 操作系统的调度器——
//   就绪队列（TaskQueue）+ 处理机集合（Worker）+ 调度策略（FIFO）。
//   优雅停机 ≈ 系统关机的"先让进程处理完、再回收资源"。
//
// 语法讲解：
//   - emplace_back(参数...)：在 vector 尾部直接构造（避免临时对象拷贝）
//   - reserve(n)：预分配容量，避免扩容时反复移动 Worker（移动会
//     搬运 std::thread 句柄，虽安全但浪费）
//   - for (auto& w : workers_) w.join()：范围 for 遍历，逐个回收

#include "runtime/executor.hpp"

#include <utility>

namespace synapse::runtime {

Executor::Executor(std::size_t num_workers,
                   std::chrono::milliseconds aging_interval,
                   int aging_max_boost,
                   std::size_t max_capacity)
    : queue_(aging_interval, aging_max_boost, max_capacity), // 背压阈值透传
      telemetry_(num_workers),      // Telemetry 定大小（每 worker 一格）
      num_workers_(num_workers) {
    // 预分配指针数组：unique_ptr 移动便宜，容量不足时只搬指针
    workers_.reserve(num_workers);
    for (std::size_t i = 0; i < num_workers; ++i) {
        // make_unique 直接在堆上构造 Worker（id=0..N-1，共享 queue/telemetry）。
        // Worker 构造即启动线程；对象地址稳定，线程内 this 恒有效。
        workers_.push_back(
            std::make_unique<Worker>(static_cast<int>(i), queue_, telemetry_));
    }
}

Executor::~Executor() {
    shutdown();  // RAII：栈上 Executor 销毁即回收全部线程
}

bool Executor::submit(Task task) {
    // 转发给队列；队列已关闭（shutdown 后）push 返回 false
    const bool accepted = queue_.push(std::move(task));
    if (accepted) {
        // 入队成功 → 队列变长，采样当前积压（峰值只增不减）
        telemetry_.record_watermark(queue_.size());
    } else {
        telemetry_.record_rejected();  // 被拒计数（可观测性）
    }
    return accepted;
}

bool Executor::push_wait(Task task, std::chrono::milliseconds timeout) {
    // 阻塞式背压：满则等空位（超时返回 false）
    const bool accepted = queue_.push_wait(std::move(task), timeout);
    if (accepted) {
        telemetry_.record_watermark(queue_.size());
    } else {
        telemetry_.record_rejected();  // 超时未入队也计拒绝
    }
    return accepted;
}

void Executor::shutdown() {
    // 幂等保护：shutdown 可被多次调用（如显式调用 + 析构自动调用）。
    if (shutdown_done_) {
        return;
    }
    shutdown_done_ = true;

    queue_.close();           // 1. 不再收新任务（已入队的照常执行）
    for (auto& wp : workers_) {
        if (wp) {
            wp->join();       // 2. 等每个 Worker 干完手头的活并退出
        }
    }
}

}  // namespace synapse::runtime
