# Research Question：文本应该如何切分（chunking）？（Month 1 环节 2/6）

## Problem

PDF 提取出的整页文本不能直接做 embedding / 检索：
- embedding 模型有固定上下文窗口（如 512 token），超长会被截断
- 检索粒度太粗：问"某个具体结论"时，整页向量和问题向量相似度低
- 需要先把文本切成信息单元（chunk），再逐个 embedding，检索时按 chunk 命中

核心问题：**用什么规则切？切多长？切断了怎么办？**

## Hypothesis

V1 用"固定长度 + 重叠窗口"切分，理由是：
- 确定性：同样输入永远同样输出，可测试、可复现（符合项目"可解释、可复现"原则）
- 可 Benchmark：块长/重叠率是显式参数，之后可以对比不同配置的检索质量
- 语义切分（按句子/段落/主题）依赖 NLP 规则或模型，正确性难以即时验证，
  属于 Month 1 之后的方向（规格书 9.5 扩展区）

## Background

chunking 是 RAG 的经典前置环节，三种主流策略：
1. **固定长度**：按字符/token 数硬切。简单、确定，但可能把语义从中间切断
2. **结构感知**：按段落/标题/句子边界切。保留语义完整性，但依赖文档结构
3. **语义切分**：用 embedding 相似度找"话题转折点"再切。最智能，但最贵、最不可预测

**重叠窗口（overlap）**的动机：语义单元（段落）如果恰好跨在 chunk 边界上，
检索时无论命中哪一半都搜不全。让相邻 chunk 共享 overlap 个字符，切断处的
信息在前后两个 chunk 里都保留，检索召回率更高。代价是存储和计算量略增。

## Experiment

实现 `memory/parser/chunker.py`：
- `chunk_text(text, chunk_size, overlap)`：纯函数，滑动窗口切分
  step = chunk_size - overlap，每次取 [start, start+chunk_size)，start += step
- `chunk_pages(pages, chunk_size, overlap)`：按页批量切分，chunk_id 全局递增
- 边界处理：空文本→[]；文本≤块长→整体一个 chunk；末块不足→取剩余全部
- 参数校验：chunk_size≥1，0≤overlap<chunk_size（否则 step≤0 死循环）

单元测试 20 个（纯函数 12 + 批量接口 8）：空、短、恰等、超长、重叠正确性、
覆盖性、中文、非法参数、跨页编号、空页跳过、统计字段。

## Result

- `python -m pytest tests/unit -q`：**31 passed**（parser 11 + chunker 20）
- 端到端（规格书样例 PDF，27 页，1136 字符）：
  - chunk_size=256, overlap=32 → 27 chunk（每页 < 256，页内只出 1 个，重叠未触发）
  - chunk_size=20, overlap=5 → 81 chunk，总字符 1406（源 1136 + 重叠 270），
    同页相邻 chunk 重叠正确的 54 处全部通过
- 修正了 2 个测试期望值错误（字符偏移数错），实现逻辑本身无误

## Analysis

- 固定长度 + 重叠在"正确性"上达标：每个 chunk 都是源文本的连续子串，
  无遗漏、无乱序，重叠精确。
- **已知边界（V1 技术债）**：
  1. 按页独立切分，不跨页——跨页段落会在页边界被切断（样例 PDF 页文本
     普遍 < 块长时尤其明显，重叠根本不会触发）。V2 需引入跨页合并或
     按全文连续切分 + 页码区间标注
  2. 纯按字符数切，不感知段落/句子边界——语义完整性无法保证
  3. 无"垃圾 chunk"过滤（如纯空白/纯标点块）
- 块长选择是超参数：256/512 字符是常见起点，最终要用检索 Benchmark
  （Recall/Precision）调优，规格书 20.2 Memory Benchmark 正是为此设计

## Conclusion

chunking 环节已达标：可运行代码 + 20 单测 + 端到端验证。
固定长度 + 重叠是"先正确后性能"原则下的正确 V1 选择，语义切分留待
Memory Benchmark 证明其收益后再引入。

## Next Question

chunk 之后下一步是 embedding：用哪个模型？中文场景下
sentence-transformers（如 BGE 系列）与 OpenAI embedding 的取舍？
即 Month 1 环节 3/6：Embedding。
