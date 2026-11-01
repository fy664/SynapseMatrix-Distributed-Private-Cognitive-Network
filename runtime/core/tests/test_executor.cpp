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

using namespace std::chrono_literals;
using synapse::runtime::Executor;
using synapse::runtime::Priority;
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

// ---- 动态扩容（Month 3 环节 9）----

// 6. 积压超阈值自动扩容：worker 数增长且任务全部执行
TEST(Executor, GrowAddsWorkerOnHighBacklog) {
    // 初始 1 worker，上限 3；积压 >= 4 时自动加 worker
    Executor ex(1, 0ms, 2, 0, 3, 4);
    EXPECT(ex.active_workers() == 1);            // 初始 1 个
    // 先占住唯一的 worker，制造积压
    ex.submit(Task([] { std::this_thread::sleep_for(50ms); }, "occupy"));
    std::atomic<int> done{0};
    for (int i = 0; i < 20; ++i) {
        ex.submit(Task([&] { done.fetch_add(1); },
                       "t" + std::to_string(i)));
    }
    // 提交 20 个后，积压超阈值 → 应已扩容（>1）
    EXPECT(ex.active_workers() > 1);
    ex.shutdown();
    EXPECT(done.load() == 20);                   // 20 个计数任务全部执行
}

// 7. 积压未超阈值不扩容（保持初始数）
TEST(Executor, NoGrowWhenBacklogLow) {
    Executor ex(2, 0ms, 2, 0, 4, 100);           // 阈值 100（很高）
    for (int i = 0; i < 5; ++i) {
        ex.submit(Task([] {}, "t" + std::to_string(i)));
    }
    EXPECT(ex.active_workers() == 2);            // shutdown 前未扩容
    ex.shutdown();
}

// 8. 默认关闭自动扩容（回归：老行为不变）
TEST(Executor, NoGrowByDefault) {
    Executor ex(2);                              // 不传扩容参数
    for (int i = 0; i < 50; ++i) {
        ex.submit(Task([] { std::this_thread::sleep_for(1ms); },
                       "t" + std::to_string(i)));
    }
    EXPECT(ex.active_workers() == 2);            // shutdown 前 worker 数不变
    ex.shutdown();
}

// 9. 扩容到上限后停止：积压再高也不会超过 max_workers
TEST(Executor, GrowStopsAtMax) {
    Executor ex(1, 0ms, 2, 0, 3, 2);             // 上限 3，阈值 2
    ex.submit(Task([] { std::this_thread::sleep_for(50ms); }, "occupy"));
    for (int i = 0; i < 200; ++i) {
        ex.submit(Task([] {}, "t" + std::to_string(i)));
    }
    // 快速提交 200 个，扩容最多到 3
    EXPECT(ex.active_workers() <= 3);
    ex.shutdown();
    // 新语义：shutdown 后所有 worker 退出，存活计数归 0
    EXPECT(ex.active_workers() == 0);
}

// ---- 动态缩容（Month 3 环节 10）----

// 10. 空闲超时自动退役：忙时扩到上限，闲时缩回核心数
TEST(Executor, ShrinkOnIdleTimeout) {
    // 核心 1，上限 3，扩容阈值 2，空闲 100ms 退役
    Executor ex(1, 0ms, 2, 0, 3, 2, 100ms);
    // 占住唯一的 worker → 制造积压 → 触发扩容到 3
    ex.submit(Task([] { std::this_thread::sleep_for(50ms); }, "occupy"));
    for (int i = 0; i < 20; ++i) {
        ex.submit(Task([] {}, "t" + std::to_string(i)));
    }
    EXPECT(ex.active_workers() == 3);            // 高峰 3 个
    // 全部任务跑完，worker 空闲 100ms 后逐个退役
    std::this_thread::sleep_for(300ms);
    EXPECT(ex.active_workers() == 1);            // 缩回核心 1 个
    ex.shutdown();
}

// 11. 不会缩到核心数以下（CAS 抢名额保证）
TEST(Executor, NeverShrinkBelowCore) {
    // 核心 2（默认上限 2），空闲 50ms
    Executor ex(2, 0ms, 2, 0, 0, 0, 50ms);
    std::this_thread::sleep_for(200ms);          // 远超空闲超时
    EXPECT(ex.active_workers() == 2);            // 核心线程常驻
    ex.shutdown();
}

