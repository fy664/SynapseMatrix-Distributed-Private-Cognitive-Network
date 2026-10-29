// SynapseMatrix — Worker 实现（Month 3 环节 2 / 环节 10）
// ==============================================
// Worker 是"一个消费线程"：从共享队列取任务、执行、上报统计。
//
// 生命周期（谁负责启动/回收）：
//   - 构造：std::thread 构造即启动，线程体 = run()
//   - 退出（正常）：run() 里 pop 返回 nullopt（队列 close 且清空）→ 线程结束
//   - 退出（缩容，环节 10）：空闲超时 + 线程有富余 → 自我退役
//   - 回收：join()（幂等）；析构也调 join()，防忘调（RAII）
//
// 动态缩容细节（环节 10）：
//   run() 在 idle_timeout > 0 时改用 pop_wait(idle_timeout)：
//     - 取到任务 → 执行（与原来完全一致）
//     - 返回 nullopt 且队列已关闭 → 下班（与 pop 的关闭语义一致）
//     - 返回 nullopt 且未关闭 → 说明"超时没货"= 线程空闲
//       此时若存活线程数 > 核心数 → CAS 抢退役名额；
//       否则（已到核心水位）继续等——核心线程永不退役。
//   存活计数语义：live_workers_ 恒等于"run() 还在跑的线程数"。
//     构造 +1；线程退出恰好 -1 一次（下班或退役都减）。
//     退役路径的 CAS 递减 = "抢名额 + 减计数"原子完成，
//     保证并发空闲时不会缩到核心数以下（读-校验-更新的标准无锁套路）。
//
// 408 映射：Worker = 进程/线程执行体；空闲退役 ≈ 操作系统的
//   资源回收（进程结束后回收 PCB/栈）；核心线程 ≈ 常驻资源池。
//
// 语法讲解：
//   - std::thread([this] { run(); })：lambda 捕获 this 指针，
//     构造即在新线程执行 run()——线程入口必须是可调用对象
//   - joinable()：thread 是否持有可等待的线程（移动后/已 join 为 false）
//   - thread_ = ...：std::thread 是 move-only 类型（不可拷贝，可移动）
//   - compare_exchange_weak(expected, desired)：无锁 CAS——
//     当前值 == expected 则置为 desired 返回 true；
//     否则把 expected 更新为当前值返回 false（循环重试）。
//     weak 允许偶发伪失败，适合循环重试（通常更高效）
//   - fetch_add/fetch_sub(1, relaxed)：原子加/减。
//     relaxed 就够——这里只关心计数本身，不依赖它同步其他数据

#include "runtime/worker.hpp"

#include <chrono>  // steady_clock：测任务执行耗时（单调时钟）

namespace synapse::runtime {

Worker::Worker(int worker_id,
               TaskQueue& queue,
               Telemetry& telemetry,
               std::atomic<int>& live_workers,
               std::size_t core_workers,
               std::chrono::milliseconds idle_timeout)
    : worker_id_(worker_id),
      queue_(queue),
      telemetry_(telemetry),
      live_workers_(live_workers),
      core_workers_(core_workers),
      idle_timeout_(idle_timeout) {
    live_workers_.fetch_add(1, std::memory_order_relaxed);  // 注册存活
    // 启动后台线程：lambda 捕获 this，调用成员函数 run()
    thread_ = std::thread([this] { run(); });
}

Worker::~Worker() {
    join();  // RAII：析构必 join，否则 thread 析构会 std::terminate
}

void Worker::join() {
    // 幂等 join：只有"持有可等待线程"时才 join
    // （已 join 过 / 线程被移动走 / 从未启动 → joinable() 为 false）
    if (thread_.joinable()) {
        thread_.join();
    }
}

// 尝试退役：原子地"抢一个退役名额"。
//   成功 → 存活计数已 -1，线程应退出；
//   失败 → 没抢到（有人先退了，或已到核心水位），线程继续服役。
// CAS 循环细节：
//   cur 先读当前存活数；只要 cur > 核心数 就尝试把 cur 换成 cur-1。
//   失败时 compare_exchange_weak 会把 cur 更新为最新值再重试，
//   所以循环天然处理并发竞争——最终要么抢到名额，要么发现
//   存活数已降到核心水位（cur <= core）而放弃。
bool Worker::try_retire() {
    int cur = live_workers_.load(std::memory_order_relaxed);
    while (cur > static_cast<int>(core_workers_)) {
        if (live_workers_.compare_exchange_weak(
                cur, cur - 1, std::memory_order_relaxed)) {
            return true;   // 抢到名额：计数已减，线程可以退役
        }
        // CAS 失败：cur 已被更新为最新存活数，继续判断
    }
    return false;          // 存活数已 <= 核心数：不退役
}

void Worker::run() {
    // 两种消费模式：
    //   1. idle_timeout == 0（默认）：老路径——阻塞式 pop，
    //      只有队列关闭才返回 nullopt（禁用缩容时的原始行为）
    //   2. idle_timeout  > 0：pop_wait 带超时轮询——空闲可退役
    while (true) {
        std::optional<Task> task =
            (idle_timeout_.count() > 0) ? queue_.pop_wait(idle_timeout_)
                                        : queue_.pop();
        if (!task) {
            // nullopt 的两种含义：
            //   a. 队列已关闭（且清空）→ 下班
            //   b. 超时空闲（仅缩容模式）→ 尝试退役
            if (queue_.is_closed()) {
                live_workers_.fetch_sub(1, std::memory_order_relaxed);
                break;                 // 下班：存活计数 -1（线程结束）
            }
            if (try_retire()) {
                break;                 // 退役：计数已在 CAS 里减过
            }
            continue;                  // 没抢到/到核心水位 → 继续服役
        }
        // 取到任务：执行并上报统计（与环节 2 完全一致）
        // 用单调时钟测执行耗时（steady_clock 不受系统调时间影响）
        const auto start = std::chrono::steady_clock::now();
        (*task)();                               // 执行任务体
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                       start);
        // 上报：该 worker 计数 +1、累计耗时（Telemetry 内部无锁原子）
        telemetry_.record_task_done(worker_id_, elapsed);
    }
    // 走到这里 = 线程结束（下班或退役）：标记退役，等 Executor 回收。
    // Executor 的 reap_retired() 看到 retired_ 后 join + 从数组移除。
    retired_.store(true, std::memory_order_relaxed);
}

}  // namespace synapse::runtime
