// SynapseMatrix — Executor：线程池调度器（接口声明，实现由你完成）
// ====================================================================
// 规格书 Month 3 项目对象之一：Executor。
//
// 职责：管理 N 个 Worker 共享一个 TaskQueue——
//   - submit(task)  : 生产者入口，投递任务（转给 queue_.push）
//   - shutdown()    : 优雅停机——close 队列 → 所有 Worker 自然退出 → join
//
// 你要实现的部分（TODO）：
//   - 构造：创建 N 个 Worker（Worker 构造即启动线程）
//   - submit：转发给 queue_.push；失败（队列已关闭）返回 false
//   - shutdown：queue_.close() + 逐个 join Worker（确保全部退出）
//   - 析构：自动调用 shutdown（RAII：对象销毁即回收线程）
//
// 线程数 N 的取舍（408 + 工程直觉）：
//   - 计算密集任务：N ≈ 硬件线程数（std::thread::hardware_concurrency()）
//   - IO 密集任务：可开更多（线程在等 IO 时让出 CPU）
//
// 错误处理注意：
//   - 析构里若线程还没 join 就析构 thread 对象 → std::terminate
//   - shutdown 后再次 submit 必须拒绝（queue 已关闭，push 返回 false）

#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include "runtime/queue.hpp"
#include "runtime/task.hpp"
#include "runtime/telemetry.hpp"
#include "runtime/worker.hpp"

namespace synapse::runtime {

class Executor {
public:
    // 构造：创建 num_workers 个 Worker，共享一个队列 + 一套统计
    // aging_interval/aging_max_boost 透传给队列（老化防饥饿，
    // 默认禁用 = 纯优先级，行为与环节 5 一致）
    // max_capacity：有界队列容量（背压，环节 7）。0 = 无限制；
    //   >0 时积压达上限后 submit 返回 false（拒绝式背压）
    // max_workers：扩容上限（环节 9）。0 = 等于 num_workers（不扩容）。
    //   >0 时 worker 数可动态增长到该上限。
    // auto_scale_threshold：自动扩容阈值。0 = 关闭。
    //   >0 时每次成功入队后检查积压，若 >= 阈值且未达上限，
    //   自动新增一个 worker（监控 → 治理闭环）。
    explicit Executor(std::size_t num_workers,
                      std::chrono::milliseconds aging_interval =
                          std::chrono::milliseconds::zero(),
                      int aging_max_boost = 2,
                      std::size_t max_capacity = 0,
                      std::size_t max_workers = 0,
                      std::size_t auto_scale_threshold = 0);

    // 析构：shutdown()（RAII——栈上对象销毁自动回收所有线程）
    ~Executor();

    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    // 提交任务：队列已关闭时返回 false（调用方可据实处理）
    bool submit(Task task);

    // 阻塞式提交（环节 8）：队列满时等待空位，最多等 timeout；
    // 超时或已关闭返回 false。对应 408 生产者-消费者"满则等"。
    bool push_wait(Task task, std::chrono::milliseconds timeout);

    // 优雅停机：close 队列 → 等待全部 Worker 退出
    // 幂等：可多次调用（第二次起无操作）
    void shutdown();

    // 当前队列积压（Telemetry/调试）
    [[nodiscard]] std::size_t pending() const { return queue_.size(); }

    // 当前活跃 Worker 数（环节 9：动态扩容后可能 > num_workers）
    [[nodiscard]] std::size_t active_workers() const {
        std::lock_guard<std::mutex> lock(worker_mutex_);
        return workers_.size();
    }

    // 队列积压峰值（历史最大；submit 时采样）——背压设计依据
    [[nodiscard]] std::size_t peak_pending() const noexcept {
        return telemetry_.peak_pending();
    }

    // 运行统计（已执行任务总数等）——验收/观察用
    [[nodiscard]] const Telemetry& telemetry() const noexcept {
        return telemetry_;
    }

private:
    TaskQueue queue_;                      // 共享任务队列
    Telemetry telemetry_;                  // 运行统计（Worker 上报到这里）
    // 用 unique_ptr<Worker> 而非 vector<Worker>：
    //   Worker 含 std::atomic/std::thread，不可拷贝也不可移动，
    //   vector 的 reserve/扩容需要可移动构造 → 编译失败。
    //   指针容器：Worker 对象地址永不移动，线程捕获的 this 恒有效。
    std::vector<std::unique_ptr<Worker>> workers_;
    mutable std::mutex worker_mutex_;      // 保护 workers_ 数组（扩容/遍历/join）
    std::size_t num_workers_;              // 初始线程数（记录用）
    std::size_t max_workers_;              // 扩容上限（0=不扩容）
    std::size_t auto_scale_threshold_;     // 自动扩容阈值（0=关闭）
    std::size_t next_worker_id_ = 0;       // 下一个 worker 的 id（递增分配）
    bool shutdown_done_ = false;           // 幂等标志：shutdown 只执行一次

    // 动态扩容（环节 9）：成功入队后检查积压，超阈值且未达上限
    // 则新增一个 worker。双重检查（先无锁看、再加锁复查）避免
    // 每次 submit 都抢锁。
    void maybe_grow();
};

}  // namespace synapse::runtime
