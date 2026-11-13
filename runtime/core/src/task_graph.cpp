// SynapseMatrix — TaskGraph 实现（Month 3 环节 16）
// ==================================================
// DAG 调度：把「任务 + 依赖」编排成 AOV 网，按拓扑序并行推进。
//
// 执行模型（一句话）：
//   入度为 0 的节点入队执行；节点成功完成 → 后继入度 -1；
//   减到 0 的后继立即可执行（立刻提交）；节点失败/取消 → 后继递归 Skipped。
//
// 为什么包装任务要挂"哑令牌"：
//   Executor::submit 会给没有令牌的任务补挂自己的令牌。如果底层调用
//   request_stop()/stop()，包装任务会被跳过 → 后继永远等不到入度-1 →
//   图卡死（等待者永久阻塞）。
//   所以包装任务预先挂一个**谁也不置位**的独立令牌，保证包装体一定执行：
//   结算与释放后继由我们自己的图级令牌控制，语义更可预期。
//
// 语法讲解：
//   - std::enable_shared_from_this：在对象内部拿到自己的 shared_ptr，
//     用于把"图的共享状态"捕获进提交出去的包装任务（保活）
//   - atomic<bool>::exchange(true)：原子读旧值并写 true（一次性标记）
//   - fetch_sub(1) == 1：原子减一返回旧值，旧值 1 表示"减到 0"——
//     只有最后一个完成的前驱会看到 1（并发下的"最后一票"判定）
//   - shared_ptr 捕获进 lambda：包装任务持有图数据的强引用，
//     TaskGraph 对象析构后内存仍有效

#include "runtime/task_graph.hpp"

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

