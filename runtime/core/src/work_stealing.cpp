// SynapseMatrix — WorkStealingPool 实现（Month 3 环节 11 / 13）
// ==============================================================
// 多队列 Work-Stealing 线程池：
//   每个 Worker 一个本地队列（无锁环形队列），Worker 先吃自己的；
//   自己的空了 → 按偏移顺序偷别人的。
//
// 环节 13（2026-11-08）：本地队列无锁化。
//   旧实现：每队列 mutex + std::deque——抢锁会阻塞/唤醒线程，
//   高竞争下锁开销超过任务本身。
//   新实现：Vyukov MPMC 无锁环形队列（每槽 seq 版本号），
//   push/pop 都是"CAS 抢位置 + release/acquire 发布"，
//   不存在阻塞，不存在死锁。
//
// Vyukov 无锁队列算法要点（详见 LocalQueue::push/pop 注释）：
//   每个槽位有版本号 seq，通过 seq 与位置的差值判断三种状态：
//     空闲（可写）/ 已填充（可读）/ 已被消费（等待下一轮）。
//   这样即使生产者/消费者以不同速度前进，也不会读到写了一半的槽。
//
// 关闭语义：
//   closed_ 置位后，Worker 在"自己的队列 + 所有可偷的队列都空"
//   时退出。由于队列只会被消费、不会被回填（shutdown 后 submit
//   被拒），"全空 + closed" 就是安全退出点。
//
// 语法讲解：
//   - std::optional<T>     : 可能无值的包装（任务取没取到）
//   - std::ptrdiff_t       : 有符号指针差类型（判断 seq 与 pos 的
//                             相对大小——无符号相减会回绕，必须转有符号）
//   - compare_exchange_weak: CAS；weak 允许伪失败，循环重试即可
//   - memory_order_release : 之前的所有写（数据）先完成，再发布 seq
//   - memory_order_acquire : 看到 seq 之后，保证能看到之前发布的数据
//   - std::move(task)      : 移动语义，避免拷贝 std::function
//   - vector<atomic>       : 构造定大小（atomic 不可移动，不能扩容）

#include "runtime/work_stealing.hpp"

#include <optional>
#include <utility>

