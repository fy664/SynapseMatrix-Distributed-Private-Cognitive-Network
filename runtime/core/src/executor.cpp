// SynapseMatrix — Executor 实现（Month 3 环节 2/9）
// ==============================================
// Executor 是"线程池调度器"：N 个 Worker 共享一个队列。
//   - submit   : 生产者入口（转发给 queue_.push）
//   - push_wait: 阻塞式提交（满则等空位，环节 8）
//   - shutdown : 优雅停机（close 队列 → 等全部 Worker 退出）
//   - 析构     : 自动 shutdown（RAII）
//   - maybe_grow: 动态扩容（环节 9）——积压超阈值自动加 worker
//
// 为什么 shutdown 是"优雅"的：
//   先 close 队列（不再收新活），已入队的任务仍会被 Worker 取走执行，
//   队列清空后 pop 返回 nullopt，Worker 自然退出。
//   不会出现"任务没干完就被杀"——这是线程池停机的基本原则。
//
// 动态扩容（环节 9）：
//   初始 N 个 worker；每次成功入队后检查积压，若 >= 阈值且
//   worker 数 < 上限 → 新增一个 worker（Worker 构造即启动线程）。
//   这是"监控（peak_pending）→ 治理（扩 worker）"闭环的最后一块。
//   408 映射：多核系统里就绪队列变长 → 操作系统可把更多 CPU
//   加入调度（负载均衡/处理器热插拔的简化版）。
//
// 线程安全：
//   workers_ 数组被 worker_mutex_ 保护（扩容 push_back、遍历 join
//   都在锁内）；队列/telemetry 由各自内部同步。maybe_grow 用
//   "先无锁看阈值 → 加锁复查"的双重检查，避免每次 submit 抢锁。
//
// 语法讲解：
//   - emplace_back(参数...)：在 vector 尾部直接构造（避免临时对象拷贝）
//   - reserve(n)：预分配容量，避免扩容时反复移动指针
//   - make_unique<Worker>(id, queue, tm)：堆上构造 Worker 并接管所有权
//   - std::lock_guard：RAII 锁（构造加锁、析构解锁，异常安全）

#include "runtime/executor.hpp"

#include <utility>

namespace synapse::runtime {

Executor::Executor(std::size_t num_workers,
                   std::chrono::milliseconds aging_interval,
                   int aging_max_boost,
                   std::size_t max_capacity,
                   std::size_t max_workers,
                   std::size_t auto_scale_threshold)
    : queue_(aging_interval, aging_max_boost, max_capacity), // 背压阈值透传
      telemetry_(max_workers ? max_workers : num_workers), // 槽位 = 扩容上限
      num_workers_(num_workers),
      max_workers_(max_workers ? max_workers : num_workers),
      auto_scale_threshold_(auto_scale_threshold) {
    // 预分配指针数组：unique_ptr 移动便宜，容量不足时只搬指针
    workers_.reserve(max_workers_);
    for (std::size_t i = 0; i < num_workers; ++i) {
        // make_unique 直接在堆上构造 Worker（共享 queue/telemetry）。
        // Worker 构造即启动线程；对象地址稳定，线程内 this 恒有效。
        workers_.push_back(
            std::make_unique<Worker>(static_cast<int>(i), queue_, telemetry_));
    }
    next_worker_id_ = num_workers;   // 后续扩容从该 id 继续分配
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
        maybe_grow();   // 积压超阈值则自动扩 worker（环节 9）
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
        maybe_grow();   // 同上
    } else {
        telemetry_.record_rejected();  // 超时未入队也计拒绝
    }
    return accepted;
}

void Executor::maybe_grow() {
    // 快速路径：未开启自动扩容，或已到上限 → 直接返回（无锁）
    if (auto_scale_threshold_ == 0) {
        return;
    }
    // 快速路径：积压未达阈值 → 不扩（无锁看 size，近似即可）
    if (queue_.size() < auto_scale_threshold_) {
        return;
    }
    // 慢路径：加锁复查（双重检查），避免并发重复扩容
    std::lock_guard<std::mutex> lock(worker_mutex_);
    if (workers_.size() >= max_workers_) {
        return;                       // 已到上限
    }
    if (queue_.size() < auto_scale_threshold_) {
        return;                       // 复查：阈值已回落
    }
    // 新增 worker：构造即启动线程，id 递增（槽位在 telemetry 内预分配）
    const int new_id = static_cast<int>(next_worker_id_++);
    workers_.push_back(std::make_unique<Worker>(new_id, queue_, telemetry_));
}

void Executor::shutdown() {
    // 幂等保护：shutdown 可被多次调用（如显式调用 + 析构自动调用）。
    if (shutdown_done_) {
        return;
    }
    shutdown_done_ = true;

    queue_.close();           // 1. 不再收新任务（已入队的照常执行）
    std::lock_guard<std::mutex> lock(worker_mutex_);
    for (auto& wp : workers_) {
        if (wp) {
            wp->join();       // 2. 等每个 Worker 干完手头的活并退出
        }
    }
}

}  // namespace synapse::runtime
