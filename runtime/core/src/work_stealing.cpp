// SynapseMatrix — WorkStealingPool 实现（Month 3 环节 11）
// ==============================================
// 多队列 Work-Stealing 线程池：
//   每个 Worker 一个本地双端队列，Worker 先吃自己的；
//   自己的空了 → 按偏移顺序偷别人的队尾（最旧的任务）。
//
// 无死锁论证：
//   Worker 每次只持有一把锁——取自己队列时锁自己的，偷时锁
//   别人的，取完立即解锁，绝不嵌套持锁 → 没有锁等待环。
//
// 关闭语义：
//   closed_ 置位后，Worker 在"自己的队列 + 所有可偷的队列都空"
//   时退出。由于队列只会被消费、不会被回填（shutdown 后 submit
//   被拒），"全空 + closed" 就是安全退出点。
//
// 语法讲解：
//   - std::deque<T>：双端队列，支持前后两端 O(1) 插入删除——
//     队头 pop_front（自己取）、队尾 pop_back（别人偷）
//   - std::lock_guard：RAII 锁（构造加锁、析构解锁）
//   - std::move(task)：移动语义，避免拷贝 std::function
//   - vector<atomic>：构造定大小（atomic 不可移动，不能扩容）
//   - 范围 for (auto& t : threads_)：遍历 join

#include "runtime/work_stealing.hpp"

#include <optional>
#include <utility>

namespace synapse::runtime {

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
    // 轮询分发：把任务交给"下一个"本地队列（简单均匀）
    // 多线程 submit 时用锁保护 next_queue_（否则自增是数据竞争）
    std::size_t target;
    {
        std::lock_guard<std::mutex> lock(submit_mutex_);
        if (closed_.load(std::memory_order_relaxed)) {
            return false;      // 已关闭：拒绝
        }
        target = next_queue_;
        next_queue_ = (next_queue_ + 1) % queues_.size();
    }
    std::lock_guard<std::mutex> lock(queues_[target]->mutex);
    queues_[target]->tasks.push_back(std::move(task));
    return true;
}

bool WorkStealingPool::submit_to(std::size_t worker, Task task) {
    // 亲和性提交：直接放进指定 Worker 的本地队列
    if (worker >= queues_.size()) {
        return false;          // 越界
    }
    {
        std::lock_guard<std::mutex> lock(submit_mutex_);
        if (closed_.load(std::memory_order_relaxed)) {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(queues_[worker]->mutex);
    queues_[worker]->tasks.push_back(std::move(task));
    return true;
}

void WorkStealingPool::worker_loop(std::size_t id) {
    auto& mine = queues_[id];
    const std::size_t n = queues_.size();
    while (true) {
        // 第 1 步：先取自己的队头（最近提交的任务，缓存友好）
        // 用 optional 包任务：Task 没有默认构造函数（必须显式构造），
        // optional 允许"暂时没有值"——正好表达"没取到"。
        std::optional<Task> task;
        {
            std::lock_guard<std::mutex> lock(mine->mutex);
            if (!mine->tasks.empty()) {
                task = std::move(mine->tasks.front());
                mine->tasks.pop_front();
            }
        }
        if (task) {
            ++executed_[id];          // 自己队列的执行计数
            (*task)();                // 执行任务体
            continue;
        }

        // 第 2 步：自己的空了 → 按偏移顺序偷别人的队尾。
        // 从 (id+1) 开始轮流看，避免所有 Worker 一窝蜂抢同一队列。
        bool stole = false;
        for (std::size_t step = 1; step < n; ++step) {
            const std::size_t other = (id + step) % n;
            auto& q = queues_[other];
            std::lock_guard<std::mutex> lock(q->mutex);
            if (!q->tasks.empty()) {
                task = std::move(q->tasks.back());  // 偷队尾（最旧的）
                q->tasks.pop_back();
                stole = true;
                break;
            }
        }
        if (stole) {
            ++stolen_[id];            // 偷取计数（验证 Work-Stealing 发生）
            ++executed_[id];
            (*task)();
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
