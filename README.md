# SynapseMatrix

端侧优先（Edge-first）、P2P 协同、隐私保护的个人分布式认知计算系统原型。

> 将一个长期项目作为主线，倒逼补齐 C++、操作系统、计算机网络、数据结构与算法、数据库、AI Engineering、全栈、分布式系统、GPU/推理、安全工程等能力。
> 详见 `docs/SynapseMatrix_技术规格书_V1.0.md`。

## What

由 PC / 手机 / 平板 / 嵌入式设备组成的个人计算网络，可共享计算资源、协同执行 AI 推理、构建统一但可控的个人记忆空间，并在设备间增量同步认知状态。

## Why

- 个人数据与认知状态跨设备统一，但默认本地处理、隐私优先
- 无云或弱联网场景下保持基础能力
- 以可解释、可复现、可 Benchmark 的方式验证"多设备协同智能"这一核心假设

## Architecture（八层）

| Layer | 名称 | 核心职责 | 技术栈 |
|---|---|---|---|
| L1 | Device Runtime | 节点执行、线程、资源 | C++20 / CMake / CUDA / OpenMP |
| L2 | Inference Engine | 模型加载与推理 | llama.cpp / ggml / GGUF |
| L3 | Distributed Inference | 多节点协同推理 | QUIC / libp2p / Synapse Protocol |
| L4 | Memory Fabric | 个人记忆（语义+时间+空间+关系） | Neo4j + Vector + Metadata |
| L5 | AI Pipeline | 文档/多模态处理 | Python / FastAPI / PyTorch |
| L6 | Control Plane | 设备、任务、权限、元数据 | NestJS / PostgreSQL / Redis |
| L7 | Event Runtime | 事件、触发、任务流 | Redis Streams |
| L8 | Console | Web 控制台与可视化 | Vue3 / TS / D3 / ECharts |

## Quick Start

Month 1 Local Cognitive Core 已收官（解析→切分→向量化→检索→排序→持久化）。
Month 2 网络层：TCP 回显 + 同步协议 v1 + 同步上 TCP 已落地；
Memory Benchmark（Vector RAG vs Synapse Memory）已通过验收。
Month 3 C++ Runtime：CMake + 测试框架 + Task/Queue 已落地，
多线程 Task Runtime（Worker + Executor）已实现并验证通过。

```text
本地核心（已完成 2026-10-14）：
PDF → Parser → Chunker → Embedding → Retrieval → Memory Score → Memory Store → LLM

网络层（2026-10-15~18）：
设备A ←— TCP 回显 —→ 设备B        （跨进程传字节）
设备A ←— 同步协议v1 —→ 设备B      （长度前缀帧 + 增量差集 + 后写者胜）
设备A ←— 同步上TCP —→ 设备B      （7 步对账时序 + ACK 确认 + 断线重试）

记忆层验收（2026-10-20）：
Vector RAG ←— Benchmark —→ Synapse Memory（Temporal Retrieval + 5 指标对比）

C++ Runtime（2026-10-21~22，Month 3）：
CMake 工程 + 自研测试框架 → Task/Queue → Worker/Executor 线程池
（多线程 Task Runtime 已实现，14 C++ 单测全过）
```

运行：

```bash
pip install -r requirements.txt
python -m pytest tests/unit -q          # 单元测试（168 个）
python memory/parser/pdf_parser.py <path-to-pdf>   # 命令行提取
python memory/parser/chunker.py <path-to-pdf> 512 64   # 提取 + 切分
python memory/embedding/embedder.py '文本A' '文本B'     # 向量化 + 相似度
python memory/retrieval/vector_store.py '问题' <path-to-pdf> 5   # 索引 + 检索
python memory/activation/memory_score.py                 # 综合排序演示
python memory/graph/graph.py                             # 图谱演示
python memory/pipeline.py <path-to-pdf> '问题'          # 一键导入 + 检索（持久化）

# 网络回显（两个终端）：
python -m net.echo_server --port 9000                   # 终端1：服务端
python -m net.echo_client "你好" --port 9000            # 终端2：客户端

# 同步协议（进程内演示）：
python -m net.sync                                       # 双向差集同步演示

# 同步上 TCP（两个终端，真实网络同步）：
python -m net.sync_server --port 9001 --store data/device_b.json   # 终端1：服务端（设备B）
python -m net.sync_client --port 9001 --store data/device_a.json  # 终端2：客户端（设备A）

# Memory Benchmark（Vector RAG vs Synapse Memory，Month 2 验收）：
python benchmarks/memory/run_benchmark.py

# C++ Runtime（Month 3；需 MSVC + CMake）：
cmd /c build_runtime.bat                  # 一键：vcvars64 + configure + build
runtime/build/Release/test_queue.exe      # 单测：线程安全队列（5 用例）
runtime/build/Release/test_task.exe       # 单测：Task（4 用例）
runtime/build/Release/test_executor.exe   # 单测：Executor 线程池（5 用例）
runtime/build/Release/test_telemetry.exe  # 单测：Telemetry 深化（12 用例）
runtime/build/Release/test_priority.exe   # 单测：优先级调度（5 用例）
runtime/build/Release/demo_threadpool.exe # 演示：优先级插队 + 并发执行 + 统计
```

