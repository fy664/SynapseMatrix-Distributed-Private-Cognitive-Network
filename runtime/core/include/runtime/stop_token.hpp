// SynapseMatrix — StopToken：协作式中断令牌（Month 3 环节 14）
// ==================================================================
// 规格书 Month 3：Task 生命周期治理。
//
// 为什么需要"协作式"中断（408 + 工程）：
//   环节 12 的 Executor::stop() 能丢弃排队任务，但**正在执行的任务
//   打不断**——C++（以及 Java、.NET）都没有"从外部强制杀死线程"的
//   安全手段：Java 的 Thread.stop() 早已废弃，因为它可能在任意指令
//   处撕开栈，让锁/资源留在不一致状态。
//   现实可行的做法只有一个方向：**任务自己检查停止标志并主动返回**
//   （协作式 / cooperative cancellation）。运行时负责"广播停止信号"，
//   任务在安全点（循环体、阶段之间）自查后退出。
//
// 与环节 12 的分工：
//   stop()        ：进程级撤销——丢队列、立即不再收活（强，但打断不了
//                   正在跑的指令）
//   request_stop()：本环节新增——只广播令牌，让运行中的任务自查退出，
//                   排队任务执行前直接跳过（温和、可预期、资源状态安全）
//   两者可由调用方组合：先 request_stop（给任务体面的收尾时间），
//   超时后再 stop（强制收摊）。
//
// 为什么用 shared_ptr 而不是裸引用：
//   一个令牌要被 Executor、多个 Task、以及可能的外部线程（工作线程
//   之外的发起者）同时持有。shared_ptr 的引用计数保证"最后一个持有
//   者释放时对象才销毁"，不存在令牌比任务先死的悬垂引用。
//   令牌内部只有一个 atomic<bool>，多线程读写无锁。
//
// 408 映射：这是"进程/线程的协作式终止"——对应 408 里进程撤销需要
//   先运行到安全点、以及中断（interrupt）与轮询（polling）的区别：
//   中断是硬件/外部异步信号，协作式是软件轮询标志位（polling）。
//
// 语法讲解：
//   - std::atomic<bool>：原子的布尔量；store/load 都是单条指令级操作
//   - memory_order_release/acquire：写标志用 release，读标志用 acquire，
//     保证"置位前写入的其他状态"对看到置位的线程可见（这里其实只需
//     relaxed，因为标志独立于其他数据；用 release/acquire 更保险、
//     也符合"标志位语义"的教学口径）
//   - noexcept：承诺不抛异常（令牌操作是纯原子读写，不可能失败）
//   - using 别名：StopTokenPtr = std::shared_ptr<StopToken>，短名好写

#pragma once

#include <atomic>
#include <memory>

namespace synapse::runtime {

class StopToken {
public:
    StopToken() = default;

    // 不可复制：令牌是"共享状态的身份"，复制出两个独立令牌没有意义
    StopToken(const StopToken&) = delete;
    StopToken& operator=(const StopToken&) = delete;

    // 广播停止信号（幂等：重复置位无副作用，也允许后来者再调）
    // release：保证置位之前对共享状态的其他修改，对读到置位的线程可见
    void request_stop() noexcept {
        stop_requested_.store(true, std::memory_order_release);
    }

    // 任务体在安全点调用：返回 true 表示"外部已请求停止，应尽快返回"
    [[nodiscard]] bool is_stop_requested() const noexcept {
        return stop_requested_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> stop_requested_{false};  // 全局停止标志（单写下多读）
};

// 共享所有权别名（Executor 与全部 Task 共享同一个令牌）
using StopTokenPtr = std::shared_ptr<StopToken>;

}  // namespace synapse::runtime
