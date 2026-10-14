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
Month 2 网络层起步：TCP 回显链路已通。

```text
本地核心（已完成 2026-10-14）：
PDF → Parser → Chunker → Embedding → Retrieval → Memory Score → Memory Store → LLM

网络层（起步 2026-10-15）：
设备A ←— TCP 回显 —→ 设备B   （跨进程传数据的最小路径）
```

运行：

```bash
pip install -r requirements.txt
python -m pytest tests/unit -q          # 单元测试（135 个）
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
```

## Benchmark

（占位：见 docs/benchmark/ 与 benchmarks/）

- Inference: TTFT / Token/s / VRAM
- Memory: Recall / Precision / Retrieval Latency
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
- [ ] Month 2 环节 2：同步协议 v1（帧格式 + 增量同步 + 冲突处理）
- [ ] Month 2：网络基础 + P2P 同步协议起步

## 学习执行模式

```text
今日 Milestone → 知识缺口 → 理论学习 → 最小实验 → 接入项目 → 测试 → 记录 → 提交
```
