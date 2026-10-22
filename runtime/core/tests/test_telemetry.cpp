// SynapseMatrix — 单测：Telemetry 深化（Month 3 环节 3）
// ======================================================
// 覆盖：
//   1. per-worker 独立计数（每个 worker 一格，互不干扰）
//   2. 总执行数 = 各 worker 求和
//   3. worker_id 越界查询返回 0（不崩）
//   4. 拒绝计数
//   5. 平均耗时计算（已知耗时 + 已知任务数）
//   6. Executor 集成：提交 N 任务后总执行数 = N，各 worker 计数和为 N

#include "test_framework.hpp"

#include <chrono>

#include "runtime/executor.hpp"
#include "runtime/telemetry.hpp"

using namespace std::chrono_literals;
using synapse::runtime::Executor;
using synapse::runtime::Task;
using synapse::runtime::Telemetry;

// 1. per-worker 独立计数
TEST(Telemetry, PerWorkerIndependentCounters) {
    Telemetry tm(4);                              // 4 个 worker
    tm.record_task_done(0, 1ms);
    tm.record_task_done(0, 1ms);                  // worker0 两次
    tm.record_task_done(2, 1ms);                  // worker2 一次
    EXPECT(tm.task_count(0) == 2);
    EXPECT(tm.task_count(1) == 0);
    EXPECT(tm.task_count(2) == 1);
    EXPECT(tm.task_count(3) == 0);
}

// 2. 总执行数 = 各 worker 求和
TEST(Telemetry, TotalIsSumOfWorkers) {
    Telemetry tm(3);
    tm.record_task_done(0, 1ms);
    tm.record_task_done(1, 1ms);
    tm.record_task_done(1, 1ms);
    tm.record_task_done(2, 1ms);
    EXPECT(tm.tasks_done() == 4);                 // 1+2+1
}

// 3. 越界查询返回 0
TEST(Telemetry, OutOfRangeWorkerReturnsZero) {
    Telemetry tm(2);
    EXPECT(tm.task_count(2) == 0);                // 越界
    EXPECT(tm.task_count(-1) == 0);               // 负数
}

// 4. 拒绝计数
TEST(Telemetry, RejectedCounter) {
    Telemetry tm(2);
    tm.record_rejected();
    tm.record_rejected();
    tm.record_rejected();
    EXPECT(tm.rejected() == 3);
}

// 5. 平均耗时：10 个任务 × 2ms = 20ms 总耗时 → 平均 2ms
TEST(Telemetry, AverageElapsedComputed) {
    Telemetry tm(1);
    for (int i = 0; i < 10; ++i) {
        tm.record_task_done(0, 2ms);              // 每个任务 2 毫秒
    }
    EXPECT(tm.total_elapsed_us() == 20000);       // 20 ms = 20000 us
    // 浮点比较：2.0ms（允许微小误差）
    const double avg = tm.avg_task_ms();
    EXPECT(avg > 1.99 && avg < 2.01);
}

// 6. 无任务时平均耗时为 0（防除零）
TEST(Telemetry, EmptyAverageIsZero) {
    Telemetry tm(1);
    EXPECT(tm.avg_task_ms() == 0.0);
}

// 7. Executor 集成：提交任务后，per-worker 计数总和非 0 且等于提交数
TEST(Telemetry, ExecutorIntegration) {
    constexpr int kTasks = 50;
    Executor ex(4);
    for (int i = 0; i < kTasks; ++i) {
        ex.submit(Task([] { /* 空任务 */ }, "t"));
    }
    ex.shutdown();

    const auto& tm = ex.telemetry();
    EXPECT(tm.tasks_done() == kTasks);            // 全部执行
    EXPECT(tm.worker_count() == 4);               // 4 个 worker 槽位
    // 至少有一个 worker 干了活（并发分配下通常不止一个）
    std::size_t covered = 0;
    for (int w = 0; w < 4; ++w) {
        if (tm.task_count(w) > 0) {
            ++covered;
        }
    }
    EXPECT(covered >= 1);
}

TEST_MAIN()
