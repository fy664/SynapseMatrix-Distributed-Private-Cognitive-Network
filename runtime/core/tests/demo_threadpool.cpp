// SynapseMatrix — 最小线程池演示（Month 3 环境验证 + 教学示例）
// ====================================================================
// 目的：
//   1. 验证工具链闭环（vcvars64 + CMake + C++20 编译/运行）
//   2. 演示 Month 3 全部核心并发原语，作为你实现
//      Worker/Executor 的"最小参考骨架"
//
// 演示内容：
//   - 手工创建 3 个消费者线程（std::thread），共享 1 个 TaskQueue
//   - 主线程投递 10 个任务（每个打印"我是谁 + 干了什么"）
//   - 关闭队列 → 消费者自然退出 → join 回收
//
// 408 映射：这就是"生产者-消费者问题"的直接实现——
//   互斥（mutex）保护共享队列、条件变量（cv）避免忙等、
//   join 对应进程/线程的回收（孤儿/僵尸线程管理）。
//
// 运行：cmake --build 后执行 runtime/build/Release/demo_threadpool.exe

#include <cstdio>
#include <thread>
#include <vector>

#include "runtime/queue.hpp"
#include "runtime/task.hpp"

using synapse::runtime::Task;
using synapse::runtime::TaskQueue;

int main() {
    TaskQueue queue;
    constexpr int kWorkers = 3;      // 消费者线程数
    constexpr int kTasks = 10;       // 任务数

    // 创建消费者线程：每个线程循环 pop，拿到任务就执行
    std::vector<std::thread> workers;
    for (int w = 0; w < kWorkers; ++w) {
        workers.emplace_back([&, w] {
            while (auto t = queue.pop()) {   // nullopt = 结束信号
                std::printf("  [worker %d] 执行任务: %s\n", w, t->id().c_str());
                (*t)();                      // 跑任务体
            }
            std::printf("  [worker %d] 队列已关闭，退出\n", w);
        });
    }

    // 主线程：投递任务（生产者侧）
    for (int i = 0; i < kTasks; ++i) {
        queue.push(Task([i] {
            std::printf("    任务体 %d：假装在计算…\n", i);
        }, "task-" + std::to_string(i)));
    }

    // 优雅关闭：先通知队列不再收活，等所有 worker 把手头活干完退出
    queue.close();
    for (auto& th : workers) {
        th.join();
    }

    std::printf("演示完成：%d 个任务被 %d 个 worker 消费。\n", kTasks, kWorkers);
    return 0;
}
