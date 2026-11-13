// SynapseMatrix — TaskGraph（DAG 依赖调度）单测（Month 3 环节 16）
// ==================================================================
// 覆盖点：
//   1. 线性链 A→B→C 严格按拓扑序执行
//   2. 菱形 A→(B,C)→D：A 必最先、D 必最后，B/C 并行不分先后
//   3. 节点抛异常 → 后续依赖递归 Skipped，图正常收尾不卡死
//   4. 启动前 request_stop：全图 Skipped，句柄立刻可读
//   5. 一父多子扇出：所有子都能在根完成后执行
//   6. 非法依赖（越界 / 重复 / start 后添加）抛异常
//
// 语法讲解：
//   - std::atomic<size_t> order_idx：并发下给"执行顺序"编号（fetch_add
//     返回旧值 = 该任务排第几），避免手工加锁
//   - 用 std::vector<std::atomic<int>>? 不行：atomic 不可移动/复制，
//     容器元素需可构造——这里改用定长裸数组 + atomic<int>
//     （或用 vector<unique_ptr<atomic<int>>>）；本文件用 std::array
//   - EXPECT 内联比较：失败打印文件:行，不中断用例

#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

#include "runtime/executor.hpp"
#include "runtime/task.hpp"
#include "runtime/task_graph.hpp"
#include "runtime/task_handle.hpp"
#include "test_framework.hpp"

using synapse::runtime::Executor;
using namespace std::chrono_literals;   // 2000ms → std::chrono::milliseconds
using synapse::runtime::Task;
using synapse::runtime::TaskGraph;
using synapse::runtime::TaskHandle;
using synapse::runtime::TaskOutcome;

TEST(TaskGraph, LinearChainRunsInOrder) {
    Executor executor(2);
    TaskGraph graph(executor);

    std::array<std::atomic<int>, 3> seq{};
    for (auto& s : seq) {
        s.store(-1);
    }
    std::atomic<int> counter{0};

    // C 依赖 B，B 依赖 A
    auto id_a = graph.add(Task([&seq, &counter] {
        seq[0].store(counter.fetch_add(1));
    }, "A"));
    auto id_b = graph.add(Task([&seq, &counter] {
        seq[1].store(counter.fetch_add(1));
    }, "B"), {id_a});
    auto id_c = graph.add(Task([&seq, &counter] {
        seq[2].store(counter.fetch_add(1));
    }, "C"), {id_b});

    graph.start();

    EXPECT(graph.handle(id_c).wait_for(2000ms) == true);
    EXPECT(graph.handle(id_c).state() == TaskOutcome::Done);
    EXPECT(graph.handle(id_a).state() == TaskOutcome::Done);
    EXPECT(graph.handle(id_b).state() == TaskOutcome::Done);
    // 严格拓扑序：A=0 → B=1 → C=2
    EXPECT(seq[0].load() == 0);
    EXPECT(seq[1].load() == 1);
    EXPECT(seq[2].load() == 2);

    executor.shutdown();
}

TEST(TaskGraph, DiamondHonorsDependencies) {
    Executor executor(4);
    TaskGraph graph(executor);

    std::array<std::atomic<int>, 4> seq{};   // A B C D
    for (auto& s : seq) {
        s.store(-1);
    }
    std::atomic<int> counter{0};
    std::atomic<int> sum{0};

    auto id_a = graph.add(Task([&seq, &counter, &sum] {
        seq[0].store(counter.fetch_add(1));
        sum.fetch_add(10);
    }, "A"));
    auto id_b = graph.add(Task([&seq, &counter, &sum] {
        seq[1].store(counter.fetch_add(1));
        sum.fetch_add(20);
    }, "B"), {id_a});
    auto id_c = graph.add(Task([&seq, &counter, &sum] {
        seq[2].store(counter.fetch_add(1));
        sum.fetch_add(30);
    }, "C"), {id_a});
    // D 必须等 B 和 C 都完成
    auto id_d = graph.add(Task([&seq, &counter] {
        seq[3].store(counter.fetch_add(1));
    }, "D"), {id_b, id_c});

    graph.start();

    EXPECT(graph.handle(id_d).wait_for(3000ms) == true);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT(graph.handle(i).state() == TaskOutcome::Done);
    }
    // A 一定第 0 个，D 一定最后（第 3 个）；B/C 中间任意
    EXPECT(seq[0].load() == 0);
    EXPECT(seq[3].load() == 3);
    EXPECT(sum.load() == 60);          // 依赖链上的副作用都生效了
    EXPECT(graph.finished());

    executor.shutdown();
}

