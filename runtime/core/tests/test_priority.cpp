// SynapseMatrix — 单测：优先级调度（Month 3 环节 5）
// ==================================================
// 覆盖：
//   1. 高优先级任务先出队（混合优先级，pop 顺序 = 优先级降序）
//   2. 同优先级 FIFO（先提交的先出，稳定排序）
//   3. 优先级 + 同优先 FIFO 组合排序
//   4. close 后 pop 语义不变（先取完存量再 nullopt）
//   5. Executor 集成：高优先级任务先执行（执行顺序观察）

#include "test_framework.hpp"

#include <atomic>
#include <string>
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

TEST_MAIN()
