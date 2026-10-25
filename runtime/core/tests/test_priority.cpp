// SynapseMatrix — 单测：优先级调度 + 老化防饥饿（Month 3 环节 5/6）
// ==================================================
// 覆盖：
//   1. 高优先级任务先出队（混合优先级，pop 顺序 = 优先级降序）
//   2. 同优先级 FIFO（先提交的先出，稳定排序）
//   3. 优先级 + 同优先 FIFO 组合排序
//   4. close 后 pop 语义不变（先取完存量再 nullopt）
//   5. Executor 集成：高优先级任务先执行（执行顺序观察）
//   6~10. 老化防饥饿：禁用默认/等待提升/上限封顶/FIFO 保持/无饿死

#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "runtime/executor.hpp"
#include "runtime/queue.hpp"

using synapse::runtime::Executor;
using synapse::runtime::Priority;
using synapse::runtime::Task;
using synapse::runtime::TaskQueue;

// 1. 高优先级先出：压入 Low/Normal/High 混合，pop 按优先级降序
TEST(PriorityQueue, HighPriorityPopsFirst) {
    TaskQueue q;
    q.push(Task([] {}, "low", Priority::Low));
    q.push(Task([] {}, "normal", Priority::Normal));
    q.push(Task([] {}, "high", Priority::High));
    q.push(Task([] {}, "normal2", Priority::Normal));

    EXPECT(q.pop()->id() == "high");      // High 最先
    EXPECT(q.pop()->id() == "normal");    // 两个 Normal：先提交的先出
    EXPECT(q.pop()->id() == "normal2");
    EXPECT(q.pop()->id() == "low");       // Low 最后
}

// 2. 同优先级 FIFO：全部 Normal，严格按提交顺序
TEST(PriorityQueue, SamePriorityFifo) {
    TaskQueue q;
    for (int i = 0; i < 20; ++i) {
        q.push(Task([] {}, "t" + std::to_string(i), Priority::Normal));
    }
    for (int i = 0; i < 20; ++i) {
        EXPECT(q.pop()->id() == "t" + std::to_string(i));  // 顺序一致
    }
}

// 3. 组合：先压一堆 Low，再压一个 High——High 插队
TEST(PriorityQueue, HighPriorityPreemptsLine) {
    TaskQueue q;
    for (int i = 0; i < 5; ++i) {
        q.push(Task([] {}, "low" + std::to_string(i), Priority::Low));
    }
    q.push(Task([] {}, "urgent", Priority::High));   // 后到的插队
    EXPECT(q.pop()->id() == "urgent");               // 高优先越过队列
    EXPECT(q.pop()->id() == "low0");                 // 然后回到 FIFO
}

// 4. close 语义不变：关闭后仍能取出存量，清空后 nullopt
TEST(PriorityQueue, CloseDrainsThenNullopt) {
    TaskQueue q;
    q.push(Task([] {}, "a", Priority::High));
    q.push(Task([] {}, "b", Priority::Low));
    q.close();
    EXPECT(q.pop()->id() == "a");          // 高优先仍先出
    EXPECT(q.pop()->id() == "b");
    EXPECT(!q.pop().has_value());          // 清空 → 结束信号
}

// 5. Executor 集成：高优先级任务整体先执行
TEST(PriorityQueue, ExecutorRunsHighFirst) {
    // 记录执行顺序；每任务体在 vector 里追加自己的 id
    std::vector<std::string> order;
    Executor ex(1);                        // 单 worker：执行顺序 = 出队顺序
    // 先提交 6 个 Normal，再提交 2 个 High——High 应插队到最前
    for (int i = 0; i < 6; ++i) {
        ex.submit(Task([&order, i] { order.push_back("n" + std::to_string(i)); },
                       "n" + std::to_string(i), Priority::Normal));
    }
    for (int i = 0; i < 2; ++i) {
        ex.submit(Task([&order, i] { order.push_back("h" + std::to_string(i)); },
                       "h" + std::to_string(i), Priority::High));
    }
    ex.shutdown();

    EXPECT(order.size() == 8);
    EXPECT(order[0] == "h0");              // 两个 High 先执行
    EXPECT(order[1] == "h1");
    EXPECT(order[2] == "n0");              // 之后 Normal 按 FIFO
    EXPECT(order[7] == "n5");
}

