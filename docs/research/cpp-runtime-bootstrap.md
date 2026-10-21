# Research Question：怎么在 Windows 上把 C++ Runtime 工程立起来？（Month 3 环节 1）

## Problem

规格书 Month 3 的验收项：自己实现多线程 Task Runtime
（Device / Worker / Task / Queue / Executor / Telemetry）。
但项目里 C++ 侧还是零——没有 CMake 工程、没有测试框架、
工具链（vcvars64 + cl）没在项目内验证过。
第一步是把"能编译、能测试、能复现构建"的骨架立起来。

## 分工（沿用既定协议）

- 基础设施（环境/骨架/测试框架/构建脚本）——AI 负责
- 核心算法（Worker/Executor 的多线程循环）——用户按规格书自己实现

## Background

**工具链**：本机 VS18 Community，MSVC 19.51（cl 不在 PATH，
需 `vcvars64.bat`）；CMake 4.3.1 但 PATH 里是 Git Bash shim
（`cmake.cmd`/`ctest` 转发到 VS 内置 CMake）。

**Month 3 必学清单**（规格书）：C++20 / CMake / RAII / Smart Pointer /
Move Semantics / Thread / Mutex / Atomic / Condition Variable / Thread Pool。
第一环节只需要前 7 项的子集——把并发原语跑通。

## Experiment

1. **CMake 工程**：`runtime/CMakeLists.txt`——C++20、MSVC 下
   `/utf-8 /EHsc /W4`（中文注释不报 C4819、异常不崩线程、四级警告）。
2. **自研测试框架**：`runtime/core/tests/test_framework.hpp`
   ——零依赖单头（静态注册表 + 宏 + 断言），不赌联网拉 gtest。
3. **Task**：`runtime/core/include/runtime/task.hpp`——std::function
   类型擦除 + id + 优先级（V1 记录，V2 调度用）。
4. **线程安全 Queue**：`runtime/core/include/runtime/queue.hpp`
   ——mutex + condition_variable + deque；支持 close（优雅停机）；
   push/pop 全并发安全。
5. **Worker/Executor/Device/Telemetry**：接口声明 + 语义注释，
   核心循环留 TODO 给用户实现。
6. **demo_threadpool.cpp**：手工 3 线程 + 10 任务的消费者演示，
   验证 thread/mutex/CV/atomic 全部可用。
7. **构建脚本**：`build_runtime.bat`——一键 vcvars + configure + build。

## Result

- 编译：synapse_core.lib + demo_threadpool.exe + test_queue.exe +
  test_task.exe，**零警告**（/W4）
- 单测：**9 个用例全过**（Queue 5 + Task 4），含 4 生产者 × 1000
  任务并发无丢失
- CTest：2/2 passed
- demo_threadpool：10 任务被 3 个 worker 正确消费、优雅关闭 join

## 踩坑记录（本环节最有价值的部分）

1. **LF-only 的 .bat 在 cmd 里会被吞行**：Write 工具写出的 bat 是
   LF 行尾，cmd 解析到某行后静默停止执行后续命令。必须 CRLF。
   这是"cmd + LF bat"的经典坑。
2. **PATH 里 cmake 是 .cmd shim**：`C:\Users\Administrator\bin\cmake.cmd`
   （Git Bash shim，转发到 VS 内置 CMake）。在 .bat 里直接调用
   **控制权不返回**，bat 提前终止——必须 `call cmake ...`。
   PowerShell 里同理：管道中间不能运行 .cmd，要 `&` 调用或找真身
   `...\CMake\bin\ctest.exe`。
3. **测试可执行文件必须有 main**：自研框架只提供断言/注册，
   `TEST_MAIN()` 宏生成 main——测试文件末尾调用。

## 408 映射

- **生产者-消费者问题**：Queue 就是经典解法——mutex 保护共享
  区、CV 避免忙等、close 对应"结束信号"。
- **线程生命周期**：join 对应线程回收；std::thread 析构不 join
  会 terminate——和"僵尸进程必须 wait"同理。
- **原子量 vs 锁**：Telemetry 计数用 atomic（无锁 read-modify-write），
  队列用 mutex（临界区多条语句）——对应临界区与互斥的理论。

## Conclusion

Month 3 骨架立住：CMake + 测试框架 + Task/Queue 基础设施 +
可复现构建脚本（build_runtime.bat），工具链闭环验证通过。
下一步（你的环节）：按 worker.hpp / executor.hpp 的 TODO 实现
Worker 线程循环与 Executor 线程池，Telemetry 计数接上，
然后跑 demo 验证多线程 Task Runtime 验收。

## Next Question

- Worker 的 run() 循环：pop 到 nullopt 退出——队列 close 后
  如何保证 worker 把已取出的任务执行完再退出？（边执行边检查）
- Executor 析构顺序：workers_ 是 vector，析构按逆序——若
  queue 先析构而 worker 还在 pop 会怎样？（成员析构顺序坑）
