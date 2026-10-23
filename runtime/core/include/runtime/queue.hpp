// SynapseMatrix — Queue：线程安全的任务队列（优先级调度版）
// ==========================================================
// 规格书 Month 3 项目对象之一：Queue。
// 这是 Worker 与 Executor 之间的"生产者-消费者"缓冲区：
//   - 生产者（提交方）push 任务
//   - 消费者（Worker 线程）pop 任务
// 多线程同时 push/pop 时必须互斥，否则数据竞争（data race）是未定义行为。
//
// 优先级调度（2026-10-23，Month 3 环节 5）：
//   内部结构从 deque 升级为 std::priority_queue：
//     - 高优先级任务先出队（Task::Priority，High > Normal > Low）
//     - 同优先级按提交顺序 FIFO（seq 序号稳定排序——priority_queue
//       本身不稳定，必须显式加序号保证先来先出）
//   对应 408 操作系统"优先级调度算法（PS）"——就绪队列按优先级
//   排队，高优先级进程先上处理机。
//
// 实现用到的并发原语（Month 3 必学 + 408 2.3 同步与互斥）：
//   - std::mutex              : 互斥锁，保证临界区（队列数据操作）独占
//   - std::condition_variable : 条件变量，让消费者"等待有货"，避免忙轮询
//   - std::unique_lock        : RAII 锁——析构自动解锁，异常安全
//   - std::priority_queue     : 二叉堆，插入/删除 O(log n)，top 恒最大
//
// 关闭语义（close）：
//   队列可被显式关闭；关闭后 push 拒绝、pop 在队列清空后返回 nullopt。
//   Executor 停机时先 close 再让 Worker 自然退出——优雅关闭。
//
// 语法讲解：
//   - std::optional<T>        : C++17 起，"可能有值也可能没有"的包装
//   - wait(pred)              : 条件变量等待谓词为真，等价
//                               while(!pred) wait(lock)——防止虚假唤醒
//   - std::priority_queue 的 Compare：strict weak ordering，返回 true
//     表示 a 排在 b 后面（"a 比 b 小"）；堆顶是"最大"的元素
//   - 注意 priority_queue::top() 返回 const 引用——pop 只能拷贝出
//     top 再移除（V1 接受拷贝开销，任务闭包通常很小）

#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

#include "runtime/task.hpp"

namespace synapse::runtime {

class TaskQueue {
public:
    // 构造空队列（无需额外初始化）
    TaskQueue() = default;

    // 禁止拷贝/赋值：互斥锁不可复制（std::mutex 不可拷贝构造）
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    // ---- 生产者侧 ----
    // 入队一个任务；队列已关闭时返回 false（任务被拒绝）
    bool push(Task task) {
        {
            // lock_guard：作用域结束（}处）自动解锁——异常也会解锁
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) {
                return false;      // 已关闭：拒绝新任务
            }
            // 打上入队序号（同优先级 FIFO 的稳定键），放入二叉堆
            const std::uint64_t seq = seq_counter_++;
            queue_.push(Entry{std::move(task), seq});
        }
        cv_.notify_one();  // 通知一个等待中的消费者"有新任务"
        return true;
    }

    // ---- 消费者侧 ----
    // 取一个任务；队列关闭且清空时返回 nullopt（worker 应退出）
    // wait 用谓词重载：等价于 while(!ready) cv_.wait(lock)，
    // 自动处理虚假唤醒（spurious wakeup）
    std::optional<Task> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] {
            return !queue_.empty() || closed_;   // 有货或已关闭才醒来
        });
        if (queue_.empty()) {
            return std::nullopt;   // 关闭且空：告诉调用方"结束"
        }
        // priority_queue::top() 是 const 引用，只能拷贝
        // （V1 接受：Task 的 std::function 小闭包拷贝代价低）
        Task t = queue_.top().task;
        queue_.pop();
        return t;
    }

    // ---- 管理侧 ----
    // 关闭队列：之后 push 被拒；pop 在清空后返回 nullopt
    void close() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
        }
        cv_.notify_all();  // 唤醒所有等待者，让它们检查关闭状态
    }

    // 当前队列长度（调试/Telemetry 用；注意锁外看会有竞态，仅近似）
    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    // 队列元素：任务 + 入队序号（同优先级按序号升序 = FIFO）
    struct Entry {
        Task task;
        std::uint64_t seq;   // 提交顺序号（push 时分配，单调递增）
    };

    // 堆排序规则（strict weak ordering）：
    //   1. 优先级高者优先（高优先级"更大"，堆顶是最大的）
    //   2. 同优先级，seq 小者优先（先来先出，稳定）
    struct Compare {
        bool operator()(const Entry& a, const Entry& b) const {
            if (a.task.priority() != b.task.priority()) {
                return a.task.priority() < b.task.priority();
            }
            return a.seq > b.seq;   // seq 小 = "更大" = 先出
        }
    };

    mutable std::mutex mutex_;      // mutable：size() const 也要能加锁
    std::condition_variable cv_;    // 消费者等待"队列非空"
    std::priority_queue<Entry, std::vector<Entry>, Compare> queue_; // 二叉堆
    std::uint64_t seq_counter_ = 0; // 入队序号分配器（锁内递增）
    bool closed_ = false;           // 关闭标志
};

}  // namespace synapse::runtime
