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

Month 1 已落地环节：PDF 文本提取 + 文本切分 + 向量化。

```text
PDF → Parser → Chunker → Embedding → Graph + Vector → LLM
      ↑ 已完成    ↑ 已完成    ↑ 已完成（2026-10-09）
```

运行：

```bash
pip install -r requirements.txt
python -m pytest tests/unit -q          # 单元测试（50 个）
python memory/parser/pdf_parser.py <path-to-pdf>   # 命令行提取
python memory/parser/chunker.py <path-to-pdf> 512 64   # 提取 + 切分
python memory/embedding/embedder.py '文本A' '文本B'     # 向量化 + 相似度
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
- [ ] Month 1 环节 4/6：Retrieval 检索
- [ ] Month 1 Local Cognitive Core 完成（Embedding → Graph+Vector → LLM 引用记忆）

## 学习执行模式

```text
今日 Milestone → 知识缺口 → 理论学习 → 最小实验 → 接入项目 → 测试 → 记录 → 提交
```
