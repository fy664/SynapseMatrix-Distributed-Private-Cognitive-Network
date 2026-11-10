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
#include <stdexcept>
#include <thread>
#include <vector>

#include "runtime/executor.hpp"

using namespace std::chrono_literals;
using synapse::runtime::Executor;
using synapse::runtime::Priority;
using synapse::runtime::Task;
using synapse::runtime::TaskOutcome;

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

// 18. 停止令牌初始未置位；提交时自动挂到任务上（环节 14）
TEST(Executor, TokenNotRequestedInitially) {
    Executor ex(2);
    EXPECT(!ex.token()->is_stop_requested());      // 初始未请求停止
    ex.shutdown();
}

// 19. 请求停止后提交的任务在执行前被跳过（不清队列的惰性丢弃）
//     确定性做法：先 request_stop，再提交——任务取到时令牌必然已置位。
TEST(Executor, RequestStopSkipsPendingTasks) {
    std::atomic<int> ran{0};
    {
        Executor ex(1);
        ex.request_stop();                          // 先广播停止
        for (int i = 0; i < 20; ++i) {
            // request_stop 不等于关闭队列：submit 仍应成功（任务会被跳过）
            const bool ok = ex.submit(Task([&] { ran.fetch_add(1); },
                                            "skip" + std::to_string(i)));
            EXPECT(ok);
        }
        ex.shutdown();                              // 等 worker 消费完这些"空任务"
        EXPECT(ran.load() == 0);                    // 一个都没执行
        EXPECT(ex.telemetry().skipped() == 20);     // 全部计入跳过
        EXPECT(ex.telemetry().tasks_done() == 0);   // 执行数不被跳过污染
    }
}

// 20. 正在执行的任务自查令牌后提前返回（协作式取消的核心）
//     任务体每 1ms 检查一次 should_stop()，全跑完需 2s；
//     中途 request_stop 后应很快返回（远小于满载耗时）。
TEST(Executor, RunningTaskObservesStopAndExitsEarly) {
    Executor ex(2);
    const auto token = ex.token();                  // 共享令牌（拷贝 shared_ptr）
    std::atomic<bool> started{false};
    std::atomic<bool> cancelled{false};
    ex.submit(Task([&, token] {
        started.store(true);
        for (int i = 0; i < 2000; ++i) {             // 满载 2000ms
            if (token->is_stop_requested()) {       // 安全点自查
                cancelled.store(true);
                return;                             // 协作式退出（RAII 收尾）
            }
            std::this_thread::sleep_for(1ms);
        }
    }, "long"));
    while (!started.load()) {                       // 事件同步：确保已开始执行
        std::this_thread::yield();
    }
    const auto t0 = std::chrono::steady_clock::now();
    ex.request_stop();                              // 只广播，不等待
    ex.shutdown();                                  // 收尾（等 worker 退出）
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    EXPECT(cancelled.load());                       // 任务体看到了停止请求
    EXPECT(elapsed.count() < 500);                  // 远小于满载 2000ms
}

// 21. request_stop 幂等、不阻塞、不清队列
TEST(Executor, RequestStopIsIdempotentAndNonBlocking) {
    Executor ex(2);
    const auto t0 = std::chrono::steady_clock::now();
    ex.request_stop();
    ex.request_stop();                              // 重复调用无副作用
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    EXPECT(ex.token()->is_stop_requested());
    EXPECT(elapsed.count() < 100);                  // 立即返回（不 join）
    ex.shutdown();
}

// 22. 与环节 12 的集成：stop() 也广播令牌，自查的正在执行任务提前返回
TEST(Executor, StopSignalsRunningTaskToExitEarly) {
    Executor ex(1);
    const auto token = ex.token();
    std::atomic<bool> started{false};
    std::atomic<bool> cancelled{false};
    ex.submit(Task([&, token] {
        started.store(true);
        for (int i = 0; i < 1000; ++i) {             // 满载 1000ms
            if (token->is_stop_requested()) {
                cancelled.store(true);
                return;
            }
            std::this_thread::sleep_for(1ms);
        }
    }, "long"));
    while (!started.load()) {
        std::this_thread::yield();
    }
    const auto t0 = std::chrono::steady_clock::now();
    ex.stop();                                       // 急停：置令牌 + 丢队列 + join
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    EXPECT(cancelled.load());                        // 任务体提前退出
    EXPECT(elapsed.count() < 300);                   // 不再等满载 1000ms
}

// 23. 句柄：任务正常完成后 wait() 返回 Done（环节 15）
TEST(Executor, HandleDoneAfterExecution) {
    Executor ex(2);
    std::atomic<int> value{0};
    auto handle = ex.submit_handle(Task([&] { value.store(42); }, "h"));
    EXPECT(handle.valid());
    EXPECT(handle.wait() == TaskOutcome::Done);   // 阻塞等到完成
    EXPECT(value.load() == 42);
    ex.shutdown();
}

// 24. 句柄：被协作式取消跳过的任务 → Skipped（不会永久挂住等待者）
TEST(Executor, HandleSkippedAfterRequestStop) {
    Executor ex(1);
    ex.request_stop();                            // 先广播停止
    auto handle = ex.submit_handle(Task([] {}, "skip"));
    EXPECT(handle.wait() == TaskOutcome::Skipped);
    ex.shutdown();
}

// 25. 句柄：提交到已关闭的池 → 立刻就绪为 Rejected
TEST(Executor, HandleRejectedAfterShutdown) {
    Executor ex(1);
    ex.shutdown();
    auto handle = ex.submit_handle(Task([] {}, "rej"));
    EXPECT(handle.wait() == TaskOutcome::Rejected);
}

// 26. 句柄：wait_for 超时返回 false，任务完成后再次等待成功
TEST(Executor, HandleWaitForTimeout) {
    Executor ex(1);
    auto handle = ex.submit_handle(Task(
        [] { std::this_thread::sleep_for(100ms); }, "slow"));
    EXPECT(!handle.wait_for(30ms));               // 30ms 时还没完成
    EXPECT(handle.state() == TaskOutcome::Pending);
    EXPECT(handle.wait() == TaskOutcome::Done);   // 再等：完成
    EXPECT(handle.wait_for(30ms));                // 已就绪：立即返回 true
    ex.shutdown();
}

// 27. 异常安全：任务体抛异常被捕获 → Failed，线程存活，后续任务照常执行
TEST(Executor, ExceptionTaskMarkedFailedAndPoolSurvives) {
    Executor ex(1);
    auto failed_handle = ex.submit_handle(Task(
        [] { throw std::runtime_error("boom"); }, "boom"));
    EXPECT(failed_handle.wait() == TaskOutcome::Failed);
    EXPECT(ex.telemetry().failed() == 1);         // 失败单独计数
    EXPECT(ex.telemetry().tasks_done() == 0);     // 失败不算执行
    // 线程没有因异常退出：下一个任务仍能正常跑完
    std::atomic<bool> ran{false};
    auto ok_handle = ex.submit_handle(Task([&] { ran.store(true); }, "ok"));
    EXPECT(ok_handle.wait() == TaskOutcome::Done);
    EXPECT(ran.load());
    ex.shutdown();
}

TEST_MAIN()
