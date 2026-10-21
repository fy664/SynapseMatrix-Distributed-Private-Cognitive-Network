// SynapseMatrix — Queue：线程安全的任务队列
// ==================================================
// 规格书 Month 3 项目对象之一：Queue。
// 这是 Worker 与 Executor 之间的"生产者-消费者"缓冲区：
//   - 生产者（提交方）push 任务
//   - 消费者（Worker 线程）pop 任务
// 多线程同时 push/pop 时必须互斥，否则数据竞争（data race）是未定义行为。
//
// 实现用到的并发原语（Month 3 必学 + 408 操作系统 2.3 同步与互斥）：
//   - std::mutex              : 互斥锁，保证临界区（队列数据操作）独占
//   - std::condition_variable : 条件变量，让消费者"等待有货"，避免忙轮询
//   - std::unique_lock        : RAII 锁——析构自动解锁，异常安全
//   - std::deque              : 双端队列，push_back/pop_front 都 O(1)
//
// 关闭语义（close）：
//   队列可被显式关闭；关闭后 push 拒绝、pop 在队列清空后返回 nullopt。
//   Executor 停机时先 close 再让 Worker 自然退出——优雅关闭。
//
// 语法讲解：
//   - std::optional<T>        : C++17 起，"可能有值也可能没有"的包装
//   - wait(pred)              : 条件变量等待谓词为真，等价
//                               while(!pred) wait(lock)——防止虚假唤醒
//   - notify_one/all          : 唤醒一个/全部等待者；push 后 notify_one
//                               足够（一个消费者拿一个任务）
//   - std::lock_guard         : 构造加锁、析构解锁（作用域 RAII）
//   - [[nodiscard]]           : 返回值必须被使用，防漏判

#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
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
            queue_.push_back(std::move(task));  // 移动入队，避免拷贝
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
        Task t = std::move(queue_.front());
        queue_.pop_front();
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
    mutable std::mutex mutex_;      // mutable：size() const 也要能加锁
    std::condition_variable cv_;    // 消费者等待"队列非空"
    std::deque<Task> queue_;        // 任务本体（双端队列）
    bool closed_ = false;           // 关闭标志
};

}  // namespace synapse::runtime
