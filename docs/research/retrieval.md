# Research Question：有了向量，检索该怎么做？（Month 1 环节 4/6）

## Problem

embedding 环节产出"文本 -> 向量"，但检索还没打通。核心问题：
- 给定一个 query 向量，如何在大量 chunk 向量中找到最相关的 k 个？
- 全量扫描 vs 索引，V1 该选哪个？
- 检索结果如何带出处返回（页码/原文），让"AI 能引用记忆"成为可能？

## Hypothesis

V1 用"暴力扫描 + 堆取 top-k"：
- 暴力扫描对每个 chunk 算余弦相似度，O(N·D) —— N<1 万时完全够用，且结果精确
- 堆取 top-k 复杂度 O(N log k)，优于全排序 O(N log N)，是"前 k 大"标准最优解
- 检索接口（add/search）对上层稳定，V2 换 ANN 时上层零改动

## Background

检索路径的演进（按规模从小到大）：
1. **暴力扫描（brute force）**：每个 query 都全量算相似度。精确，但 O(N·D)，
   N 到百万级就不可行
2. **倒排索引（inverted index）**：词 -> 文档列表。只适用于稀疏向量（TF-IDF），
   查"含某词的文档"极快，但语义检索（稠密向量）用不了
3. **ANN（近似最近邻，faiss/hnswlib）**：牺牲少量召回换数量级提速，
   适用于稠密语义向量，N 到亿级的标准方案

**为什么用堆选 top-k（本题核心算法）**：
全排序 O(N log N) 浪费——我们只要前 k 个，不需要全序。
维护一个大小 k 的小顶堆（堆顶是最小元素）：
- 堆未满：push
- 堆满且新分数 > 堆顶：pop 堆顶 + push 新分数（heapreplace 一步完成）
结束时堆里恰好是最大的 k 个，总代价 O(N log k)。
这就是 LeetCode 215（数组第 K 大）的同款思路：k 小时（如 5~10）log k 几乎可忽略，
复杂度实际趋近 O(N)。

**阈值过滤（min_score）**：余弦相似度 0.2 的两个文本可能只共享 1 个词，
算不上"相关"。检索必须设置最低分，把噪声排除——否则 system 会把垃圾当记忆引用。

## Experiment

实现 `memory/retrieval/vector_store.py`：
- `VectorStore`：内存索引，`add`/`add_batch`/`search`/`__len__`
- `search(query_vector, top_k)`：全量余弦 + 小顶堆选 top-k + 阈值过滤
- `RetrievalHit`：chunk_id / page_no / score / text（带出处）

单元测试 14 个：命中正确性、top-k 数量边界、降序、阈值过滤、
空索引、空 query、非法 top_k、批量长度校验、真实链路组合。

## Result

- `python -m pytest tests/unit -q`：**64 passed**（parser 11 + chunker 20 + embedding 19 + retrieval 14）
- 端到端（规格书样例 PDF，27 chunk）：
  - query "这个项目是什么定位" → 第 1 页 chunk#1（项目定位段落，score 0.320）
  - query "什么是记忆衰减" → 无高分命中：样例 PDF 只含规格书前 120 行，
    第 9 章记忆衰减不在索引里 —— 阈值过滤正确返回空/低分，未吐噪声
  - 这恰好验证了 min_score 的价值：内容不存在时不伪造结果
- 注意：本机 Python 环境被重置过（sandbox base 更换），pytest/jieba/PyMuPDF 需重装

## Analysis

- 暴力扫描 + 堆在"正确性"上达标：命中准确、排序严格降序、阈值生效
- **已知边界（技术债）**：
  1. 索引只存内存，进程退出即丢失——V2 需持久化（向量数据库/文件）
  2. 不支持删除/更新——V2 加 chunk_id 级操作
  3. 无多字段过滤（时间/来源）——V2 配合 metadata 加过滤条件
  4. N 到万级后暴力扫描变慢——V2 换 ANN
- 稀疏向量（TF-IDF）用倒排索引其实更高效，但为了 V2 平滑迁移到
  稠密语义向量，统一走"全量扫描"接口，将来直接换 faiss

## Conclusion

Retrieval 环节 V1 达标：可运行代码 + 14 单测 + 端到端命中验证。
"问题 -> 向量 -> 命中 chunk（带页码）"链路已通，这为最终验收项
"Vector Retrieval 成功 / AI 能引用记忆"打下了基础。

## Next Question

检索已返回"最相关的 chunk"，但记忆不止向量一个维度：
规格书 9.1 说每个记忆同时有 Semantic / Temporal / Spatial / Relational /
Importance / Activation——怎么把时间衰减、图关系融入排序？
即 Month 1 环节 5/6：Memory Score 与图谱（Graph）。
