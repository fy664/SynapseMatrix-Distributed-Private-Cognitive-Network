// SynapseMatrix — 多线程 Task Runtime 演示（Month 3 验收预览）
// ====================================================================
// 用 Executor（自己实现的线程池，优先级调度版）提交任务，
// 展示：submit → 优先级插队 → 并发执行 → 优雅关闭 → Telemetry 统计。
//
// 408 映射：生产者-消费者 + 优先级调度（PS）——
//   高优先级任务插队先执行；同优先级 FIFO（seq 稳定排序）。
//   对应"就绪队列按优先级排队，高优先级进程先上处理机"。
//
// 运行：runtime/build/Release/demo_threadpool.exe

#include <chrono>
#include <cstdio>
#include <thread>

#include "runtime/executor.hpp"
#include "runtime/task.hpp"

using namespace std::chrono_literals;
using synapse::runtime::Executor;
using synapse::runtime::Priority;
using synapse::runtime::Task;

int main() {
    constexpr int kWorkers = 2;

    Executor executor(kWorkers);

    // 先提交 6 个 Normal 任务，再提交 2 个 High——
    // High 应插队先执行（优先级调度），Normal 之间保持 FIFO
    for (int i = 0; i < 6; ++i) {
        executor.submit(Task([i] {
            std::this_thread::sleep_for(5ms);
            std::printf("  [normal %d] 执行中\n", i);
        }, "normal-" + std::to_string(i), Priority::Normal));
    }
    for (int i = 0; i < 2; ++i) {
        executor.submit(Task([i] {
            std::this_thread::sleep_for(5ms);
            std::printf("  [HIGH %d]  插队执行！\n", i);
        }, "high-" + std::to_string(i), Priority::High));
    }

    executor.shutdown();  // 优雅关闭：等全部任务执行完

    // ---- Telemetry 输出 ----
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

    // ---- 背压演示（环节 7）：容量 3 的有界队列 ----
    {
        Executor bounded(1, 0ms, 2, 3);   // 单 worker + 容量 3
        // 占住 worker → 队列能积压到 3；第 4 个起被拒
        bounded.submit(Task([] { std::this_thread::sleep_for(30ms); },
                            "occupy", Priority::High));
        int accepted = 0;
        for (int i = 0; i < 6; ++i) {
            if (bounded.submit(Task([] {}, "t" + std::to_string(i),
                                    Priority::Normal))) {
                ++accepted;
            }
        }
        bounded.shutdown();
        std::printf("\n=== 背压演示（容量 3） ===\n");
        std::printf("提交 6 个，入队 %d 个，拒绝 %d 个（积压峰值 %zu）\n",
                    accepted,
                    static_cast<int>(bounded.telemetry().rejected()),
                    bounded.peak_pending());
    }

    // ---- 阻塞式背压演示（环节 8）：push_wait 满则等 ----
    {
        Executor bounded(1, 0ms, 2, 3);   // 单 worker + 容量 3
        bounded.submit(Task([] { std::this_thread::sleep_for(20ms); },
                            "occupy", Priority::High));
        int accepted = 0;
        for (int i = 0; i < 6; ++i) {
            if (bounded.push_wait(Task([] {}, "t" + std::to_string(i),
                                       Priority::Normal),
                                  2000ms)) {
                ++accepted;   // 满则等空位，最终全部挤进去
            }
        }
        bounded.shutdown();
        std::printf("\n=== 阻塞式背压演示（容量 3） ===\n");
        std::printf("push_wait 6 个，全部入队 %d 个，拒绝 %d 个\n",
                    accepted,
                    static_cast<int>(bounded.telemetry().rejected()));
    }
    return 0;
}
