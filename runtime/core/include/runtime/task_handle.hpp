// SynapseMatrix — TaskHandle：任务完成句柄（Month 3 环节 15）
// ==================================================================
// 规格书 Month 3：Task 生命周期治理（结果回传 / 等待）。
//
// 为什么需要句柄（408 + 工程）：
//   到环节 14 为止，submit 只是"扔进去"——调用方拿不回任何东西，
//   也无法知道任务什么时候干完。真实场景必须要：
//     - 等某个任务完成（依赖关系：B 要用 A 的结果）
//     - 带超时地等（不能永远卡住）
//     - 知道结果是"正常完成 / 被取消跳过 / 被拒绝 / 抛异常"
//   这就是 std::future / 回调的同类需求，只是我们用句柄对象表达。
//
// 实现：共享完成状态 + 条件变量
//   TaskCompletion 是句柄背后的共享状态（shared_ptr 共享）；
//   complete() 置结果并 notify_all；wait() 在条件变量上等
//   "结果 != Pending"。这是标准的等待-通知模式。
//
// 408 映射：
//   - 条件变量 wait/notify ↔ 信号量的 P/V 操作（wait 相当于 P，
//     notify 相当于 V）；mutex ↔ 互斥锁（临界区保护共享状态）
//   - wait 前必须在锁内检查条件（防止丢失唤醒）——这是条件变量
//     使用的标准套路，也是 408 常考的"为什么不能用 if 而要用 while"
//   - wait_for ↔ 带超时的 P 操作（超时返回，避免无限期阻塞）
//
// 语法讲解：
//   - std::condition_variable：条件变量（只能配合 unique_lock 使用）
//   - unique_lock<mutex>：可手动加解锁的锁（cv.wait 内部要解锁再挂起）
//   - cv.wait(lk, 谓词)：等价于 while (!谓词()) wait(lk)——
//     循环检查条件，防止虚假唤醒（spurious wakeup）
//   - cv.wait_for(lk, 时长, 谓词)：带超时版本，返回 bool（谓词是否满足）
//   - enum class TaskOutcome：强类型枚举，避免结果值混用

#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace synapse::runtime {

// 任务最终状态（句柄的返回值）
enum class TaskOutcome {
    Pending,   // 还没结束（排队中或执行中）
    Done,      // 正常执行完毕
    Skipped,   // 协作式取消：执行前令牌已置位被跳过（环节 14）
    Failed,    // 任务体抛异常（运行时已捕获，线程存活）
    Rejected,  // 提交时队列已关闭/已满，任务从未入队
};

// 共享完成状态：句柄与执行器共用的"信箱"
class TaskCompletion {
public:
    // 置结果（幂等：只有第一次生效，避免执行器与拒绝路径重复写）
    void complete(TaskOutcome outcome) {
        {
            std::lock_guard<std::mutex> lock(m_);
            if (outcome_ != TaskOutcome::Pending) {
                return;                 // 已有结果：后到的写无效
            }
            outcome_ = outcome;
        }
        cv_.notify_all();               // 唤醒全部等待者
    }

    // 查询当前状态（不阻塞）
    [[nodiscard]] TaskOutcome state() const {
        std::lock_guard<std::mutex> lock(m_);
        return outcome_;
    }

    // 阻塞等待直到有结果（返回最终状态）
    [[nodiscard]] TaskOutcome wait() const {
        std::unique_lock<std::mutex> lock(m_);
        // 谓词循环：防虚假唤醒 + 防丢失唤醒
        cv_.wait(lock, [this] { return outcome_ != TaskOutcome::Pending; });
        return outcome_;
    }

    // 带超时等待：超时返回 false（状态仍可能是 Pending）
    [[nodiscard]] bool wait_for(std::chrono::milliseconds timeout) const {
        std::unique_lock<std::mutex> lock(m_);
        return cv_.wait_for(lock, timeout,
                            [this] { return outcome_ != TaskOutcome::Pending; });
    }

private:
    mutable std::mutex m_;         // 保护 outcome_（wait 是 const，故 mutable）
    // cv 的 wait 系列是非 const 成员函数：wait() 声明为 const 时
    // 必须让 cv 也可变，否则 const 方法里调 wait 会丢限定符（编译报错）
    mutable std::condition_variable cv_;   // 结果就绪通知
    TaskOutcome outcome_ = TaskOutcome::Pending;
};

using TaskCompletionPtr = std::shared_ptr<TaskCompletion>;

// 调用方持有的句柄：轻量包装（复制句柄共享同一完成状态）
class TaskHandle {
public:
    TaskHandle() = default;        // 默认构造 = 无效句柄（未绑定任务）

    explicit TaskHandle(TaskCompletionPtr completion)
        : completion_(std::move(completion)) {}

    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(completion_);
    }

    // 当前状态（无效句柄恒为 Pending）
    [[nodiscard]] TaskOutcome state() const {
        return completion_ ? completion_->state() : TaskOutcome::Pending;
    }

    // 阻塞等待（无效句柄直接返回 Pending，不卡死）
    [[nodiscard]] TaskOutcome wait() const {
        return completion_ ? completion_->wait() : TaskOutcome::Pending;
    }

    // 带超时等待（无效句柄返回 false）
    [[nodiscard]] bool wait_for(std::chrono::milliseconds timeout) const {
        return completion_ ? completion_->wait_for(timeout) : false;
    }

private:
    TaskCompletionPtr completion_; // 共享完成状态（可空）
};

}  // namespace synapse::runtime