namespace synapse::runtime {

TaskGraph::TaskGraph(Executor& executor)
    : state_(std::make_shared<State>()), executor_(executor) {
    state_->token = std::make_shared<StopToken>();
}

TaskGraph::NodeId TaskGraph::add(Task task, std::vector<NodeId> deps) {
    if (started_) {
        throw std::logic_error("TaskGraph::add：start() 之后不能再添加节点");
    }
    const NodeId id = state_->nodes.size();   // 新节点 id = 当前节点数
    for (std::size_t k = 0; k < deps.size(); ++k) {
        const NodeId dep = deps[k];
        if (dep >= id) {
            // 只有已存在的节点才能作为前驱（dep == id 即自依赖，同样非法）
            throw std::invalid_argument(
                "TaskGraph::add：依赖节点不存在或自依赖");
        }
        // 去重：同一前驱写两次会让入度多 1（虽然自洽，但语义容易误读）
        for (std::size_t j = 0; j < k; ++j) {
            if (deps[j] == dep) {
                throw std::invalid_argument(
                    "TaskGraph::add：依赖列表含重复节点");
            }
        }
    }

    auto node = std::make_unique<Node>(std::move(task));
    // 图级令牌：节点任务默认挂上图令牌（任务体可捕获自查）；
    // 已自带令牌的任务保持原样（调用方显式指定优先）
    if (!node->task.has_token()) {
        node->task.attach_token(state_->token);
    }
    node->deps.store(static_cast<int>(deps.size()), std::memory_order_relaxed);
    node->completion = std::make_shared<TaskCompletion>();
    // 建立后继边：每个前驱的 succ 表加上本节点
    for (const NodeId dep : deps) {
        state_->nodes[dep]->succ.push_back(id);
    }
    state_->nodes.push_back(std::move(node));
    return id;
}

void TaskGraph::start() {
    if (started_) {
        return;                                // 重复启动无副作用
    }
    started_ = true;

    const std::size_t n = state_->nodes.size();
    // ---- 拓扑检查（Kahn）：有环 → 永远有节点入度不为 0 → 死锁 ----
    // 用一份临时入度副本做 BFS，不改动真实计数器
    std::vector<int> indeg(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        indeg[i] = state_->nodes[i]->deps.load(std::memory_order_relaxed);
    }
    std::vector<NodeId> ready;                 // 入度为 0 的队列
    for (std::size_t i = 0; i < n; ++i) {
        if (indeg[i] == 0) {
            ready.push_back(i);
        }
    }
    std::size_t visited = 0;
    for (std::size_t head = 0; head < ready.size(); ++head) {
        const NodeId cur = ready[head];
        ++visited;
        for (const NodeId succ : state_->nodes[cur]->succ) {
            if (--indeg[succ] == 0) {
                ready.push_back(succ);
            }
        }
    }
    if (visited != n) {
        throw std::invalid_argument("TaskGraph::start：依赖图存在环");
    }

    // ---- 提交入度为 0 的节点（其余等前驱释放）----
    for (std::size_t i = 0; i < n; ++i) {
        if (state_->nodes[i]->deps.load(std::memory_order_relaxed) == 0 &&
            !state_->nodes[i]->settled.load(std::memory_order_relaxed)) {
            submit_node(i);
        }
    }
}

void TaskGraph::submit_node(NodeId id) {
    submit_static(*state_, executor_, id);
}

void TaskGraph::submit_static(State& state, Executor& executor, NodeId id) {
    auto shared = state.shared_from_this();   // 保活：包装任务持有强引用
    Executor* ex = &executor;                 // 执行器由调用方保证活到图结束
    std::function<void()> body = [shared, ex, id] {
        Node& node = *shared->nodes[id];
        // 图级取消已置位：直接结算跳过，并递归跳过后继
        if (shared->token->is_stop_requested()) {
            if (settle_static(*shared, id, TaskOutcome::Skipped)) {
                skip_static(*shared, id);
            }
            return;
        }
        static_cast<void>(node.task.run());    // [[nodiscard]]：结果走 last_outcome
        const TaskOutcome outcome = node.task.last_outcome();
        if (!settle_static(*shared, id, outcome)) {
            return;                           // 已被 request_stop 抢先结算
        }
        if (outcome == TaskOutcome::Done) {
            release_static(*shared, *ex, id); // 成功：释放后继
        } else {
            skip_static(*shared, id);         // 失败/跳过：后继全部跳过
        }
    };
    Task wrapper(std::move(body), "dag" + std::to_string(id));
    // 哑令牌：阻止 Executor 补挂自己的令牌（否则底层 stop 会跳过包装体，
    // 导致后继永远等不到入度-1，图卡死）
    wrapper.attach_token(std::make_shared<StopToken>());
    ex->submit(std::move(wrapper));
}

bool TaskGraph::settle_static(State& state, NodeId id, TaskOutcome outcome) {
    Node& node = *state.nodes[id];
    if (node.settled.exchange(true, std::memory_order_acq_rel)) {
        return false;                         // 已落定：不重复结算/递归
    }
    node.completion->complete(outcome);       // 唤醒句柄等待者
    state.settled_count.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void TaskGraph::release_static(State& state, Executor& executor, NodeId id) {
    for (const NodeId succ : state.nodes[id]->succ) {
        Node& node = *state.nodes[succ];
        // fetch_sub 返回旧值：旧值 1 表示"这是最后一个完成的前驱"
        if (node.deps.fetch_sub(1, std::memory_order_acq_rel) != 1) {
            continue;
        }
        // 入度归零：该后继可以执行
        if (state.token->is_stop_requested()) {
            if (settle_static(state, succ, TaskOutcome::Skipped)) {
                skip_static(state, succ);
            }
            continue;
        }
        submit_static(state, executor, succ); // 立刻提交（并行推进）
    }
}

void TaskGraph::skip_static(State& state, NodeId id) {
    for (const NodeId succ : state.nodes[id]->succ) {
        if (!settle_static(state, succ, TaskOutcome::Skipped)) {
            continue;   // 已落定（可能已经执行完成）→ 不重复递归
        }
        skip_static(state, succ);             // 传递性：后继的后继也跳过
    }
}

void TaskGraph::request_stop() {
    state_->token->request_stop();
    // 全部未落定节点统一结算为 Skipped：一次性唤醒所有等待者，
    // 不依赖包装任务被调度（已入队未执行的包装体之后会自己 no-op）
    for (NodeId i = 0; i < state_->nodes.size(); ++i) {
        settle_static(*state_, i, TaskOutcome::Skipped);
    }
}

TaskHandle TaskGraph::handle(NodeId id) const {
    if (id >= state_->nodes.size()) {
        throw std::out_of_range("TaskGraph::handle：节点 id 越界");
    }
    return TaskHandle(state_->nodes[id]->completion);
}

std::size_t TaskGraph::node_count() const noexcept {
    return state_->nodes.size();
}

bool TaskGraph::finished() const noexcept {
    return state_->settled_count.load(std::memory_order_relaxed) ==
           state_->nodes.size();
}

}  // namespace synapse::runtime
