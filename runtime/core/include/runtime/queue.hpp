// SynapseMatrix — Queue：线程安全的任务队列（优先级调度 + 老化防饥饿）
// ==========================================================
// 规格书 Month 3 项目对象之一：Queue。
// 这是 Worker 与 Executor 之间的"生产者-消费者"缓冲区：
//   - 生产者（提交方）push 任务
//   - 消费者（Worker 线程）pop 任务
// 多线程同时 push/pop 时必须互斥，否则数据竞争（data race）是未定义行为。
//
// 优先级调度（环节 5，2026-10-23）：
//   - 高优先级任务先出队（Task::Priority，High > Normal > Low）
//   - 同优先级按提交顺序 FIFO（seq 序号稳定排序——容器本身不稳定，
//     必须显式加序号保证先来先出）
//   对应 408 操作系统"优先级调度算法（PS）"——就绪队列按优先级
//   排队，高优先级进程先上处理机。
//
// 老化防饥饿（环节 6，2026-10-25）：
//   纯优先级调度有个致命缺陷：如果 High 任务持续到达，Low 任务
//   可能永远得不到执行——"饥饿（starvation）"。
//   解法 = 老化（aging）：任务在队列里等待超过一个老化间隔
//   （aging_interval）后，其有效优先级提升一级（boost+1），
//   等待越久提升越多，保证低优先级任务最终能被调度。
//   对应 408 多级反馈队列（MLFQ）防止低优先级进程饿死的机制，
//   也对应 Linux CFS 的虚拟运行时间（vruntime 越小越先跑）。
//
// 为什么用 std::multiset（红黑树）而不是 std::priority_queue：
//   1. priority_queue 的比较器必须是"固定键"——堆建立后比较结果
//      不能再随时间变化，否则堆序被破坏（堆顶不再是最大）。
//      而老化提升依赖等待时长（时间在变）→ 无法安全做。
//   2. priority_queue 只能动堆顶，被压制的饥饿任务永远到不了堆顶
//      → 惰性提升只救堆顶，救不了深层任务（环节 6 第一版踩坑）。
//   3. multiset 允许遍历（begin→end）、任意删除、按新键重插，
//      pop 时全量扫描一次"超龄任务"，提升后重插，红黑树
//      插入/删除仍是 O(log n)；扫描本身 O(n)（任务量小可接受，
//      换来了"任意任务都能被提升"的正确性）。
//   这是"二叉堆 vs 红黑树"的经典权衡：堆更省内存更紧凑，
//   树支持更灵活的遍历与任意修改（408 数据结构两章都有）。
//
// 实现用到的并发原语（Month 3 必学 + 408 2.3 同步与互斥）：
//   - std::mutex              : 互斥锁，保证临界区（队列数据操作）独占
//   - std::condition_variable : 条件变量，让消费者"等待有货"，避免忙轮询
//   - std::unique_lock        : RAII 锁——析构自动解锁，异常安全
//   - std::multiset           : 红黑树（有序关联容器），begin 是最小元素
//   - std::chrono             : 时间测量；steady_clock 单调时钟
//
// 关闭语义（close）：
//   队列可被显式关闭；关闭后 push 拒绝、pop 在队列清空后返回 nullopt。
//   Executor 停机时先 close 再让 Worker 自然退出——优雅关闭。
//
// 语法讲解：
//   - std::optional<T>        : C++17 起，"可能有值也可能没有"的包装
//   - wait(pred)              : 条件变量等待谓词为真，等价
//                               while(!pred) wait(lock)——防止虚假唤醒
//   - multiset 的 Compare：strict weak ordering；begin() 返回"最小"
//     元素——所以想让"最优先"排最前，比较器必须把高有效优先级
//     定义为"更小"
//   - erase(it) 返回下一个迭代器（C++11+）：边遍历边删除的安全写法
//   - 迭代器失效规则：multiset 的 erase 只使被删迭代器失效，
//     其他迭代器不受影响（节点式容器 vs vector 的"整体搬家"）

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <utility>

#include "runtime/task.hpp"

