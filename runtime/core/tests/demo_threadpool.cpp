// SynapseMatrix — 多线程 Task Runtime 演示（Month 3 验收预览）
// ====================================================================
// 用 Executor（自己实现的线程池）提交 10 个任务，
// 展示：submit → 并发执行 → 优雅关闭 → Telemetry 深化统计
//（per-worker 分布 / 平均耗时 / 拒绝数）。
//
// 408 映射：这就是"生产者-消费者问题"的直接实现——
//   互斥（mutex）保护共享队列、条件变量（cv）避免忙等、
//   join 对应进程/线程的回收（孤儿/僵尸线程管理）。
//   per-worker 计数 ≈ 多处理机系统里各 CPU 的运行统计。
//
// 运行：runtime/build/Release/demo_threadpool.exe

#include <cstdio>
#include <chrono>
#include <thread>

#include "runtime/executor.hpp"
#include "runtime/task.hpp"

using namespace std::chrono_literals;
using synapse::runtime::Executor;
using synapse::runtime::Task;

int main() {
    constexpr int kWorkers = 4;
    constexpr int kTasks = 10;

    Executor executor(kWorkers);  // 4 个 worker 的线程池

    // 提交 10 个任务：每个模拟 5ms 工作
    for (int i = 0; i < kTasks; ++i) {
        bool accepted = executor.submit(Task([i] {
            std::this_thread::sleep_for(5ms);   // 模拟计算耗时
            std::printf("  任务 %d 执行中\n", i);
        }, "task-" + std::to_string(i)));
        if (!accepted) {
            std::printf("任务 %d 被拒绝（线程池已关闭？）\n", i);
        }
    }

    executor.shutdown();  // 优雅关闭：等全部任务执行完

    // ---- Telemetry 深化输出 ----
    const auto& tm = executor.telemetry();
    std::printf("\n=== Telemetry 统计 ===\n");
    std::printf("总执行数        : %zu\n", tm.tasks_done());
    std::printf("平均耗时/任务   : %.2f ms\n", tm.avg_task_ms());
    std::printf("拒绝数          : %zu\n", tm.rejected());
    std::printf("队列积压峰值    : %zu\n", executor.peak_pending());
    std::printf("per-worker 分布 :");
    for (std::size_t w = 0; w < tm.worker_count(); ++w) {
        std::printf("  w%zu=%zu", w, tm.task_count(static_cast<int>(w)));
    }
    std::printf("\n");
    return 0;
}
