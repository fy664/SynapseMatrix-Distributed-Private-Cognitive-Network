// SynapseMatrix — Worker：执行线程（接口声明 + 动态缩容支持）
// ==============================================================
// 规格书 Month 3 项目对象之一：Worker。
//
// 职责（规格书验收：多线程 Task Runtime）：
//   每个 Worker 是一个 std::thread，循环做：
//     1. 从共享 TaskQueue 取任务（pop/pop_wait，nullopt 即空闲或结束）
//     2. 执行任务（task()）
//     3. 上报 Telemetry（执行数 +1、耗时）
//
// 动态缩容（环节 10，2026-10-29）：
//   线程池扩容后不能只增不减——空闲的线程长期占着栈（默认 1MB）
//   和内核对象。Worker 支持"空闲超时退役"：
//     - idle_timeout > 0 时，run() 改用 pop_wait 轮询；
//       连续超时（即线程闲着）且当前存活线程数 > 核心数（core_workers）
//       → 线程自我退役（CAS 递减存活计数，保证不会缩到核心数以下）
//     - retired() 返回 true 后，由 Executor 统一 join + 移除（回收）
//   对应 408：进程/线程的资源回收策略（PCB 释放、栈回收）；
//   对应 Java ThreadPoolExecutor 的 keepAliveTime + 核心/最大线程数。
//
// 为什么用 CAS 递减而不先读后减：
//   多个 worker 可能同时判断"我该退役"，先读后减会让所有人一起退，
//   把线程数打到核心数以下。compare_exchange_weak 保证"看准了再减"，
//   减完才退役——这是无锁算法"读取-校验-更新"的标准套路。
//
// 408 映射：Worker 就是"进程/线程执行体"的模型——
//   循环取任务执行 = 操作系统的就绪队列 + 处理机调度。

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>

#include "runtime/queue.hpp"
#include "runtime/telemetry.hpp"

namespace synapse::runtime {

class Worker {
public:
    // 构造：启动一个后台线程（std::thread 构造即开始运行）。
    //   worker_id      : 本线程的编号（Telemetry 槽位索引）
    //   queue          : 共享任务队列（引用，不持有所有权）
    //   telemetry      : 统计（引用）
    //   live_workers   : 存活线程计数（共享引用；构造 +1，退役 -1）。
    //                    由 Executor 持有，多个 Worker 共享。
    //   core_workers   : 核心线程数——少于这个数时线程永不退役
    //                    （常驻线程，保证池子最低水位）
    //   idle_timeout   : 空闲超时（0 = 禁用缩容，永远阻塞取任务）
    Worker(int worker_id,
           TaskQueue& queue,
           Telemetry& telemetry,
           std::atomic<int>& live_workers,
           std::size_t core_workers,
           std::chrono::milliseconds idle_timeout);

    // 析构：请求停止并等待线程退出（否则 std::thread 析构会 std::terminate）
    // 注意：join 必须是幂等的（joinable 检查），因为 Executor::shutdown()
    // 已显式 join 过，析构时再 join 不能 UB。
    ~Worker();

    // 等待线程退出（幂等：重复调用安全）。
    // 线程退出条件：队列 close 且清空后 pop 返回 nullopt → run() 返回。
    void join();

    // 线程是否已退役（自己空闲超时退出，等 Executor 回收）。
    // Executor 的 reap_retired() 遍历数组时查这个标志：
    //   真 → join + 从数组移除；假 → 继续服役。
    [[nodiscard]] bool retired() const noexcept { return retired_.load(); }

    // 不让复制：线程不可复制
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    [[nodiscard]] int id() const noexcept { return worker_id_; }

private:
    // 线程主循环（实现见 worker.cpp）：
    //   缩容禁用：  while (auto task = queue_.pop()) { ... }
    //                （pop 返回 nullopt = 队列关闭且空 → 退出）
    //   缩容启用：  while (true) {
    //                  task = queue_.pop_wait(idle_timeout);
    //                  task 存在 → 执行；
    //                  nullopt 且队列已关闭 → 退出；
    //                  nullopt 且空闲超时 → 富余则 CAS 退役，否则继续
    //                }
    void run();

    // 尝试退役（实现见 worker.cpp）：CAS 抢退役名额。
    //   成功：存活计数已 -1，返回 true（线程应退出）；
    //   失败：已到核心水位或名额被抢，返回 false（继续服役）。
    bool try_retire();

    int worker_id_;
    TaskQueue& queue_;
    Telemetry& telemetry_;
    std::atomic<int>& live_workers_;  // 存活计数（引用 Executor 的成员）
    std::size_t core_workers_;        // 核心线程数（低于此不退役）
    std::chrono::milliseconds idle_timeout_;  // 空闲超时（0=禁用）
    std::atomic<bool> retired_{false};        // 已退役标志（供回收）
    std::thread thread_;              // 线程句柄（join 时用）
};

}  // namespace synapse::runtime
