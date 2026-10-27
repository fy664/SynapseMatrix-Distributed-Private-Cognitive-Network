// SynapseMatrix — 单测：线程安全 TaskQueue
// ==========================================
// 覆盖：
//   1. FIFO 顺序（先进先出）
//   2. 多线程并发 push/pop 无丢失（核心并发正确性验证）
//   3. close 后 push 被拒
//   4. close 且清空后 pop 返回 nullopt（Worker 退出条件）
//   5. 单线程 pop 阻塞等待（条件变量语义）
//
// 并发测试设计：
//   - 4 个生产者线程各 push 1000 个任务，1 个消费者 pop 4000 个
//   - 若队列线程不安全（数据竞争/丢任务），总数会小于 4000
//   - 用 join 保证所有线程结束再断言（否则竞态在读计数）

#include "test_framework.hpp"

#include <atomic>
#include <thread>
#include <vector>

#include "runtime/executor.hpp"
#include "runtime/queue.hpp"
#include "runtime/task.hpp"

using namespace std::chrono_literals;
using synapse::runtime::Executor;
using synapse::runtime::Priority;
using synapse::runtime::Task;
using synapse::runtime::TaskQueue;

// 1. FIFO：先入先出
TEST(Queue, FifoOrder) {
    TaskQueue q;
    for (int i = 0; i < 5; ++i) {
        q.push(Task([i] { /* 空体即可，只看顺序 */ },
                    "t" + std::to_string(i), Priority::Normal));
    }
    for (int i = 0; i < 5; ++i) {
        auto t = q.pop();
        EXPECT(t.has_value());
        EXPECT(t->id() == "t" + std::to_string(i));  // 按入队顺序出队
    }
    EXPECT(q.size() == 0);
}

// 2. 多线程并发：4 生产者 × 1000 + 1 消费者，全数收到
TEST(Queue, ConcurrentPushPopNoLoss) {
    TaskQueue q;
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 1000;
    constexpr int kTotal = kProducers * kPerProducer;

    std::atomic<int> received{0};

    // 消费者线程：pop 直到队列关闭且清空（返回 nullopt）
    std::thread consumer([&] {
        while (auto t = q.pop()) {
            (*t)();          // 执行任务体（内部 +1）——这是唯一计数口径
        }
    });

    // 4 个生产者线程：各 push 1000 个
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                q.push(Task([&] { received.fetch_add(1); },
                            "p" + std::to_string(p) + "-" + std::to_string(i)));
            }
        });
    }

    // 先 join 生产者（全部 push 完），再关闭队列让消费者退出
    for (auto& th : producers) {
        th.join();
    }
    q.close();
    consumer.join();

    // 每个任务体恰好执行一次 → received 应等于 kTotal。
    // 小于 kTotal 即说明并发下丢了任务（数据竞争/条件变量漏唤醒）。
    EXPECT(received.load() == kTotal);
}

// 3. close 后 push 被拒绝
TEST(Queue, PushAfterCloseRejected) {
    TaskQueue q;
    q.close();
    bool ok = q.push(Task([] {}, "late"));
    EXPECT(!ok);              // 关闭后入队失败
    EXPECT(q.size() == 0);    // 队列保持空
}

// 4. close 且清空后 pop 返回 nullopt
TEST(Queue, PopAfterDrainReturnsNullopt) {
    TaskQueue q;
    q.push(Task([] {}, "a"));
    q.close();
    auto t1 = q.pop();
    EXPECT(t1.has_value());   // 关闭但还有货：仍能取出
    auto t2 = q.pop();
    EXPECT(!t2.has_value());  // 关闭且空：结束信号
}

// 5. 条件变量阻塞：空队列 pop 会等，push 后立刻被唤醒
TEST(Queue, PopBlocksUntilPush) {
    TaskQueue q;
    std::atomic<bool> got{false};
    std::thread consumer([&] {
        auto t = q.pop();     // 此刻队列空：会阻塞等待
        got = t.has_value();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 确保消费者已进入等待
    q.push(Task([] {}, "wake"));
    consumer.join();
    EXPECT(got.load());       // 被唤醒且拿到任务
}

// ---- 背压：有界队列（Month 3 环节 7）----

// 6. 容量满时 push 被拒（拒绝式背压）
TEST(Queue, CapacityRejectsWhenFull) {
    TaskQueue q(0ms, 2, 2);            // 容量 2
    EXPECT(q.push(Task([] {}, "a")));  // 1/2
    EXPECT(q.push(Task([] {}, "b")));  // 2/2
    EXPECT(!q.push(Task([] {}, "c"))); // 满：拒绝
    EXPECT(q.size() == 2);             // 队列保持 2 个
}

// 7. 默认无容量限制（回归：老行为不变）
TEST(Queue, CapacityUnlimitedByDefault) {
    TaskQueue q;                       // 默认 max_capacity = 0
    for (int i = 0; i < 100; ++i) {
        EXPECT(q.push(Task([] {}, "t" + std::to_string(i))));
    }
    EXPECT(q.size() == 100);
}

// 8. pop 释放容量后可继续入队
TEST(Queue, CapacityFreesAfterPop) {
    TaskQueue q(0ms, 2, 2);            // 容量 2
    q.push(Task([] {}, "a"));
    q.push(Task([] {}, "b"));
    EXPECT(!q.push(Task([] {}, "c"))); // 满
    q.pop();                           // 腾出一个位置
    EXPECT(q.push(Task([] {}, "c")));  // 恢复可入队
    EXPECT(q.size() == 2);
}

// 9. Executor 集成：容量满时 submit 被拒并计入 rejected
TEST(Queue, ExecutorCapacityRejectsAndCounts) {
    using namespace std::chrono_literals;
    Executor ex(1, 0ms, 2, 3);         // 单 worker + 容量 3
    // 占住 worker（High 优先级，先被取走执行）
    ex.submit(Task([] { std::this_thread::sleep_for(30ms); }, "occupy",
                   Priority::High));
    ex.submit(Task([] {}, "a"));
    ex.submit(Task([] {}, "b"));
    std::this_thread::sleep_for(5ms);  // 确保 occupy 已被 worker 取出执行
    EXPECT(ex.submit(Task([] {}, "c")));   // 队列 [a,b] → c 入队 → 3 满
    EXPECT(!ex.submit(Task([] {}, "d")));  // 满：背压拒绝
    EXPECT(ex.telemetry().rejected() == 1);      // 恰 1 个被拒
    ex.shutdown();
    EXPECT(ex.telemetry().tasks_done() == 4);    // occupy+a+b+c 全执行
}

TEST_MAIN()
