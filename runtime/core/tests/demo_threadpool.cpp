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
#include "runtime/work_stealing.hpp"

using namespace std::chrono_literals;
using synapse::runtime::Executor;
using synapse::runtime::Priority;
using synapse::runtime::Task;
using synapse::runtime::WorkStealingPool;

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

    // ---- 动态扩容演示（环节 9）：积压超阈值自动加 worker ----
    {
        // 初始 1 worker，上限 4；积压 >= 4 时自动扩容
        Executor scaling(1, 0ms, 2, 0, 4, 4);
        scaling.submit(Task([] { std::this_thread::sleep_for(40ms); },
                            "occupy", Priority::High));
        for (int i = 0; i < 24; ++i) {
            scaling.submit(Task([] { std::this_thread::sleep_for(5ms); },
                                "t" + std::to_string(i), Priority::Normal));
        }
        // 注意：worker 数要在 shutdown 前采样（shutdown 后归 0）
        std::printf("\n=== 动态扩容演示 ===\n");
        std::printf("初始 1 worker，高峰自动扩容到 %zu 个，24+1 任务全部执行\n",
                    scaling.active_workers());
        scaling.shutdown();
    }

    // ---- 动态缩容演示（环节 10）：空闲超时自动回收 worker ----
    {
        // 核心 1，上限 4，扩容阈值 4，空闲 150ms 退役
        Executor elastic(1, 0ms, 2, 0, 4, 4, 150ms);
        // 第一波高峰：占住 worker → 积压 → 自动扩容到 4
        elastic.submit(Task([] { std::this_thread::sleep_for(60ms); },
                            "occupy", Priority::High));
        for (int i = 0; i < 30; ++i) {
            elastic.submit(Task([] { std::this_thread::sleep_for(4ms); },
                                "t" + std::to_string(i), Priority::Normal));
        }
        std::printf("\n=== 动态缩容演示 ===\n");
        std::printf("高峰 worker=%zu\n", elastic.active_workers());
        // 任务跑完后空闲 150ms → 自动缩回核心 1 个
        std::this_thread::sleep_for(600ms);
        std::printf("空闲后 worker=%zu（缩回核心）\n", elastic.active_workers());
        // 第二波高峰：再扩回来（弹性闭环）
        elastic.submit(Task([] { std::this_thread::sleep_for(60ms); },
                            "occupy2", Priority::High));
        for (int i = 0; i < 30; ++i) {
            elastic.submit(Task([] { std::this_thread::sleep_for(4ms); },
                                "u" + std::to_string(i), Priority::Normal));
        }
        std::printf("再入高峰 worker=%zu（又扩回）\n", elastic.active_workers());
        elastic.shutdown();
    }

    // ---- Work-Stealing 演示（环节 11）：亲和性提交 + 偷取 ----
    {
        WorkStealingPool pool(4);
        // 亲和性提交：全部塞给 worker 0（模拟"提交方偏爱某队列"）
        for (int i = 0; i < 30; ++i) {
            pool.submit_to(0, Task([] { std::this_thread::sleep_for(3ms); },
                                   "aff" + std::to_string(i)));
        }
        pool.shutdown();
        std::printf("\n=== Work-Stealing 演示 ===\n");
        std::printf("30 个任务全塞 worker0，各 worker 执行分布：\n");
        std::size_t total_stolen = 0;
        for (std::size_t w = 0; w < pool.num_workers(); ++w) {
            std::printf("  worker%zu: 执行 %zu，偷取 %zu\n", w,
                        pool.executed(w), pool.stolen(w));
            total_stolen += pool.stolen(w);
        }
        std::printf("总执行 %zu，总偷取 %zu（负载均衡生效）\n",
                    pool.total_executed(), total_stolen);
    }
    return 0;
}
