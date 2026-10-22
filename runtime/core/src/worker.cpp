// SynapseMatrix — Worker 实现（Month 3 环节 2）
// ==============================================
// Worker 是"一个消费线程"：从共享队列取任务、执行、上报统计。
//
// 生命周期（谁负责启动/回收）：
//   - 构造：std::thread 构造即启动，线程体 = run()
//   - 退出：run() 里 pop 返回 nullopt（队列 close 且清空）→ 线程自然结束
//   - 回收：join()（幂等）；析构也调 join()，防忘调（RAII）
//
// 语法讲解：
//   - std::thread([this] { run(); })：lambda 捕获 this 指针，
//     构造即在新线程执行 run()——线程入口必须是可调用对象
//   - joinable()：thread 是否持有可等待的线程（移动后/已 join 为 false）
//   - thread_ = ...：std::thread 是 move-only 类型（不可拷贝，可移动）
//   - [[maybe_unused]]：stop_ 当前未用，防止 -W4 未引用成员警告
//     （V2 急停功能再启用）

#include "runtime/worker.hpp"

#include <chrono>  // steady_clock：测任务执行耗时（单调时钟）

namespace synapse::runtime {

Worker::Worker(int worker_id, TaskQueue& queue, Telemetry& telemetry)
    : worker_id_(worker_id),
      queue_(queue),
      telemetry_(telemetry),
      stop_(false) {
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

void Worker::run() {
    // 主循环：阻塞取任务；队列 close 且清空后 pop 返回 nullopt → 退出
    // 这就是"优雅停止"——不打断正在执行的任务，干完手里的活再下班。
    while (auto task = queue_.pop()) {
        // 用单调时钟测执行耗时（steady_clock 不受系统调时间影响）
        const auto start = std::chrono::steady_clock::now();
        (*task)();                               // 执行任务体
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                       start);
        // 上报：该 worker 计数 +1、累计耗时（Telemetry 内部无锁原子）
        telemetry_.record_task_done(worker_id_, elapsed);
    }
    // stop_ 预留字段：V2 若要做"立即急停"，把 pop 改成
    // "带超时轮询 stop_ 的变体"（当前 pop 是阻塞式，无法中途打断）。
    (void)stop_.load();  // 抑制未使用警告（V1 保留字段）
}

}  // namespace synapse::runtime
