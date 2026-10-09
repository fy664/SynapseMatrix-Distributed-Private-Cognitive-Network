# Research Question：文本如何变成向量？TF-IDF 够用吗？（Month 1 环节 3/6）

## Problem

chunker 产出的 chunk 是纯文本，无法直接检索。检索需要"度量两个文本的语义接近程度"，
而计算机只能比较数字。核心问题：
- 文本如何表示成向量？
- 用什么度量"相似"？
- V1 用什么实现，为什么？

## Hypothesis

V1 用手写 TF-IDF + 余弦相似度作为"文本向量化"的最小实现：
- 零模型依赖（jieba 分词 + 纯算法），完全可复现、可单测
- 把"文本 -> 向量 -> 检索"链路先跑通，接口对上层稳定
- 语义模型（BGE）作为 V2 替换点，检索层零改动

## Background

文本向量化的演进：
1. **One-hot**：每个词一维，维度=词典大小，任意两向量正交，相似度恒 0——无意义
2. **TF-IDF**：词频 × 逆文档频率加权。词在本文档频繁（TF 高）且在全语料罕见（IDF 高）
   → 权重高。"的/了/是"这类到处出现的词 IDF 趋近 0，自动被压低。
   本质：**词面重合度**。缺点：不理解语义，"苹果"与"水果"不同词 → 相似度为 0
3. **语义 embedding（Word2Vec/BERT/BGE）**：神经网络学到的稠密向量，
   语义相近的词在向量空间中距离近。理解"同义不同词"。代价：模型下载、算力、不可复现性

**为什么用余弦相似度而不是欧氏距离**：
- 余弦只比方向（各维度的相对比例），不比长度
- 同一语义的文本，一个 100 词一个 20 词，向量"方向"接近，余弦高
- 欧氏距离受向量长度干扰：长文本向量范数大，距离天然大，不适合语义比较

## Experiment

实现 `memory/embedding/tfidf.py` + `embedder.py`：
- `tokenize`：中文 jieba 分词、英文小写、过滤纯标点 token
- `TfidfVectorizer`：fit（建词典 + 算 IDF）→ transform（稀疏向量 {词: 权重}）
  - TF = 词频/总词数（归一化，避免长文本天然权重高）
  - IDF = log(N/df) + 1（+1 保证权重非负）
- `cosine_similarity(a, b)`：稀疏向量点积 / 范数积
- `Embedder`：统一接口（fit/encode/encode_all/similarity），V2 换后端只改内部

单元测试 19 个：分词（中英/空/标点）、IDF 性质（罕见词权重高）、
OOV 词忽略、向量长度归一化、相同文本=1、无关文本低分、余弦方向性、空向量=0、接口行为。

## Result

- `python -m pytest tests/unit -q`：**50 passed**（parser 11 + chunker 20 + embedding 19）
- 端到端：规格书 PDF → 27 chunk → 227 维稀疏向量；
  query "SynapseMatrix 是一个端侧优先 P2P 协同的系统"
  → top1 相似度 0.419（项目定位段落）、top2 0.328（Edge-first 描述）、
  top7 0.169（"不是堆技术名词"段落），排序符合语义直觉
- 踩坑 2 个（均为分词细节）：
  1. jieba 会把标点也切成 token，需过滤纯标点（否则词表被 "，" 污染）
  2. jieba 对"天气晴朗"等词按整体切，测试期望需与分词行为对齐

## Analysis

- TF-IDF 在"词面重合"检索上达标：query 与规格书开头段落大量共用词 → 高分，排序合理
- **已知边界（技术债）**：
  1. 无语义理解：同义不同词相似度为 0（"苹果" vs "水果"）——这是换 BGE 的根本动机
  2. 无停用词过滤：标点虽已过滤，"的/是/了"等高频虚词仍会拉高相似度
  3. OOV 新词直接忽略：fit 后出现的新词无向量，真实系统需增量更新词表
  4. 无 sublinear TF / BM25 变体（长文档词频饱和问题）
- 块长与检索质量的关系要等 Memory Benchmark（规格书 20.2）用 Recall/Precision 验证，
  目前 200~512 字符是常见起点

## Conclusion

Embedding 环节 V1 达标：可运行代码 + 19 单测 + 端到端检索验证。
TF-IDF 作为基线是有价值的——它量化了"词面检索"的天花板，
将来用 BGE 做 Benchmark 对比时，"提升多少"就有了可测量的基线。

## Next Question

V2 语义模型选型：BGE-small-zh 与 text2vec、OpenAI embedding 的取舍
（离线/中文/维度/显存），以及稀疏向量检索（倒排索引）vs 稠密向量检索（ANN）。
即：先做 Naive RAG 还是直接上向量数据库？——Month 1 环节 4/6：Retrieval。
