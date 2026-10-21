// SynapseMatrix — Worker：执行线程（接口声明，实现由你完成）
// ==============================================================
// 规格书 Month 3 项目对象之一：Worker。
//
// 职责（规格书验收：多线程 Task Runtime）：
//   每个 Worker 是一个 std::thread，循环做：
//     1. 从共享 TaskQueue 取任务（pop，返回 nullopt 即退出）
//     2. 执行任务（task()）
//     3. 上报 Telemetry（执行数 +1、耗时）
//
// 你要实现的部分（TODO）：构造时启动线程、析构时优雅停机。
// 已给的提示在注释里；先读 queue.hpp 理解 pop 的关闭语义再动手。
//
// 为什么接口这样设计：
//   - worker_id       : 每个线程一个，Telemetry 需要知道谁干了什么
//   - queue           : 引用共享队列（不持有所有权，避免复制锁）
//   - stop_token      : 用 std::atomic<bool> 表达"该停下了"
//
// 408 映射：Worker 就是"进程/线程执行体"的模型——
//   循环取任务执行 = 操作系统的就绪队列 + 处理机调度。

#pragma once

#include <atomic>
#include <thread>

#include "runtime/queue.hpp"
#include "runtime/telemetry.hpp"

namespace synapse::runtime {

class Worker {
public:
    // 构造：启动一个后台线程（std::thread 构造即开始运行）
    // 实现要点：
    //   thread_ = std::thread([this] { run(); });
    //   run() 是成员函数，lambda 捕获 this 即可调用
    Worker(int worker_id, TaskQueue& queue, Telemetry& telemetry);

    // 析构：请求停止并等待线程退出（否则 std::thread 析构会 std::terminate）
    // 实现要点：
    //   stop_.store(true);
    //   若线程在 pop 等待中，需要 queue.close() 或 notify 让它醒来
    //   然后 thread_.join()（阻塞直到线程跑完 run()）
    ~Worker();

    // 不让复制：线程不可复制
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    [[nodiscard]] int id() const noexcept { return worker_id_; }

private:
    // 线程主循环：TODO（你实现）
    //   while (!stop_) {
    //     auto t = queue_.pop();
    //     if (!t) break;            // 队列关闭且清空：退出
    //     (*t)();                   // 执行任务
    //     telemetry_.record_task_done(worker_id_);
    //   }
    void run();

    int worker_id_;
    TaskQueue& queue_;
    Telemetry& telemetry_;
    std::atomic<bool> stop_{false};  // 停止标志（原子量：跨线程可见）
    std::thread thread_;             // 线程句柄（join 时用）
};

}  // namespace synapse::runtime
