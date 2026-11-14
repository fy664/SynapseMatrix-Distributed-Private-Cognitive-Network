// SynapseMatrix — TaskGraph（DAG 依赖调度）单测（Month 3 环节 16）
// ==================================================================
// 覆盖点：
//   1. 线性链 A→B→C 严格按拓扑序执行
//   2. 菱形 A→(B,C)→D：A 必最先、D 必最后，B/C 并行不分先后
//   3. 节点抛异常 → 后续依赖递归 Skipped，图正常收尾不卡死
//   4. 启动前 request_stop：全图 Skipped，句柄立刻可读
//   5. 一父多子扇出：所有子都能在根完成后执行
//   6. 非法依赖（越界 / 重复 / start 后添加）抛异常
//   7. 环节 17：线性链时序 —— 每个节点松弛量恒为 0
//   8. 环节 17：菱形 —— 短分支有正松弛，关键路径只走长分支
//   9. 环节 17：失败图无 Done 节点 → 关键路径返回 nullopt
//  10. 环节 17：EF = ES + 耗时、LF = LS + 耗时（会计恒等式）
//
// 语法讲解：
//   - std::atomic<size_t> order_idx：并发下给"执行顺序"编号（fetch_add
//     返回旧值 = 该任务排第几），避免手工加锁
//   - 用 std::vector<std::atomic<int>>? 不行：atomic 不可移动/复制，
//     容器元素需可构造——这里改用定长裸数组 + atomic<int>
//     （或用 vector<unique_ptr<atomic<int>>>）；本文件用 std::array
//   - EXPECT 内联比较：失败打印文件:行，不中断用例

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
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

// ---- 环节 17：时序分析 / 关键路径 ----

// 线性链：每个节点的最晚开始 = 最早开始 → 松弛量恒为 0
TEST(TaskGraph, ChainTimingsHaveZeroSlack) {
    Executor executor(2);
    TaskGraph graph(executor);

    auto a = graph.add(Task([] { std::this_thread::sleep_for(10ms); }, "A"));
    auto b = graph.add(Task([] { std::this_thread::sleep_for(20ms); }, "B"), {a});
    auto c = graph.add(Task([] { std::this_thread::sleep_for(5ms); }, "C"), {b});

    graph.start();
    EXPECT(graph.handle(c).wait() == TaskOutcome::Done);

    const auto table = graph.timings();
    EXPECT(table.size() == 3);                // 只有 Done 节点入表
    for (const auto& t : table) {
        EXPECT(t.slack == 0us);               // 链上无空隙 → 全关键
        EXPECT(t.critical == true);
    }
    // 最早开始 = 前面所有耗时的前缀和
    EXPECT(table[0].earliest_start == 0us);
    EXPECT(table[1].earliest_start >= 9500us);   // ~10ms
    EXPECT(table[2].earliest_start >= 29000us);  // ~10+20ms

    const auto path = graph.critical_path();
    EXPECT(path.has_value());
    if (path) {
        EXPECT(path->nodes.size() == 3);      // A→B→C 全部在关键路径上
        EXPECT(path->nodes[0] == a);
        EXPECT(path->nodes[2] == c);
        EXPECT(path->total >= 33000us);       // 总时长 ≥ 35ms 的保守下界
    }

    executor.shutdown();
}

// 菱形：短分支 C 有正松弛，关键路径只走长分支 A→B→D
TEST(TaskGraph, DiamondSlackOnShortBranch) {
    Executor executor(4);
    TaskGraph graph(executor);

    auto a = graph.add(Task([] { std::this_thread::sleep_for(10ms); }, "A"));
    auto b = graph.add(Task([] { std::this_thread::sleep_for(40ms); }, "B"), {a});
    auto c = graph.add(Task([] { std::this_thread::sleep_for(10ms); }, "C"), {a});
    auto d = graph.add(Task([] { std::this_thread::sleep_for(5ms); }, "D"), {b, c});

    graph.start();
    EXPECT(graph.handle(d).wait() == TaskOutcome::Done);

    const auto table = graph.timings();
    EXPECT(table.size() == 4);

    // 找 C 那一条：松弛应约等于 B 比 C 多花的 30ms（允许调度抖动）
    const auto& node_c = table[2];
    EXPECT(node_c.name == "C");
    EXPECT(node_c.slack > 20ms);
    EXPECT(node_c.critical == false);

    const auto path = graph.critical_path();
    EXPECT(path.has_value());
    if (path) {
        EXPECT(path->nodes.size() == 3);      // 长分支上只有 A、B、D
        EXPECT(std::find(path->nodes.begin(), path->nodes.end(), a) !=
               path->nodes.end());
        EXPECT(std::find(path->nodes.begin(), path->nodes.end(), b) !=
               path->nodes.end());
        EXPECT(std::find(path->nodes.begin(), path->nodes.end(), c) ==
               path->nodes.end());            // 短分支不在关键路径上
        EXPECT(path->nodes.back() == d);
    }

    executor.shutdown();
}

// 失败图：全部失败/跳过 → 没有 Done 节点 → 关键路径 nullopt
TEST(TaskGraph, NoCriticalPathWithoutDoneNodes) {
    Executor executor(2);
    TaskGraph graph(executor);

    auto a = graph.add(Task([] { throw std::runtime_error("boom"); }, "A"));
    auto b = graph.add(Task([] {}, "B"), {a});

    graph.start();
    EXPECT(graph.handle(b).wait() == TaskOutcome::Skipped);

    EXPECT(graph.timings().empty());
    EXPECT(graph.critical_path().has_value() == false);

    executor.shutdown();
}

// 会计恒等式：EF = ES + 耗时，LF = LS + 耗时（任何图都必须成立）
TEST(TaskGraph, TimingArithmeticConsistent) {
    Executor executor(4);
    TaskGraph graph(executor);

    auto a = graph.add(Task([] { std::this_thread::sleep_for(5ms); }, "A"));
    auto b = graph.add(Task([] { std::this_thread::sleep_for(15ms); }, "B"), {a});
    auto c = graph.add(Task([] { std::this_thread::sleep_for(5ms); }, "C"), {a});
    auto d = graph.add(Task([] { std::this_thread::sleep_for(5ms); }, "D"), {b, c});

    graph.start();
    EXPECT(graph.handle(d).wait() == TaskOutcome::Done);

    const auto table = graph.timings();
    EXPECT(table.size() == 4);
    for (const auto& t : table) {
        EXPECT(t.earliest_finish == t.earliest_start + t.duration);
        EXPECT(t.latest_finish == t.latest_start + t.duration);
        EXPECT(t.slack == t.latest_start - t.earliest_start ||
               t.slack == 0us);              // 截断到 0 的宽松校验
    }
    // 最晚结束不能超过总工期
    const auto path = graph.critical_path();
    EXPECT(path.has_value());
    if (path) {
        for (const auto& t : table) {
            EXPECT(t.earliest_finish <= path->total);
            EXPECT(t.latest_finish <= path->total);
        }
    }

    executor.shutdown();
}

TEST_MAIN()
