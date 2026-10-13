# Research Question：检索排序只靠相似度够吗？（Month 1 环节 5/6）

## Problem

retrieval 环节已实现"按向量相似度排序"，但规格书场景 B 暴露了一个盲区：
用户问"我之前为什么决定学 Agent"，系统找到所有提到 Agent 的 chunk，
但无法区分"上周记的"和"去年记的"——个人记忆里新旧本身就是相关性的组成部分。

同样，两条记忆语义完全相同，一条被频繁访问、一条从未被碰过——
检索时应优先给出活跃的那条。纯相似度排序回答不了这些。

核心问题：**排序除了语义相似度，还该融入哪些维度？怎么融合？**

## Hypothesis

采用规格书 9.4 的 Memory Score 加权模型：
```
MemoryScore = a*Semantic + b*Recency + c*Importance + d*Activation + e*GraphRelevance
```
- 权重是超参数，V1 给合理起点，靠 Benchmark 调优（规格书明确不把参数当"科学真理"）
- 各分量归一化到 [0,1] 后加权求和，总分在 [0,1] 可比较
- 时间维度用指数衰减（规格书 9.5）：activation(t) = a0 * exp(-lambda * dt)
- 图维度用最小图谱（规格书第 10 章）：实体关系让"看似无关"的记忆连起来

## Background

**为什么指数衰减**：
- 人的记忆随时间指数淡忘（艾宾浩斯遗忘曲线）
- 数学性质好：dt=0 时满值、单调递减、永不为负、有明确的半衰期（T = ln2/lambda）
- 半衰期是直观的超参数："60 天半衰期"比"lambda=1.3e-7"好理解得多

**为什么需要图**：
- 向量检索度量"词面/语义相似"，图度量"实体关系"
- "408" 和 "南邮" 没有共同词，向量相似度≈0；但在图里都连着"考研目标"，
  二跳可达——"为什么考南邮"这个问题，图能给出向量给不出的答案

## Experiment

实现三个模块：
1. `memory/activation/decay.py`：exponential_decay / half_life / reinforce
   （强化封顶 1.0，对应"常访问记得牢"）
2. `memory/activation/memory_score.py`：MemoryScoreWeights（权重注入，默认
   语义0.5/新近0.2/激活0.15/重要0.1/图0.05）+ compute（纯函数）+ rank
3. `memory/graph/graph.py`：MemoryGraph 邻接表，neighbors（一阶）/
   two_hop（BFS 深度2）/ graph_relevance（度/最大度归一化）

单元测试 44 个：衰减边界与单调性、半衰期换算、强化封顶、
权重校验、分量越界、rank 降序、图边幂等性、二跳正确性、归一化。

## Result

- `python -m pytest tests/unit -q`：**108 passed**（累计 64 + 44）
- 端到端演示（半衰期 60 天）：
  - 语义相似度同为 0.62 的两条记忆：
    7 天前 -> total 0.683（新近 0.922），200 天前 -> total 0.395（新近 0.099）
    ——新记忆正确胜出，且差距主要来自新近度/激活度分量
  - 图谱："408" 二跳可达 {考研目标, C++, 南邮, Agent项目}，
    "南邮" 与 "408" 通过 "考研目标" 连通——向量检索做不到的关联，图做到了

## Analysis

- 加权融合在"正确性"上达标：可解释（components 保留各分量）、可复现、可测试
- **已知边界（技术债）**：
  1. graph_relevance 用"节点自身度/最大度"，严格说应算"与查询实体的距离"——
     V2 改 BFS 最短路径倒数
  2. 图无关系类型（RELATED_TO/MENTIONS/PART_OF 统一按"有边"）、无节点属性、
     无带权边——规格书 10.2/10.3 的丰富关系是 V2
  3. 权重是拍脑袋的起点，未 Benchmark——规格书 20.2 Memory Benchmark
     正是为此设计（对比 Naive RAG / Vector RAG / Graph RAG / Synapse Memory）
  4. Importance 在 V1 无来源（全默认 0.5），需要文档类型/用户标记驱动
- 指数衰减的参数敏感性：半衰期 60 天 vs 30 天，200 天记忆的激活度差距明显，
  参数选择本身该进 Benchmark

## Conclusion

Memory Score 环节 V1 达标：可运行代码 + 44 单测 + 端到端新旧排序验证。
排序从"一维相似度"升级为"五维加权"，且每个维度可解释、每个权重可调。
图结构的最简形态已落地，为"图检索补足向量盲区"提供了实现基础。

## Next Question

检索、排序都通了，但整条链路还没有"服务化"：PDF 进来后
Parser→Chunker→Embedding→Retrieval 是散落的命令行调用，
没有统一的编排入口，也没有持久化。怎么把它们串成一个可重复调用的
pipeline，并落库？——Month 1 环节 6/6：Pipeline 编排 + Memory 持久化。
