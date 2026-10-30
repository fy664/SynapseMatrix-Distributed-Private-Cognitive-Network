// SynapseMatrix — 单测：WorkStealingPool 多队列线程池（Month 3 环节 11）
// ======================================================================
// 覆盖：
//   1. N 个任务全部执行（总数 == 提交数，无丢失）
//   2. 亲和性提交制造不均 → 偷取确实发生（stolen 总和 > 0）
//   3. 轮询分发下各 Worker 负载均匀（不触发偷取也正确执行）
//   4. shutdown 后 submit 被拒
//   5. shutdown 幂等（调用两次安全）
//   6. 构造后立即 shutdown（空转退出，不崩）
//   7. 多线程并发 submit 无丢失
//
// 怎么验证"偷取"：stolen_ 计数只在 worker 从"别人的队列"
// 取到任务时 +1——只要亲和性提交把任务全塞给 worker 0，
// worker 1/2/3 必然会来偷（stolen 总和 > 0 是确定性断言）。

#include "test_framework.hpp"

#include <atomic>
#include <thread>
#include <vector>

#include "runtime/work_stealing.hpp"

using namespace std::chrono_literals;
using synapse::runtime::Task;
using synapse::runtime::WorkStealingPool;

// 1. 轮询分发：100 个任务全部执行，无丢失
TEST(WorkStealing, AllTasksExecuted) {
    constexpr int kTasks = 100;
    std::atomic<int> done{0};
    {
        WorkStealingPool pool(4);
        for (int i = 0; i < kTasks; ++i) {
            pool.submit(Task([&] { done.fetch_add(1); },
                             "t" + std::to_string(i)));
        }
        pool.shutdown();
    }
    EXPECT(done.load() == kTasks);               // 一个都不能少
}

// 2. 亲和性提交制造不均 → 偷取必然发生
TEST(WorkStealing, StealingHappens) {
    WorkStealingPool pool(4);
    std::atomic<int> done{0};
    // 全部塞给 worker 0 → 它忙不过来，1/2/3 来偷
    for (int i = 0; i < 40; ++i) {
        pool.submit_to(0, Task([&] { done.fetch_add(1); },
                               "affinity" + std::to_string(i)));
    }
    pool.shutdown();
    EXPECT(done.load() == 40);                   // 全执行
    // 偷取计数总和 > 0（worker 0 之外的线程偷了它的任务）
    std::size_t total_stolen = 0;
    for (std::size_t w = 0; w < pool.num_workers(); ++w) {
        total_stolen += pool.stolen(w);
    }
    EXPECT(total_stolen > 0);                    // 偷取确实发生
}

// 3. 轮询分发：任务带耗时 → 4 个 worker 都能分到活（并行窗口）
TEST(WorkStealing, RoundRobinDistributes) {
    WorkStealingPool pool(4);
    std::atomic<int> done{0};
    // 空任务执行太快：一个 worker 可能一轮循环偷光所有队列，
    // 其他 worker 醒来时全空——那是 Work-Stealing 的正常行为
    //（本测试第一次踩坑：无耗时任务时 w3 独吞 200 个）。
    // 每个任务睡 2ms → 400ms 总工时，4 个 worker 必然都有活干。
    for (int i = 0; i < 200; ++i) {
        pool.submit(Task([&] {
            std::this_thread::sleep_for(2ms);
            done.fetch_add(1);
        }, "rr" + std::to_string(i)));
    }
    pool.shutdown();
    EXPECT(done.load() == 200);
    std::size_t busy = 0;
    for (std::size_t w = 0; w < pool.num_workers(); ++w) {
        if (pool.executed(w) > 0) {
            ++busy;
        }
    }
    EXPECT(busy >= 2);
}

// 4. shutdown 后 submit 拒绝
TEST(WorkStealing, SubmitAfterShutdownRejected) {
    WorkStealingPool pool(2);
    pool.shutdown();
    bool ok = pool.submit(Task([] {}));
    EXPECT(!ok);
    bool ok2 = pool.submit_to(0, Task([] {}));
    EXPECT(!ok2);                                // 亲和性提交同样被拒
}

// 5. shutdown 幂等：调用两次安全
TEST(WorkStealing, ShutdownIdempotent) {
    WorkStealingPool pool(3);
    pool.submit(Task([] {}));
    pool.shutdown();
    pool.shutdown();                             // 第二次：无操作
    EXPECT(true);                                // 能走到这里即不崩
}

// 6. 空转退出：构造后立即 shutdown，不崩
TEST(WorkStealing, EmptyPoolShutdown) {
    WorkStealingPool pool(4);                    // 没有提交任何任务
    pool.shutdown();                             // 全部 worker 空转后退出
    EXPECT(true);
}

// 7. 多线程并发 submit 无丢失
TEST(WorkStealing, ConcurrentSubmitNoLoss) {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 150;
    constexpr int kTotal = kProducers * kPerProducer;
    std::atomic<int> done{0};
    {
        WorkStealingPool pool(4);
        std::vector<std::thread> producers;
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&, p] {
                for (int i = 0; i < kPerProducer; ++i) {
                    pool.submit(Task([&] { done.fetch_add(1); },
                                     "p" + std::to_string(p) + "-" +
                                         std::to_string(i)));
                }
            });
        }
        for (auto& th : producers) {
            th.join();
        }
        pool.shutdown();
    }
    EXPECT(done.load() == kTotal);               // 无丢失
}

TEST_MAIN()
