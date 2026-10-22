// SynapseMatrix — Telemetry：运行时可观测性（Month 3 项目对象之一）
// ====================================================================
// 规格书 Month 3 项目对象之一：Telemetry。
// 语义：统计 Runtime 的运行指标——谁执行了多少任务、执行耗时、
// 多少任务被拒绝（submit 到已关闭的队列）。
// 可观测性（"先可观测，后优化"是规格书核心原则）——没有计数，
// 优化无从谈起。
//
// 深化内容（2026-10-22）：
//   1. 每 worker 独立计数（负载均衡/热点观察）
//   2. 拒绝计数（submit 失败次数）
//   3. 执行耗时统计（总耗时 + 平均耗时，微秒精度）
//
// 线程安全设计（全部无锁）：
//   - done_per_worker_  : vector<atomic<size_t>>，每个 worker 一格，
//                         各格独立原子自增——多 worker 同时写互不干扰
//   - rejected_ / total_elapsed_us_：单一原子量，fetch_add 累加
//   - 为什么 vector<atomic> 必须在构造时定大小：atomic 不可拷贝/
//     不可移动，vector 的 resize/扩容需要移动构造 → 编译失败。
//     构造定大小后永不改变，每个 atomic 被自己的 worker 独占写。
//
// 语法讲解：
//   - std::atomic<T>::fetch_add(n)：原子地执行 v += n 并返回旧值
//   - std::chrono::steady_clock：单调时钟（不受系统改时间影响），
//     测耗时用它；duration_cast 把纳秒转成微秒/毫秒
//   - vector<atomic<size_t>>(n)：n 个默认构造的 atomic（值为 0）
//   - std::accumulate：<numeric> 里的求和（只读遍历 load()）

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <numeric>
#include <vector>

namespace synapse::runtime {

class Telemetry {
public:
    // 构造：固定 worker 数（每 worker 一格独立计数）。
    // 注意：必须在 Executor 构造时一次性定好，之后不能扩容。
    explicit Telemetry(std::size_t num_workers)
        : done_per_worker_(num_workers) {}

    // Worker 完成一个任务时调用：该 worker 计数 +1，累计耗时
    void record_task_done(int worker_id,
                          std::chrono::microseconds elapsed = {}) {
        done_per_worker_.at(static_cast<std::size_t>(worker_id))
            .fetch_add(1);                // 该 worker 专属格子原子 +1
        total_elapsed_us_.fetch_add(      // 总耗时累加（无锁）
            static_cast<std::uint64_t>(elapsed.count()));
    }

    // submit 到已关闭队列（被拒）时调用
    void record_rejected() {
        rejected_.fetch_add(1);
    }

    // 队列水位采样：记录历史最大积压（峰值，单调不减）。
    // 调用时机：每次 submit 成功后（入队使队列变长，可能创新高）。
    // 无锁实现：CAS 循环更新 max——
    //   load 当前峰值 → 若新值更大则 CAS 换入；失败说明别的线程
    //   已把峰值提得更高，重试。对应 408 "无锁同步"思想。
    void record_watermark(std::size_t current) {
        std::size_t peak = peak_pending_.load();
        while (current > peak &&
               !peak_pending_.compare_exchange_weak(peak, current)) {
            // CAS 失败：peak 已被刷新为最新值（别的线程更新），
            // 循环重新比较 current 与最新 peak
        }
    }

    // ---- 查询（快照语义：调用瞬间的值）----

    // 总执行数 = 各 worker 求和
    [[nodiscard]] std::size_t tasks_done() const noexcept {
        std::size_t sum = 0;
        for (const auto& c : done_per_worker_) {
            sum += c.load();
        }
        return sum;
    }

    // 单个 worker 的执行数（worker_id 越界返回 0）
    [[nodiscard]] std::size_t task_count(int worker_id) const noexcept {
        if (worker_id < 0 ||
            static_cast<std::size_t>(worker_id) >= done_per_worker_.size()) {
            return 0;
        }
        return done_per_worker_[static_cast<std::size_t>(worker_id)].load();
    }

    // 被拒绝的任务数
    [[nodiscard]] std::size_t rejected() const noexcept {
        return rejected_.load();
    }

    // 队列积压峰值（历史最大未消费任务数）——背压/容量规划依据
    [[nodiscard]] std::size_t peak_pending() const noexcept {
        return peak_pending_.load();
    }

    // 总执行耗时（微秒）
    [[nodiscard]] std::uint64_t total_elapsed_us() const noexcept {
        return total_elapsed_us_.load();
    }

    // 平均每任务耗时（毫秒，double 精度）；无任务时为 0
    [[nodiscard]] double avg_task_ms() const noexcept {
        const auto n = tasks_done();
        if (n == 0) {
            return 0.0;
        }
        return static_cast<double>(total_elapsed_us_.load()) / n / 1000.0;
    }

    // worker 总数
    [[nodiscard]] std::size_t worker_count() const noexcept {
        return done_per_worker_.size();
    }

private:
    // 每 worker 一格；下标 = worker_id。构造定大小，永不 resize。
    std::vector<std::atomic<std::size_t>> done_per_worker_;
    std::atomic<std::size_t> rejected_{0};           // 拒绝计数
    std::atomic<std::uint64_t> total_elapsed_us_{0}; // 总耗时（微秒）
    std::atomic<std::size_t> peak_pending_{0};       // 队列积压峰值
};

}  // namespace synapse::runtime
