// SynapseMatrix — TaskGraph：任务依赖图（DAG 调度，Month 3 环节 16）
// =====================================================================
// 规格书 Month 3：Task 生命周期治理（依赖编排）。
//
// 为什么需要依赖图（408 + 工程）：
//   到现在为止提交的任务彼此独立——谁先谁后由调度器随机决定。
//   真实流水线里任务有先后约束：C 要用 A、B 的结果，必须等两者都完成。
//   这就是 408 的 **AOV 网**（Activity On Vertex）：顶点 = 任务，
//   有向边 = 先后约束；入度 = 该任务的前驱数。
//   经典解法 = **拓扑排序**（每次取入度为 0 的顶点，去掉它的出边），
//   这里的实现就是它的并行版：任务完成时把后继入度减 1，
//   减到 0 的后继立刻提交执行。
//
// 实现要点：
//   1. add()      ：登记任务 + 声明依赖，构建 succ（后继）邻接表
//   2. start()    ：拓扑检查（有环直接抛异常）+ 提交所有入度为 0 的节点
//   3. 节点包装任务执行完：settle 记录结果 → 成功则 release 后继
//      （入度 -1，减到 0 就 submit），失败/取消则递归标记后继为 Skipped
//   4. request_stop()：图级取消——未完成节点全部 Skipped（不依赖
//      Executor 的 stop，因为清空队列会让后继永远等不到释放）
//
// 与 Executor 的关系：
//   TaskGraph 不拥有线程，只向外部 Executor 提交包装任务（组合而非继承）。
//   注意：请配合优雅停机（shutdown）；如果对底层 Executor 调 stop()
//   （清空队列），在途依赖会断链——此时应同时调 TaskGraph::request_stop()
//   让所有等待者被唤醒。
//
// 408 映射：
//   - AOV 网 + 拓扑排序：入度归零就绪
//   - 信号量 P/V：入度计数可看作"需要几个 V 才能唤醒"（前驱完成 = V）
//   - 死锁：有环的依赖图永远有节点入度不为 0 = 循环等待 → 必须提前检测
//
// 语法讲解：
//   - std::shared_ptr<State>：图的数据被提交出去的包装任务共享持有，
//     即使 TaskGraph 对象先销毁，任务仍能安全访问（shared_ptr 保活）
//   - vector<unique_ptr<Node>>：Node 含 atomic（不可移动）→ 用指针容器
//   - atomic<int>::fetch_sub：原子减一并返回旧值（==1 表示减到 0）

#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "runtime/executor.hpp"
#include "runtime/stop_token.hpp"
#include "runtime/task.hpp"
#include "runtime/task_handle.hpp"

namespace synapse::runtime {

class TaskGraph {
public:
    using NodeId = std::size_t;

    // 绑定一个执行器（不拥有它，生命周期由调用方保证）
    explicit TaskGraph(Executor& executor);

    // 不可复制：内部持有共享状态与引用
    TaskGraph(const TaskGraph&) = delete;
    TaskGraph& operator=(const TaskGraph&) = delete;

    // 登记一个任务节点，deps = 前驱节点列表（空 = 无依赖，可立即执行）。
    // 返回节点 id（从 0 递增）。依赖 id 必须已存在且不能自依赖，
    // 否则抛 std::invalid_argument。
    NodeId add(Task task, std::vector<NodeId> deps = {});

    // 启动：拓扑检查（有环抛 std::invalid_argument）→ 提交全部入度 0 的节点。
    // 重复调用无副作用。
    void start();

    // 图级取消：置位内部令牌 → 所有未完成节点标记 Skipped（等待者被唤醒）；
    // 正在执行的节点包装任务在下一个节点边界看到令牌后收尾。
    // 与 start 的先后无关；可在 start 前调用（则整图直接全部 Skipped）。
    void request_stop();

    // 图级取消令牌（任务体可捕获它，在长循环里自查提前返回）
    [[nodiscard]] const StopTokenPtr& token() const noexcept {
        return state_->token;
    }

    // 节点句柄：wait() 返回 Done / Skipped / Failed
    [[nodiscard]] TaskHandle handle(NodeId id) const;

    [[nodiscard]] std::size_t node_count() const noexcept;

    // 是否全部节点都已落定（完成/跳过/失败）
    [[nodiscard]] bool finished() const noexcept;

private:
    // 单个节点
    struct Node {
        // Task 没有默认构造函数（std::function 无默认体）→ Node 必须显式
        // 用任务构造，不能用"先默认构造后赋值"
        explicit Node(Task t) : task(std::move(t)) {}

        Task task;                        // 任务本体
        std::atomic<int> deps;            // 剩余前驱数（归零 = 可执行）
        std::vector<NodeId> succ;         // 后继（完成任务时递减它们的 deps）
        TaskCompletionPtr completion;     // 句柄共享状态
        std::atomic<bool> settled{false}; // 是否已落定（防重复结算/递归）
    };

    // 图数据：被包装任务共享持有（shared_ptr 保活）
    // 继承 enable_shared_from_this：包装任务里可拿到自己的 shared_ptr
    struct State : public std::enable_shared_from_this<State> {
        std::vector<std::unique_ptr<Node>> nodes;
        std::atomic<std::size_t> settled_count{0};
        StopTokenPtr token;               // 图级取消令牌
    };

    // 提交一个节点（包装任务：执行 → 结算 → 释放后继）
    void submit_node(NodeId id);

    // ---- 静态实现（包装任务里调用，不依赖 TaskGraph 对象存活）----
    // 用 static 而非成员函数：包装任务只持有 shared_ptr<State> 与
    // Executor*，TaskGraph 对象先销毁也不会悬垂。
    static void submit_static(State& state, Executor& executor, NodeId id);

    // 结算节点（幂等）：第一次生效时写句柄并累加计数
    static bool settle_static(State& state, NodeId id, TaskOutcome outcome);

    // 释放后继：入度减 1，减到 0 就提交（成功路径）
    static void release_static(State& state, Executor& executor, NodeId id);

    // 递归标记后继为 Skipped（失败/取消路径）
    static void skip_static(State& state, NodeId id);

    std::shared_ptr<State> state_;  // 图数据（与包装任务共享）
    Executor& executor_;            // 提交目标（外部拥有）
    bool started_ = false;          // 防重复启动
};

}  // namespace synapse::runtime
