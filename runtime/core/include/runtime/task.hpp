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
// 语法讲解（C++20）：
//   - std::function<void()>：可调用对象容器，类似"函数指针的泛化版"
//   - [[nodiscard]]：调用方必须使用返回值，否则编译警告
//   - inline：类内定义的成员函数隐式 inline，头文件多处包含不重定义
//   - std::move：把对象"转移"而非复制，避免深拷贝（移动语义）

#pragma once

#include <functional>
#include <string>
#include <utility>

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
    explicit Task(std::function<void()> work,
                  std::string id = "",
                  Priority prio = Priority::Normal)
        : work_(std::move(work)), id_(std::move(id)), priority_(prio) {}

    // 执行任务体。const 方法？不行——work_ 被调用即修改其状态，
    // 所以是非 const 方法（operator() 的语义）
    void operator()() {
        if (work_) {
            work_();
        }
    }

    // 是否有可执行体（构造时传了空函数时为 false）
    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(work_); }

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] Priority priority() const noexcept { return priority_; }

private:
    std::function<void()> work_;   // 类型擦除的可调用体
    std::string id_;               // 任务标识（Telemetry 用）
    Priority priority_;            // 优先级（V1 记录，V2 生效）
};

}  // namespace synapse::runtime