TEST(TaskGraph, FailurePropagatesToDependents) {
    Executor executor(2);
    TaskGraph graph(executor);

    std::atomic<bool> downstream_ran{false};

    auto id_a = graph.add(Task([] {
        throw std::runtime_error("boom");
    }, "A"));
    auto id_b = graph.add(Task([&downstream_ran] {
        downstream_ran.store(true);
    }, "B"), {id_a});
    auto id_c = graph.add(Task([&downstream_ran] {
        downstream_ran.store(true);
    }, "C"), {id_b});

    graph.start();

    EXPECT(graph.handle(id_c).wait_for(2000ms) == true);
    EXPECT(graph.handle(id_a).state() == TaskOutcome::Failed);
    EXPECT(graph.handle(id_b).state() == TaskOutcome::Skipped);
    EXPECT(graph.handle(id_c).state() == TaskOutcome::Skipped);
    EXPECT(downstream_ran.load() == false);   // 后继一次都没执行
    EXPECT(graph.finished());

    executor.shutdown();
}

TEST(TaskGraph, CancelBeforeStartSkipsAll) {
    Executor executor(2);
    TaskGraph graph(executor);

    std::atomic<int> ran{0};
    auto id_a = graph.add(Task([&ran] { ran.fetch_add(1); }, "A"));
    auto id_b = graph.add(Task([&ran] { ran.fetch_add(1); }, "B"), {id_a});

    graph.request_stop();                     // 启动前取消

    EXPECT(graph.handle(id_a).state() == TaskOutcome::Skipped);
    EXPECT(graph.handle(id_b).state() == TaskOutcome::Skipped);
    EXPECT(graph.finished());

    graph.start();                            // 之后再启动：包装体看到令牌即收尾
    EXPECT(graph.handle(id_b).wait_for(1000ms) == true);
    EXPECT(ran.load() == 0);
    EXPECT(graph.handle(id_a).state() == TaskOutcome::Skipped);

    executor.shutdown();
}

TEST(TaskGraph, FanOutAllRunAfterRoot) {
    Executor executor(4);
    TaskGraph graph(executor);

    std::atomic<int> root_seq{-1};
    std::array<std::atomic<int>, 5> child_seq{};
    for (auto& s : child_seq) {
        s.store(-1);
    }
    std::atomic<int> counter{0};
    std::atomic<int> ran{0};

    auto root = graph.add(Task([&root_seq, &counter] {
        root_seq.store(counter.fetch_add(1));
    }, "root"));

    std::vector<TaskGraph::NodeId> children;
    for (int i = 0; i < 5; ++i) {
        children.push_back(graph.add(Task([&child_seq, &counter, &ran, i] {
            child_seq[static_cast<std::size_t>(i)].store(counter.fetch_add(1));
            ran.fetch_add(1);
        }, "child" + std::to_string(i)), {root}));
    }

    graph.start();

    for (const auto id : children) {
        EXPECT(graph.handle(id).wait_for(3000ms) == true);
    }
    EXPECT(ran.load() == 5);
    EXPECT(root_seq.load() == 0);             // 根永远最先
    for (int i = 0; i < 5; ++i) {
        EXPECT(child_seq[static_cast<std::size_t>(i)].load() >= 1);  // 都在根之后
    }

    executor.shutdown();
}

TEST(TaskGraph, InvalidDependencyRejected) {
    Executor executor(1);
    TaskGraph graph(executor);

    auto id_a = graph.add(Task([] {}, "A"));

    // 1) 依赖不存在（越界）
    bool threw_oob = false;
    try {
        graph.add(Task([] {}, "B"), {42});
    } catch (const std::invalid_argument&) {
        threw_oob = true;
    }
    EXPECT(threw_oob);

    // 2) 依赖重复
    bool threw_dup = false;
    try {
        graph.add(Task([] {}, "C"), {id_a, id_a});
    } catch (const std::invalid_argument&) {
        threw_dup = true;
    }
    EXPECT(threw_dup);

    // 3) start 之后不能再加节点
    graph.add(Task([] {}, "D"), {id_a});
    graph.start();
    bool threw_after = false;
    try {
        graph.add(Task([] {}, "E"));
    } catch (const std::logic_error&) {
        threw_after = true;
    }
    EXPECT(threw_after);

    executor.shutdown();
}

TEST_MAIN()
