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

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace synapse::runtime {

namespace {
// 当前时刻（steady_clock 微秒）
// steady_clock 是单调时钟：不会被系统校时/时区改动影响，专用于测耗时；
// 返回 long long 便于直接塞进 atomic<long long>
long long NowMicros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

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
        // 环节 17：记录真实起止时刻（steady_clock 单调时钟，单位微秒）
        node.begin_us.store(NowMicros(), std::memory_order_relaxed);
        static_cast<void>(node.task.run());    // [[nodiscard]]：结果走 last_outcome
        node.end_us.store(NowMicros(), std::memory_order_relaxed);
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

// ===================== 环节 17：时序分析与关键路径 =====================
// 408 AOE 网映射：
//   - 顶点活动（本实现是 AOV 形态）→ 把"节点耗时"当边权，等价于 AOE 的
//     活动时间 a；求的是"节点加权 DAG 的最长路径"
//   - ve（最早开始）：前驱全部结束后才能开始 → ve[j] = max(ve[i] + a[i])
//   - vl（最晚开始）：不能拖累总工期 → vl[j] = min(vl[k] - a[j])，
//     终点 vl = 总工期
//   - 松弛时间 = vl - ve；松弛为 0 的活动就是关键活动，串起来是关键路径

std::vector<TaskGraph::NodeTiming> TaskGraph::timings() const {
    const std::size_t n = state_->nodes.size();
    std::vector<bool> done(n, false);
    std::size_t done_count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        done[i] = state_->nodes[i]->completion->state() == TaskOutcome::Done;
        if (done[i]) {
            ++done_count;
        }
    }
    std::vector<NodeTiming> result;
    if (done_count == 0) {
        return result;                       // 没有成功节点 → 空表
    }

    // ---- 前置：前驱表（Node 只存了后继，这里反向扫一遍）----
    std::vector<std::vector<NodeId>> preds(n);
    for (std::size_t i = 0; i < n; ++i) {
        for (const NodeId succ : state_->nodes[i]->succ) {
            preds[succ].push_back(i);
        }
    }

    // ---- 拓扑序（只在 Done 子图上做 Kahn）----
    std::vector<int> indeg(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (!done[i]) {
            continue;
        }
        for (const NodeId succ : state_->nodes[i]->succ) {
            if (done[succ]) {
                ++indeg[succ];
            }
        }
    }
    std::vector<NodeId> order;
    order.reserve(done_count);
    for (std::size_t i = 0; i < n; ++i) {
        if (done[i] && indeg[i] == 0) {
            order.push_back(i);
        }
    }
    for (std::size_t head = 0; head < order.size(); ++head) {
        const NodeId cur = order[head];
        for (const NodeId succ : state_->nodes[cur]->succ) {
            if (done[succ] && --indeg[succ] == 0) {
                order.push_back(succ);
            }
        }
    }

    // ---- 耗时（微秒；异常/跳过节点不进表，所以一定是非负）----
    std::vector<long long> dur(n, 0);
    for (const NodeId id : order) {
        const long long begin = state_->nodes[id]->begin_us.load(
            std::memory_order_relaxed);
        const long long end = state_->nodes[id]->end_us.load(
            std::memory_order_relaxed);
        dur[id] = (end > begin && begin > 0) ? (end - begin) : 0;
    }

    // ---- 正向：ve / ef（前驱最大结束时刻 = 自己的最早开始）----
    std::vector<long long> es(n, 0);
    std::vector<long long> ef(n, 0);
    for (const NodeId id : order) {
        long long start = 0;
        for (const NodeId p : preds[id]) {
            if (done[p]) {
                start = std::max(start, ef[p]);
            }
        }
        es[id] = start;
        ef[id] = start + dur[id];
    }
    long long makespan = 0;
    for (const NodeId id : order) {
        makespan = std::max(makespan, ef[id]);
    }

    // ---- 反向：lf / ls（后继最小开始时刻 = 自己的最晚结束）----
    std::vector<long long> ls(n, 0);
    std::vector<long long> lf(n, 0);
    for (std::size_t k = order.size(); k > 0; --k) {
        const NodeId id = order[k - 1];
        long long finish = makespan;         // 没有后继（汇点）→ 最晚 = 总工期
        for (const NodeId s : state_->nodes[id]->succ) {
            if (done[s]) {
                finish = std::min(finish, ls[s]);
            }
        }
        lf[id] = finish;
        ls[id] = finish - dur[id];
    }

    // ---- 汇总（按节点 id 升序，便于阅读）----
    result.reserve(done_count);
    for (std::size_t i = 0; i < n; ++i) {
        if (!done[i]) {
            continue;
        }
        NodeTiming t;
        t.id = i;
        t.name = state_->nodes[i]->task.id();
        t.duration = std::chrono::microseconds(dur[i]);
        t.earliest_start = std::chrono::microseconds(es[i]);
        t.earliest_finish = std::chrono::microseconds(ef[i]);
        t.latest_start = std::chrono::microseconds(ls[i]);
        t.latest_finish = std::chrono::microseconds(lf[i]);
        const long long slack = ls[i] - es[i];
        t.slack = std::chrono::microseconds(slack > 0 ? slack : 0);
        t.critical = (slack <= 0);            // 松弛为 0 → 关键活动
        result.push_back(std::move(t));
    }
    return result;
}

std::optional<TaskGraph::CriticalPath> TaskGraph::critical_path() const {
    const auto table = timings();
    if (table.empty()) {
        return std::nullopt;                  // 没有任何成功节点 → 无关键路径
    }

    // 终点 = 最长路径的"汇点"：在没有任何 Done 后继的节点里取 EF 最大者。
    // （不能直接取全表 EF 最大：菱形下 merge/report 与 checksum 的 EF 相同，
    //   取到中间节点会把后半段路径截断）
    std::size_t best = 0;
    bool best_found = false;
    for (std::size_t k = 0; k < table.size(); ++k) {
        bool has_done_succ = false;
        for (const NodeId s : state_->nodes[table[k].id]->succ) {
            if (state_->nodes[s]->completion->state() == TaskOutcome::Done) {
                has_done_succ = true;
                break;
            }
        }
        if (has_done_succ) {
            continue;
        }
        if (!best_found ||
            table[k].earliest_finish > table[best].earliest_finish) {
            best = k;
            best_found = true;
        }
    }
    if (!best_found) {
        // 兜底：所有 Done 节点都有 Done 后继（理论上有环才会发生）
        for (std::size_t k = 1; k < table.size(); ++k) {
            if (table[k].earliest_finish > table[best].earliest_finish) {
                best = k;
            }
        }
    }

    // 回溯：找"结束时刻 == 我的开始时刻且松弛为 0"的前驱，逐段倒推
    std::vector<NodeId> rev;
    std::size_t cur = best;
    rev.push_back(table[cur].id);
    const std::size_t guard = table.size() + 1;   // 防御性上限，防环
    for (std::size_t step = 0; step < guard; ++step) {
        bool moved = false;
        // 在关键节点里找"直接前驱 + 结束时刻等于我的开始时刻"的那个
        for (std::size_t k = 0; k < table.size(); ++k) {
            if (!table[k].critical) {
                continue;
            }
            if (table[k].earliest_finish == table[cur].earliest_start &&
                table[k].earliest_finish > std::chrono::microseconds(0)) {
                // 该节点的超前驱，且是它的直接前驱（用 succ 校验）
                const auto& succ = state_->nodes[table[k].id]->succ;
                if (std::find(succ.begin(), succ.end(), table[cur].id) !=
                    succ.end()) {
                    cur = k;
                    rev.push_back(table[k].id);
                    moved = true;
                    break;
                }
            }
        }
        if (!moved) {
            break;
        }
    }

    CriticalPath path;
    path.total = table[best].earliest_finish;
    path.nodes.assign(rev.rbegin(), rev.rend());   // 倒序 → 起点到终点
    return path;
}

}  // namespace synapse::runtime
