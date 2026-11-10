// SynapseMatrix — Task：Runtime 的最小工作单元
// ==================================================
// 规格书 Month 3 项目对象之一：Task。
//
// 语义：
//   Task 包装"一段可执行的工作"（任意可调用对象）加上调度元数据。
//   调度器/执行器只看 Task 的元数据（优先级等），不关心内部是什么。
//
// 与 C++ 标准库的关系：
//   - 内部用 std::function<void()> 存可调用体——类型擦除：
//     调用方传 lambda / 函数指针 / 可调用对象都行
//   - 优先级在 V1 只是记录字段；V2 接调度策略（规格书 L2 Scheduler）
//
// 环节 14（2026-11-10）：协作式中断
//   Task 增加 StopToken 共享指针。语义分两层：
//     1. 执行前检查：令牌已置位 → run() 直接跳过任务体（返回 false），
//        排队任务被"惰性丢弃"——不用清队列，省一次全表扫描。
//     2. 执行中自查：任务体内的长循环调用 should_stop()，
//        看到停止请求就提前 return（协作式取消的唯一安全做法）。
//
// 语法讲解（C++20）：
//   - std::function<void()>：可调用对象容器，类似"函数指针的泛化版"
//   - [[nodiscard]]：调用方必须使用返回值，否则编译警告
//   - inline：类内定义的成员函数隐式 inline，头文件多处包含不重定义
//   - std::move：把对象"转移"而非复制，避免深拷贝（移动语义）
//   - std::shared_ptr<T>：引用计数智能指针——多个 Task 共享同一个
//     StopToken，最后一个持有者销毁时令牌才释放（无悬垂引用）

#pragma once

#include <functional>
#include <string>
#include <utility>

#include "runtime/stop_token.hpp"

namespace synapse::runtime {

// Task 优先级：数值越大越优先（V2 调度器使用，V1 仅记录）
enum class Priority : int {
    Low = 0,
    Normal = 1,
    High = 2,
};

class Task {
public:
    // 构造：收下可调用体 + 元数据
    // 参数 work 用 std::move 转移进来，避免大闭包复制
    // token 默认 nullptr：独立构造的 Task 不参与取消（行为与环节 13 一致）
    explicit Task(std::function<void()> work,
                  std::string id = "",
                  Priority prio = Priority::Normal,
                  StopTokenPtr token = nullptr)
        : work_(std::move(work)),
          id_(std::move(id)),
          priority_(prio),
          stop_token_(std::move(token)) {}

    // 执行任务体，返回"是否真的执行了"：
    //   true  = 正常执行完毕
    //   false = 被协作式取消跳过（令牌已置位），或任务体为空
    // 跳过逻辑放在这里而不是 Worker 里：任何持有 Task 的地方
    // （Worker / WorkStealingPool / 后续自定义执行器）都自动获得
    // 同一套取消语义，不需要各自重复判断。
    [[nodiscard]] bool run() {
        if (!work_ || should_stop()) {
            return false;          // 空任务 / 已请求停止 → 不执行
        }
        work_();
        return true;
    }

    // 兼容旧调用点：operator() 忽略返回值（Worker 已改用 run() 取结果）
    void operator()() { (void)run(); }

    // 任务体内部的自查接口：长循环里周期性调用，
    // 返回 true 就尽快 return（把已持有的资源交给 RAII 释放）
    [[nodiscard]] bool should_stop() const noexcept {
        return stop_token_ && stop_token_->is_stop_requested();
    }

    // 是否有可执行体（构造时传了空函数时为 false）
    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(work_); }

    // 是否已绑定停止令牌（Executor 提交时据此决定要不要补挂）
    [[nodiscard]] bool has_token() const noexcept {
        return static_cast<bool>(stop_token_);
    }

    // 补挂令牌：Executor::submit 对未携带令牌的任务自动挂上自己的令牌，
    // 使"运行时级别的停止请求"能广播到全部任务
    void attach_token(StopTokenPtr token) { stop_token_ = std::move(token); }

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] Priority priority() const noexcept { return priority_; }

private:
    std::function<void()> work_;   // 类型擦除的可调用体
    std::string id_;               // 任务标识（Telemetry 用）
    Priority priority_;            // 优先级（V1 记录，V2 生效）
    StopTokenPtr stop_token_;      // 协作式取消令牌（可空 = 不参与取消）
};

}  // namespace synapse::runtime
