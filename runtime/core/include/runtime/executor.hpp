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

#include <cstddef>
#include <vector>

#include "runtime/queue.hpp"
#include "runtime/task.hpp"
#include "runtime/worker.hpp"

namespace synapse::runtime {

class Executor {
public:
    // 构造：创建 num_workers 个 Worker 并共享一个队列
    explicit Executor(std::size_t num_workers);

    // 析构：shutdown()（RAII——栈上对象销毁自动回收所有线程）
    ~Executor();

    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    // 提交任务：队列已关闭时返回 false（调用方可据实处理）
    bool submit(Task task);

    // 优雅停机：close 队列 → 等待全部 Worker 退出
    void shutdown();

    // 当前队列积压（Telemetry/调试）
    [[nodiscard]] std::size_t pending() const { return queue_.size(); }

private:
    TaskQueue queue_;                    // 共享任务队列
    std::vector<Worker> workers_;        // Worker 集合（构造即启动线程）
    std::size_t num_workers_;
};

}  // namespace synapse::runtime