namespace synapse::runtime {

class TaskQueue {
public:
    // 构造空队列。
    // aging_interval：老化间隔（毫秒）。0 = 禁用老化（纯优先级，
    //   环节 5 行为）。>0 时，任务在队列中等待超过一个间隔，
    //   有效优先级 +1（最多 aging_max_boost 级）。
    // aging_max_boost：老化提升上限，防止无限抬升（也防比较器
    //   溢出），通常设为"最高优先级与最低之差"即可。
    explicit TaskQueue(
        std::chrono::milliseconds aging_interval = std::chrono::milliseconds::zero(),
        int aging_max_boost = 2)
        : aging_interval_(aging_interval), aging_max_boost_(aging_max_boost) {}

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
            // 打上入队序号（同优先级 FIFO 的稳定键）与入队时间戳
            //（老化计时起点），插入红黑树（O(log n)）
            const std::uint64_t seq = seq_counter_++;
            queue_.insert(Entry{std::move(task), seq,
                                std::chrono::steady_clock::now(), 0});
        }
        cv_.notify_one();  // 通知一个等待中的消费者"有新任务"
        return true;
    }

    // ---- 消费者侧 ----
    // 取一个任务；队列关闭且清空时返回 nullopt（worker 应退出）。
    // 带老化：pop 前全量扫描一次，把等待超阈值的所有任务提升，
    // 保证被高优先级压制的低优先级任务最终能"翻身"。
    std::optional<Task> pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] {
            return !queue_.empty() || closed_;   // 有货或已关闭才醒来
        });
        if (queue_.empty()) {
            return std::nullopt;   // 关闭且空：告诉调用方"结束"
        }

        // 老化扫描（老化启用时）：遍历全部任务，超龄的提升、
        // 重置计时（提升后重新计时，防止同一任务连续霸占队首）
        if (aging_interval_.count() > 0) {
            const auto now = std::chrono::steady_clock::now();
            for (auto it = queue_.begin(); it != queue_.end();) {
                Entry e = *it;                      // 拷贝（节点式容器）
                const auto waited = now - e.enqueue_time;
                if (waited >= aging_interval_) {
                    it = queue_.erase(it);          // 删除并取下一个迭代器
                    // 每满一个间隔 +1 级；封顶 aging_max_boost_
                    //（防无限抬升，也防比较器溢出）
                    const auto steps = static_cast<int>(waited / aging_interval_);
                    e.boost = std::min(e.boost + steps, aging_max_boost_);
                    e.enqueue_time = now;           // 本轮提升后重新计时
                    queue_.insert(std::move(e));    // 以新键重插（O(log n)）
                } else {
                    ++it;
                }
            }
        }

        // 取最优先：begin() 是"有效优先级最高 + seq 最小"的任务
        Task t = queue_.begin()->task;
        queue_.erase(queue_.begin());
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
    // 队列元素：任务 + 调度元数据
    struct Entry {
        Task task;
        std::uint64_t seq;                      // 提交顺序号（FIFO 稳定键）
        std::chrono::steady_clock::time_point enqueue_time; // 入队时刻（老化起点）
        int boost;                              // 老化提升级数（0=原始优先级）
    };

    // 排序规则（strict weak ordering，multiset 的 begin = "最小"）：
    //   为了让"最优先"排在 begin，把高有效优先级定义为"更小"。
    //   1. 有效优先级（原始优先级 + 老化提升）高者排前
    //   2. 相同则 seq 小者排前（先来先出，稳定）
    struct Compare {
        bool operator()(const Entry& a, const Entry& b) const {
            const int ap = static_cast<int>(a.task.priority()) + a.boost;
            const int bp = static_cast<int>(b.task.priority()) + b.boost;
            if (ap != bp) {
                return ap > bp;   // 有效优先级大 = "小" = 排前面
            }
            return a.seq < b.seq; // seq 小 = "小" = 排前面
        }
    };

    mutable std::mutex mutex_;      // mutable：size() const 也要能加锁
    std::condition_variable cv_;    // 消费者等待"队列非空"
    std::multiset<Entry, Compare> queue_;       // 红黑树（按有效优先级排序）
    std::uint64_t seq_counter_ = 0; // 入队序号分配器（锁内递增）
    bool closed_ = false;           // 关闭标志
    std::chrono::milliseconds aging_interval_; // 老化间隔（0=禁用）
    int aging_max_boost_;           // 老化提升上限
};

}  // namespace synapse::runtime
