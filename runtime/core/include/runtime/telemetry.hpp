// SynapseMatrix — Telemetry：运行时可观测性（Month 3 项目对象之一）
// ====================================================================
// 规格书 Month 3 项目对象之一：Telemetry。
// 语义：统计 Runtime 的运行指标——谁执行了多少任务、队列积压多少。
// 可观测性（"先可观测，后优化"是规格书核心原则之一）——
// 没有计数，优化无从谈起。
//
// 线程安全设计：
//   - tasks_done_ 用 std::atomic<size_t>：多个 Worker 同时 +1 也安全
//     （原子量自带互斥语义，比加锁更快——无锁的 read-modify-write）
//   - 队列长度由 TaskQueue::size() 提供（内部有锁）
//
// 语法讲解：
//   - std::atomic<T>：对 T 的原子操作；fetch_add 是"读-改-写"原子版
//   - ++ 运算符重载：原子量也支持 prefix/postfix ++（内部是 fetch_add）
//   - const 方法读原子量用 load()，写用 store()/fetch_add()

#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace synapse::runtime {

class Telemetry {
public:
    Telemetry() = default;

    // Worker 完成一个任务时调用：全局计数 +1
    void record_task_done(int /*worker_id*/) {
        tasks_done_.fetch_add(1);   // 原子自增（多线程安全）
    }

    // 已执行任务总数（快照）
    [[nodiscard]] std::size_t tasks_done() const noexcept {
        return tasks_done_.load();  // 原子读
    }

    // 按 worker 分组的计数（V2：每 worker 独立计数，用于负载均衡）
    // TODO(用户)：若需要，把 int 参数存进 vector<atomic<size_t>>
    //   当前实现忽略 worker_id，统一累计。

private:
    std::atomic<std::size_t> tasks_done_{0};  // 原子计数：无锁并发安全
};

}  // namespace synapse::runtime
