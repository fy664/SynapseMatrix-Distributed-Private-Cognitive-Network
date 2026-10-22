// SynapseMatrix — 多线程 Task Runtime 演示（Month 3 验收预览）
// ====================================================================
// 用 Executor（自己实现的线程池）提交 10 个任务，
// 展示：submit → 并发执行 → 优雅关闭 → Telemetry 统计。
//
// 对比昨天的 demo_threadpool（手工 3 线程）：
//   昨天是"手写消费循环"演示并发原语；
//   今天用 Executor 封装——提交方不再关心线程细节。
//
// 408 映射：这就是"生产者-消费者问题"的直接实现——
//   互斥（mutex）保护共享队列、条件变量（cv）避免忙等、
//   join 对应进程/线程的回收（孤儿/僵尸线程管理）。
//
// 运行：runtime/build/Release/demo_threadpool.exe

#include <cstdio>

#include "runtime/executor.hpp"
#include "runtime/task.hpp"

using synapse::runtime::Executor;
using synapse::runtime::Task;

int main() {
    constexpr int kWorkers = 4;
    constexpr int kTasks = 10;

    Executor executor(kWorkers);  // 4 个 worker 的线程池

    // 提交 10 个任务：每个打印自己是第几个
    for (int i = 0; i < kTasks; ++i) {
        bool accepted = executor.submit(Task([i] {
            std::printf("  任务 %d 执行中\n", i);
        }, "task-" + std::to_string(i)));
        if (!accepted) {
            std::printf("任务 %d 被拒绝（线程池已关闭？）\n", i);
        }
    }

    executor.shutdown();  // 优雅关闭：等全部任务执行完

    std::printf("全部完成：%d 个任务，%d 个 worker，"
                "Telemetry 统计执行数 = %zu\n",
                kTasks, kWorkers, executor.telemetry().tasks_done());
    return 0;
}