namespace synapse::runtime {

// ================================================================
// LocalQueue：Vyukov MPMC 无锁有界环形队列
// ================================================================

WorkStealingPool::LocalQueue::LocalQueue() : cells_(kCapacity) {
    // 初始化每槽版本号 = 下标。约定：
    //   seq == pos           → 槽位空（可写）
    //   seq == pos + 1       → 有数据（可读）
    //   seq == pos + capacity→ 已消费，等下一轮（可写）
    for (std::size_t i = 0; i < kCapacity; ++i) {
        cells_[i].seq.store(i, std::memory_order_relaxed);
    }
}

// push：生产者侧。先抢 enqueue_pos 拿到槽位号，再写槽位数据，
// 最后把 seq 推进到 pos+1 发布"数据已写好"。
bool WorkStealingPool::LocalQueue::push(Task task) {
    std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
    for (;;) {
        Cell& cell = cells_[pos & (kCapacity - 1)];   // & (cap-1) = 取模
        const std::size_t seq = cell.seq.load(std::memory_order_acquire);
        // 差值转有符号：seq 与 pos 的无符号相减在"seq 落后一轮"时
        // 会回绕成巨大正数，必须用 ptrdiff_t 判断真实先后关系。
        const std::ptrdiff_t dif =
            static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
        if (dif == 0) {
            // 槽位空闲：CAS 抢位置（抢占成功者才能写该槽）
            if (enqueue_pos_.compare_exchange_weak(
                    pos, pos + 1, std::memory_order_relaxed)) {
                cell.task.emplace(std::move(task));  // 写入任务（移动）
                // release：保证 task 的写入对消费者可见后，再发布 seq
                cell.seq.store(pos + 1, std::memory_order_release);
                return true;
            }
            // CAS 失败：pos 已被更新为最新值（别人抢到了）→ 重试
        } else if (dif < 0) {
            return false;          // 队列满（该槽还没轮到本轮）
        } else {
            // 别的生产者已推进，重读最新位置再看
            pos = enqueue_pos_.load(std::memory_order_relaxed);
        }
    }
}

// pop：消费者侧。先抢 dequeue_pos 拿到槽位号，再取走数据，
// 最后把 seq 推进到 pos+capacity 标记"本槽已空、下一轮可用"。
std::optional<Task> WorkStealingPool::LocalQueue::pop() {
    std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
    for (;;) {
        Cell& cell = cells_[pos & (kCapacity - 1)];
        const std::size_t seq = cell.seq.load(std::memory_order_acquire);
        // 有数据时 seq == pos + 1，所以差值对照 pos+1
        const std::ptrdiff_t dif =
            static_cast<std::ptrdiff_t>(seq) -
            static_cast<std::ptrdiff_t>(pos + 1);
        if (dif == 0) {
            // 槽位有数据：CAS 抢位置（多个消费者竞争同一个槽，只有一个赢）
            if (dequeue_pos_.compare_exchange_weak(
                    pos, pos + 1, std::memory_order_relaxed)) {
                std::optional<Task> t = std::move(cell.task);  // 取走任务
                cell.task.reset();          // 释放 moved-from 值，槽位可复用
                // release：任务取走后再发布"本槽已空、下一轮可写"
                cell.seq.store(pos + kCapacity, std::memory_order_release);
                return t;
            }
        } else if (dif < 0) {
            return std::nullopt;       // 队列空（该槽还没被生产者填上）
        } else {
            pos = dequeue_pos_.load(std::memory_order_relaxed);
        }
    }
}

// ================================================================
// WorkStealingPool
// ================================================================

WorkStealingPool::WorkStealingPool(std::size_t num_workers)
    : queues_(num_workers),   // 预构造 N 个本地队列（unique_ptr 数组）
      executed_(num_workers), // per-worker 执行计数（定大小，原子）
      stolen_(num_workers) {  // per-worker 偷取计数
    // 顺序很关键：先把全部本地队列建好，再启动线程。
    // 否则线程 i 启动后立刻去偷别的队列，可能撞上还没赋值的
    // 空 unique_ptr → 解引用空指针崩溃（环节 11 踩坑）。
    for (std::size_t i = 0; i < num_workers; ++i) {
        queues_[i] = std::make_unique<LocalQueue>();
    }
    threads_.reserve(num_workers);
    for (std::size_t i = 0; i < num_workers; ++i) {
        // Worker 构造即启动线程，线程体 = worker_loop(i)
        threads_.emplace_back([this, i] { worker_loop(i); });
    }
}

WorkStealingPool::~WorkStealingPool() {
    shutdown();  // RAII：析构自动回收全部线程
}

bool WorkStealingPool::submit(Task task) {
    // 轮询分发：原子 fetch_add 取号，无锁（环节 13：提交路径去锁）
    if (closed_.load(std::memory_order_relaxed)) {
        return false;          // 已关闭：拒绝
    }
    const std::size_t target =
        next_queue_.fetch_add(1, std::memory_order_relaxed) % queues_.size();
    return queues_[target]->push(std::move(task));   // 队列满也返回 false
}

bool WorkStealingPool::submit_to(std::size_t worker, Task task) {
    // 亲和性提交：直接放进指定 Worker 的本地队列（无锁 push）
    if (worker >= queues_.size()) {
        return false;          // 越界
    }
    if (closed_.load(std::memory_order_relaxed)) {
        return false;          // 已关闭
    }
    return queues_[worker]->push(std::move(task));
}

void WorkStealingPool::worker_loop(std::size_t id) {
    auto& mine = queues_[id];
    const std::size_t n = queues_.size();
    while (true) {
        // 第 1 步：先取自己的队头（FIFO，最旧任务；无锁 pop）
        std::optional<Task> task = mine->pop();
        bool stole = false;
        if (!task) {
            // 第 2 步：自己的空了 → 按偏移顺序偷别人的队头。
            // 从 (id+1) 开始轮流看，避免所有 Worker 一窝蜂抢同一队列。
            for (std::size_t step = 1; step < n; ++step) {
                const std::size_t other = (id + step) % n;
                task = queues_[other]->pop();   // 无锁：抢到就算偷到
                if (task) {
                    stole = true;
                    break;
                }
            }
        }
        if (task) {
            if (stole) {
                ++stolen_[id];        // 偷取计数（验证 Work-Stealing 发生）
            }
            ++executed_[id];
            (*task)();                // 执行任务体
            continue;
        }

        // 第 3 步：全局都空。检查是否已关闭：
        //   已关闭 → 不会再有人提交 → 安全退出；
        //   未关闭 → 睡 1ms 再战（避免忙轮询烧 CPU）。
        if (closed_.load(std::memory_order_relaxed)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void WorkStealingPool::shutdown() {
    closed_.store(true, std::memory_order_relaxed);
    // Worker 至多再睡 1ms 就会醒来看到 closed_ 并退出
    for (auto& t : threads_) {
        if (t.joinable()) {
            t.join();   // 等全部 Worker 退出（幂等：已 join 的跳过）
        }
    }
}

std::size_t WorkStealingPool::total_executed() const noexcept {
    std::size_t total = 0;
    for (std::size_t i = 0; i < executed_.size(); ++i) {
        total += executed(i);
    }
    return total;
}

}  // namespace synapse::runtime
