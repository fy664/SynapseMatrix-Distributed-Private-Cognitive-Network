// SynapseMatrix — 单测：Executor 多线程 Task Runtime（Month 3 环节 2）
// ======================================================================
// 覆盖：
//   1. 提交 N 个任务，全部被执行（telemetry 计数 = N）
//   2. shutdown 后 submit 被拒绝（返回 false）
//   3. 多线程并发 submit（生产者多个线程），全部执行无丢失
//   4. 优雅关闭：shutdown 返回后，正在执行的任务全部完成
//   5. 析构安全：不显式 shutdown，直接出作用域（RAII）不崩溃
//
// 并发正确性怎么验证：
//   每个任务体执行一个 atomic 计数 +1；全部结束后断言计数 == N。
//   若 Worker 循环/队列有 bug（丢任务、漏唤醒），计数 < N 立即暴露。

#include "test_framework.hpp"

#include <atomic>
#include <thread>
#include <vector>

#include "runtime/executor.hpp"

using synapse::runtime::Executor;
using synapse::runtime::Task;

// 1. 提交 N 个任务全部执行
TEST(Executor, AllTasksExecuted) {
    constexpr int kTasks = 100;
    std::atomic<int> done{0};
    {
        Executor ex(4);                          // 4 个 worker
        for (int i = 0; i < kTasks; ++i) {
            bool ok = ex.submit(Task([&] { done.fetch_add(1); },
                                     "t" + std::to_string(i)));
            EXPECT(ok);                          // shutdown 前应全部接受
        }
        ex.shutdown();                           // 等全部执行完
    }
    EXPECT(done.load() == kTasks);               // 一个都不能少
    EXPECT(done.load() > 0);
}

// 2. shutdown 后 submit 拒绝
TEST(Executor, SubmitAfterShutdownRejected) {
    Executor ex(2);
    ex.shutdown();
    bool ok = ex.submit(Task([] {}));
    EXPECT(!ok);                                 // 队列已关闭
}

// 3. 多线程并发 submit 无丢失
TEST(Executor, ConcurrentSubmitNoLoss) {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 200;
    constexpr int kTotal = kProducers * kPerProducer;
    std::atomic<int> done{0};
    {
        Executor ex(8);
        std::vector<std::thread> producers;
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&, p] {
                for (int i = 0; i < kPerProducer; ++i) {
                    ex.submit(Task([&] { done.fetch_add(1); },
                                   "p" + std::to_string(p) + "-" +
                                       std::to_string(i)));
                }
            });
        }
        for (auto& th : producers) {
            th.join();                           // 全部投递完
        }
        ex.shutdown();
    }
    EXPECT(done.load() == kTotal);               // 无丢失
}

// 4. 优雅关闭：shutdown 不打断正在执行的任务
TEST(Executor, ShutdownDrainsInFlightTasks) {
    std::atomic<int> done{0};
    {
        Executor ex(2);
        // 100 个任务，每个 sleep 1ms——保证 shutdown 时队列里还有积压
        for (int i = 0; i < 100; ++i) {
            ex.submit(Task([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                done.fetch_add(1);
            }, "sleep-task"));
        }
        ex.shutdown();                           // 阻塞直到全部执行完
    }
    EXPECT(done.load() == 100);                  // 已入队的必须全部完成
}

// 5. 析构安全：不显式 shutdown，出作用域自动回收（RAII）
TEST(Executor, DestructorShutsDown) {
    std::atomic<int> done{0};
    {
        Executor ex(3);                          // 不调 shutdown
        for (int i = 0; i < 10; ++i) {
            ex.submit(Task([&] { done.fetch_add(1); }, "auto"));
        }
    }                                            // 析构隐式 shutdown
    EXPECT(done.load() == 10);                   // 析构前任务全部完成
}

TEST_MAIN()