## Benchmark

- Inference: TTFT / Token/s / VRAM
- Memory: **Vector RAG vs Synapse Memory**（`benchmarks/memory/run_benchmark.py`，Recall/Precision/MRR/Top-1，详见 `docs/research/memory-benchmark.md`）
- Scheduler: Throughput / P95 Latency / Task Completion Rate
- Sync: Sync Time / Transferred Bytes / Conflict Count

## Roadmap

12 个月路线，见规格书第 24 章。当前进度：

- [x] 2026-10-07 初始化仓库与目录骨架
- [x] 2026-10-07 Month 1 环节 1/6：PDF 文本提取（parser + 11 单测 + 端到端样例）
- [x] 2026-10-08 Month 1 环节 2/6：Chunker 文本切分（固定长度+重叠，20 单测）
- [x] 2026-10-09 Month 1 环节 3/6：Embedding 向量化（手写 TF-IDF + 余弦，19 单测）
- [x] 2026-10-12 Month 1 环节 4/6：Retrieval 检索（暴力扫描+堆取 top-k，14 单测）
- [x] 2026-10-13 Month 1 环节 5/6：Memory Score 与图谱（衰减+加权+邻接表，44 单测）
- [x] 2026-10-14 Month 1 环节 6/6：Pipeline 编排 + Memory 持久化（JSON 双落盘，21 单测）
- [x] Month 1 Local Cognitive Core 完成（解析→切分→向量化→检索→排序→持久化，129 单测全过）
- [x] 2026-10-15 Month 2 环节 1：TCP 回显链路（echo server/client + 6 单测，135 全过）
- [x] 2026-10-16 Month 2 环节 2：同步协议 v1（长度前缀帧 + 增量差集 + 冲突处理，21 单测，156 全过）
- [x] 2026-10-18 Month 2 环节 3：同步上 TCP（7 步对账时序 + ACK 确认 + 断线重试，5 单测，161 全过）
- [x] 2026-10-20 Month 2 验收：Temporal Retrieval + Vector RAG vs Synapse Memory Benchmark（since 时间窗 + 5 指标对比，7 单测，168 全过）
- [x] Month 2：网络基础 + P2P 同步协议 + Memory Benchmark 验收完成
- [x] 2026-10-21 Month 3 环节 1：C++ Runtime 起步（CMake 工程 + 自研测试框架 + Task/线程安全 Queue + demo，9 单测全过，构建脚本可复现）
- [x] 2026-10-22 Month 3 环节 2：Worker + Executor 实现（多线程 Task Runtime 核心：优雅停机/幂等 join/RAII 析构，+5 单测，14 全过，CTest 3/3）
- [x] 2026-10-22 Month 3 环节 3：Telemetry 深化（per-worker 独立计数 + 拒绝计数 + 耗时统计，+7 单测，21 全过，CTest 4/4）
- [x] 2026-10-22 Month 3 环节 4：队列水位峰值采样（CAS 无锁 max，背压设计依据，+5 单测，26 全过）
- [x] 2026-10-23 Month 3 环节 5：优先级调度（deque → priority_queue + seq 稳定排序，408 优先级调度 PS，+5 单测，31 全过，CTest 5/5）
- [x] 2026-10-25 Month 3 环节 6：老化防饥饿（multiset 全量扫描 + boost 封顶，408 MLFQ 防饿死，+5 单测，36 全过）
- [x] 2026-10-27 Month 3 环节 7：背压（有界队列 + 满则拒绝，408 生产者-消费者有限缓冲，+4 单测，40 全过）
- [x] 2026-10-27 Month 3 环节 8：阻塞式背压（push_wait 满则等 + 超时，双条件变量 = empty/full 信号量，+6 单测，46 全过）
- [ ] Month 3：动态扩容（峰值超阈值自动加 worker）

## 学习执行模式

```text
今日 Milestone → 知识缺口 → 理论学习 → 最小实验 → 接入项目 → 测试 → 记录 → 提交
```