// ---- 老化防饥饿（Month 3 环节 6）----

// 6. 默认（老化禁用）行为不变：纯优先级，Low 一直排在后面
TEST(PriorityQueue, AgingDisabledByDefault) {
    TaskQueue q;                           // 默认构造 = 老化关闭
    q.push(Task([] {}, "low", Priority::Low));
    q.push(Task([] {}, "high", Priority::High));
    EXPECT(q.pop()->id() == "high");       // 仍是最优先出
    EXPECT(q.pop()->id() == "low");
}

// 7. 老化生效：Low 等待超过老化间隔后被提升，能越过新的 High
TEST(PriorityQueue, AgingBoostsWaitingLow) {
    using namespace std::chrono_literals;
    TaskQueue q(5ms);                      // 老化间隔 5ms
    q.push(Task([] {}, "old-low", Priority::Low));
    std::this_thread::sleep_for(20ms);     // 让 old-low 等满 4 个间隔
    // 之后来的 High 优先级 2；old-low 原始 0 + boost≥2 → 并列或更高
    q.push(Task([] {}, "new-high", Priority::High));
    EXPECT(q.pop()->id() == "old-low");    // 老化让老 Low 抢到队首
    EXPECT(q.pop()->id() == "new-high");
}

// 8. 老化提升有上限：boost 封顶，低优先不会无限超越高优先
TEST(PriorityQueue, AgingBoostBounded) {
    using namespace std::chrono_literals;
    TaskQueue q(1ms, 1);                   // 老化间隔 1ms，提升上限 1 级
    q.push(Task([] {}, "a", Priority::Low));
    q.push(Task([] {}, "b", Priority::Low));
    std::this_thread::sleep_for(50ms);     // a/b 都等满 50 个间隔
    q.push(Task([] {}, "c", Priority::High)); // c 新到，无老化
    // 扫描后：a/b boost 封顶 1 → 有效 0+1=1；c 有效 2 → c 先出。
    // 若 boost 无上限，a/b 有效 ≥50 会永久压制 c（反向饿死 High）
    EXPECT(q.pop()->id() == "c");          // 上限保护了高优先任务
    EXPECT(q.pop()->id() == "a");          // 之后 a/b 按 FIFO
    EXPECT(q.pop()->id() == "b");
}

// 9. 老化不破坏同优先级 FIFO：同优先级任务老化后仍按 seq 出队
TEST(PriorityQueue, AgingKeepsSeqOrder) {
    using namespace std::chrono_literals;
    TaskQueue q(1ms, 2);
    q.push(Task([] {}, "a", Priority::Normal));
    q.push(Task([] {}, "b", Priority::Normal));
    std::this_thread::sleep_for(10ms);     // 两者同时老化（同 seq 序）
    q.push(Task([] {}, "c", Priority::Normal));
    EXPECT(q.pop()->id() == "a");          // 老化后仍 a→b→c
    EXPECT(q.pop()->id() == "b");
    EXPECT(q.pop()->id() == "c");
}

// 10. Executor 集成：开启老化的池子不会饿死 Low
TEST(PriorityQueue, ExecutorAgingNoStarvation) {
    using namespace std::chrono_literals;
    // 单 worker + 老化 5ms：Low 任务即使后到也会在等待后被调度
    Executor ex(1, 5ms, 2);
    ex.submit(Task([] { std::this_thread::sleep_for(15ms); }, "occupy",
                   Priority::Normal));    // 先占住 worker 15ms
    ex.submit(Task([] {}, "low", Priority::Low));
    ex.submit(Task([] {}, "high", Priority::High));
    // High 会先于 Low 执行（未老化时 Low 仍可用）；关键断言：
    // 全部任务完成，无饿死（shutdown 等全部结束，不会卡死）
    ex.shutdown();
    EXPECT(ex.telemetry().tasks_done() == 3);
}

TEST_MAIN()