// 12. 默认（idle_timeout=0）空闲不退役：回归老行为
TEST(Executor, NoShrinkByDefault) {
    // 开启扩容（扩到 4）但不开启缩容
    Executor ex(1, 0ms, 2, 0, 4, 3, 0ms);
    ex.submit(Task([] { std::this_thread::sleep_for(50ms); }, "occupy"));
    for (int i = 0; i < 20; ++i) {
        ex.submit(Task([] {}, "t" + std::to_string(i)));
    }
    EXPECT(ex.active_workers() == 4);            // 已扩容到上限
    std::this_thread::sleep_for(200ms);          // 长时间空闲
    EXPECT(ex.active_workers() == 4);            // 不退役
    ex.shutdown();
}

// 13. 缩容后再扩容：空闲回收，忙时再拉起（弹性闭环）
TEST(Executor, ShrinkThenGrowAgain) {
    Executor ex(1, 0ms, 2, 0, 3, 2, 80ms);
    // 第一波高峰：扩到 3
    ex.submit(Task([] { std::this_thread::sleep_for(50ms); }, "occupy1"));
    for (int i = 0; i < 20; ++i) {
        ex.submit(Task([] {}, "a" + std::to_string(i)));
    }
    EXPECT(ex.active_workers() == 3);
    // 空闲后缩回 1
    std::this_thread::sleep_for(250ms);
    EXPECT(ex.active_workers() == 1);
    // 第二波高峰：再次扩容
    ex.submit(Task([] { std::this_thread::sleep_for(50ms); }, "occupy2"));
    for (int i = 0; i < 20; ++i) {
        ex.submit(Task([] {}, "b" + std::to_string(i)));
    }
    EXPECT(ex.active_workers() == 3);            // 又扩回来了
    ex.shutdown();
}

// ---- 急停（Month 3 环节 12）----

// 14. 急停丢弃排队任务：正在执行的任务跑完，排队的不执行
TEST(Executor, StopDiscardsPendingTasks) {
    Executor ex(1);                               // 单 worker：谁在跑谁占住
    std::atomic<int> done{0};
    std::atomic<bool> slow_started{false};
    // 慢任务占住唯一的 worker（200ms），开始时置位启动事件
    ex.submit(Task([&] {
        slow_started.store(true);
        std::this_thread::sleep_for(200ms);
        done.fetch_add(1);
    }, "slow"));
    // 50 个排队任务（本应被执行；stop 后全部丢弃）
    for (int i = 0; i < 50; ++i) {
        ex.submit(Task([&] { done.fetch_add(1); },
                       "queued" + std::to_string(i)));
    }
    // 等慢任务确实开始执行（此时 worker 被占住，50 个仍在队列）
    while (!slow_started.load()) {
        std::this_thread::yield();
    }
    // 慢任务还在跑、50 个还在排队时急停
    ex.stop();
    // stop 返回后：慢任务跑完（不可抢占），排队任务一个没执行
    EXPECT(done.load() == 1);
    // 急停后所有 worker 已退出（存活计数归 0，同 shutdown）
    EXPECT(ex.active_workers() == 0);
}

// 15. 急停与优雅停机幂等互斥：先 stop 后 shutdown / 先 shutdown 后 stop
TEST(Executor, StopAndShutdownIdempotent) {
    // 场景 A：先 stop 再 shutdown
    {
        Executor ex(2);
        ex.submit(Task([] {}));
        ex.stop();
        ex.shutdown();                            // 第二次：无操作
        EXPECT(ex.active_workers() == 0);
    }
    // 场景 B：先 shutdown 再 stop
    {
        Executor ex(2);
        ex.submit(Task([] {}));
        ex.shutdown();
        ex.stop();                                // 第二次：无操作
        EXPECT(ex.active_workers() == 0);
    }
}

// 16. 急停后 submit 拒绝
TEST(Executor, SubmitAfterStopRejected) {
    Executor ex(2);
    ex.stop();
    bool ok = ex.submit(Task([] {}));
    EXPECT(!ok);
    EXPECT(ex.telemetry().rejected() >= 1);        // 拒绝被记录（可观测性）
}

// 17. 急停不打断正在执行的任务（线程不可抢占的物理下限）
TEST(Executor, StopAllowsRunningTaskToFinish) {
    Executor ex(1);                               // 单 worker：唯一在跑的
    std::atomic<int> finished{0};
    std::atomic<bool> started{false};
    ex.submit(Task([&] {
        started.store(true);
        std::this_thread::sleep_for(100ms);       // 慢任务
        finished.fetch_add(1);
    }, "running"));
    // 等任务确实开始执行（否则 stop 可能把它也当作排队任务丢弃）
    while (!started.load()) {
        std::this_thread::yield();
    }
    // 正在执行时立即急停——不能打断它，必须等它跑完
    ex.stop();
    EXPECT(finished.load() == 1);                  // 跑完了
}

TEST_MAIN()
