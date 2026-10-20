# Research Question：Vector RAG vs Synapse Memory 谁更该被信任？（Month 2 验收）

## Problem

规格书 Month 2 的验收项：给定相同问题，能做「Vector RAG vs
Synapse Memory」的 Benchmark。此前记忆层各组件（检索/衰减/图谱/
打分）各自有单测，但**没有整体对比**——不知道"加了 Memory Score
到底值不值"，时间维度（Temporal Retrieval）也只是理论，没有量化。

## Hypothesis

- 纯向量 RAG（TF-IDF 余弦）只比语义相似度，没有时间概念
- Synapse Memory（五维 Memory Score）多了新近度/激活/重要/图相关
- 关键预测：**刚导入时两套排序几乎相同**（时间维度未激活，
  recency 全是 1.0，排序由 semantic 主导）；**人为制造新旧差异后，
  Synapse Memory 应显著偏向新记忆**，总体检索质量不劣于 RAG

## Background

**Temporal Retrieval（时间感知检索）的两层语义**：
1. 过滤层：`since` 时间窗——硬性排除窗口外记忆（"只查最近 N 天"）
2. 排序层：recency 衰减——Memory Score 的新近度分量按 created_at
   指数衰减，新记忆天然压过旧记忆

**为什么"刚导入时两套相同"不是 bug 而是特性**：
recency 由 created_at 与"现在"的差值衰减而来。刚导入时所有记忆
一样新（recency ≈ 1.0），五维里四个维度被中和，排序退化为纯语义。
只有时间拉开差距时，Memory Score 才开始体现价值——这正是
"时间感知"的全部意义：**没有时间差，就没有时间可感知**。

**Benchmark 方法**：
- 语料：完整规格书转 PDF（66 页，173 chunk），走真实链路
  PDF -> parser -> chunker -> TF-IDF -> 检索
- 问题集：12 问，每问用规格书独有概念词定位 ground truth chunk
- 指标：Recall@k（覆盖度）、Precision@k（精确度）、MRR（第一个
  相关 chunk 的位置倒数）、Top-1 命中率
- 场景 B：随机把一半 chunk 的 created_at 改成 30 天前，制造
  "新旧冲突"——同一主题同时存在新老记忆

## Experiment

1. `memory/pipeline.py`：`search` 增加 `since` 参数（时间窗过滤，
   对两套排序模式都生效），并优化为一次构建 chunk_id->record 映射
2. `benchmarks/memory/spec_full.pdf`：由完整规格书 MD 生成的语料
   （66 页，173 chunk）
3. `benchmarks/memory/run_benchmark.py`：可复现的对比脚本
   - 场景 A：12 问总体质量
   - 场景 B：8 问（新旧都有的子集）新旧冲突
4. `tests/unit/test_temporal_retrieval.py`：since 过滤 / recency
   排序 / 指标函数共 7 个单测

## Result

`python -m pytest tests/unit -q`：**168 passed**（累计 161 + 7）

**场景 A（12 问，刚导入，无时间差异）**：
| 指标 | Vector RAG | Synapse Memory |
|---|---|---|
| Recall@5 | 0.518 | 0.518 |
| Precision@5 | 0.367 | 0.367 |
| MRR | 0.590 | 0.590 |
| Top-1 | 0.417 | 0.417 |

两套完全一致——验证假设：时间维度未激活时，Memory Score 退化为
纯向量排序。

**场景 B（8 问，一半记忆 30 天前，新旧冲突）**：
| 指标 | Vector RAG | Synapse Memory |
|---|---|---|
| Recall@5 | 0.465 | 0.482 |
| Precision@5 | 0.450 | 0.475 |
| MRR | 0.635 | 0.656 |
| Top-1 | 0.500 | 0.500 |

Synapse Memory 在 Recall/Precision/MRR 上全面小幅领先。

**Top-1 返回「新记忆」的问题占比（时间感知的直接证据）**：
- Vector RAG：4/12（无时间概念，新旧各半≈随机）
- Synapse Memory：9/12（明确偏向新记忆）

## Analysis

- **时间维度真实生效**：场景 B 里 Synapse Memory 把新记忆排到
  Top-1 的概率从 33%（4/12）提到 75%（9/12），这是 recency
  衰减分量直接驱动的结果，也是 Temporal Retrieval 的可量化价值。
- **总体质量小幅提升**：Recall/Precision/MRR 各提升约 2~3 个百分点，
  说明"偏向新记忆"没有牺牲覆盖度——新记忆压过旧记忆的同时，
  相关记忆仍被召回。
- **当前 benchmark 的局限（诚实记录）**：
  1. ground truth 用关键词定位（chunk 包含概念词即相关），不是
     人工标注的相关性——Recall 数值绝对意义有限，相对对比仍有效
  2. 语料是规格书本身（结构化文档），与真实个人记忆（聊天、
     笔记、网页）分布不同
  3. 权重是默认值（0.5/0.2/0.15/0.1/0.05），未做网格调优——
     规格书明确说权重要靠 Benchmark 调，这是下一步
  4. V1 用 TF-IDF 而不是 BGE——语义上限受词面匹配限制，
     同义改写的问题召回会偏弱（V2 换 BGE 后再跑同一套指标）

## Conclusion

Month 2 验收通过：Vector RAG vs Synapse Memory 的可复现 Benchmark
落地。结论：**时间维度是 Synapse Memory 相对纯向量检索的核心增量**——
无时间差时两者等价（合理），有时间差时 Synapse Memory 明确偏向
新记忆（9/12 vs 4/12）且总体质量不降。Temporal Retrieval 的过滤层
（since 时间窗）与排序层（recency 衰减）均有单测与实测支撑。

## Next Question

时间维度验证了，但权重还是拍脑袋的默认值。下一步：
- 权重调优：对 Memory Score 五维权重做网格搜索/简单优化，
  用本 benchmark 的 MRR/Recall 作为目标函数（规格书 9.4 明确要求）
- 或进入 Month 3：C++20 多线程 Task Runtime（Device/Worker/Task/
  Queue/Executor）——408 操作系统与 C++ 的核心考点
