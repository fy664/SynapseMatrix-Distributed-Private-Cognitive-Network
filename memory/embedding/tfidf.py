"""
SynapseMatrix — Memory Embedding 层：TF-IDF 向量化（Month 1 环节 3/6 最小实现）
===============================================================================

定位：承接 chunker 的输出，对应规格书第 11 章 Pipeline 的
"Chunker -> Embedding -> ..." 中的 Embedding。

本模块职责（V1 范围）：
    1. 把文本切分为词（中文用 jieba 分词，英文/数字按空白切）
    2. 计算 TF（词频）与 IDF（逆文档频率）
    3. 把每个文本表示为 TF-IDF 稀疏向量
    4. 提供余弦相似度计算（向量检索的核心度量）

设计决策（为什么 V1 用手写 TF-IDF，而不是直接上 sentence-transformers）：
    - 规格书 0.2 核心原则：先手写最小实现，再引入成熟框架；先正确，后性能。
    - TF-IDF 是"文本向量化"的最基础模型：零模型下载、纯算法、可复现、
      可单测，能把"文本 -> 向量 -> 相似度"这条链路完整跑通。
    - 语义模型（BGE 等）作为 V2 增强：TF-IDF 只捕捉词面重合，
      无法理解"同义不同词"（如"苹果"与"水果"），这正是研究记录里
      要回答的"为什么需要语义 embedding"的对照基线。
    - 检索流程不变：无论 TF-IDF 还是语义向量，都是"向量 + 余弦相似度"，
      因此本模块的接口（encode / cosine_similarity）对上层稳定。

V1 简化（技术债，记入 TECH-DEBT）：
    - 不做停用词过滤（"的/了/是"等高频无意义词会拉高相似度）
    - 不做 TF-IDF 的平滑变体（sublinear TF、BM25 等）
    - 稀疏向量用 dict 表示（{词: 权重}），文本量小足够；量大会换稠密数组

依赖：jieba（中文分词，纯 Python 实现）
"""

from __future__ import annotations

import math
import re
from collections import Counter

import jieba

# 英文/数字按空白与标点切分；中文交给 jieba
_TOKEN_PATTERN = re.compile(r"[a-zA-Z0-9]+")


# ---------------------------------------------------------------------------
# 分词工具
# ---------------------------------------------------------------------------

def tokenize(text: str) -> list[str]:
    """把一段文本切成词序列（V1 简单策略）。

    - 对每个中文字符串片段用 jieba.cut 精确模式分词
    - 对英文/数字片段用正则按连续字母数字切
    - 全部转小写，保证 "PDF" 与 "pdf" 视为同一词

    参数：
        text: 输入文本（可为空）

    返回：
        list[str]：词序列（可重复，供 TF 统计）

    例：
        tokenize("Hello 世界，SynapseMatrix 技术规格书")
        -> ["hello", "世界", "synapsematrix", "技术", "规格书"]
    """
    if not text:
        return []

    tokens: list[str] = []
    # 先按空白/标点把中英混排拆开，避免 jieba 把英文单词切碎
    for part in _TOKEN_PATTERN.split(text):
        # 非英文/数字部分（含中文）交给 jieba
        if part.strip():
            for tok in jieba.cut(part.strip()):
                tok = tok.strip()
                # 过滤纯标点 token（如 "，"、"！"）：jieba 会把标点也切出来，
                # 但标点没有语义信息，混入词表会污染 TF-IDF 权重
                if tok and _is_valid_token(tok):
                    tokens.append(tok)
    # 英文/数字部分单独切分
    tokens.extend(m.lower() for m in _TOKEN_PATTERN.findall(text))
    return tokens


def _is_valid_token(tok: str) -> bool:
    """判断 token 是否有语义信息：含至少一个中文字符或字母数字。

    用于过滤 jieba 切出的纯标点 token（"，"、"！"、"。"等）。
    中文字符范围 \u4e00-\u9fff（CJK 统一表意文字）。
    """
    return any("\u4e00" <= ch <= "\u9fff" for ch in tok) or bool(_TOKEN_PATTERN.search(tok))


# ---------------------------------------------------------------------------
# TF-IDF 向量化器
# ---------------------------------------------------------------------------

class TfidfVectorizer:
    """手写 TF-IDF 向量化器（fit + transform 两阶段，风格对齐 sklearn）。

    原理：
        TF（词频）        = 词在本文档中出现次数 / 本文档总词数
        IDF（逆文档频率）  = log(N / df) + 1
                            其中 N = 文档总数，df = 含该词的文档数
        TF-IDF = TF * IDF
        直观含义：词在本文档中越频繁（TF 高）、在全体文档中越罕见（IDF 高），
        越能代表本文档的主题——"的/了"这类到处都有的词 IDF 趋近 0，
        权重自动被压低。

    用法：
        vec = TfidfVectorizer()
        vec.fit(chunk_texts)          # 建词典 + 算 IDF
        v1 = vec.transform(text_a)    # 得到稀疏向量 {词: 权重}
        score = cosine_similarity(v1, vec.transform(text_b))
    """

    def __init__(self) -> None:
        # 词典：词 -> 词典下标（V1 稀疏表示不需要下标，但保留以对齐后续稠密化）
        self.vocabulary_: dict[str, int] = {}
        # 每个词的 IDF 权重（fit 后可用）
        self.idf_: dict[str, float] = {}
        # 文档总数（fit 时传入的文本数）
        self.n_docs_: int = 0

    def fit(self, texts: list[str]) -> "TfidfVectorizer":
        """在文档集合上学习词典与 IDF。

        参数：
            texts: 文档文本列表（如全部 chunk 的文本）

        返回：
            self（支持链式调用）

        步骤：
            1. 统计每篇文档的词频（用于判断"哪些文档含该词"）
            2. 对每个出现过的词计算 df（含该词的文档数）
            3. 计算 idf = log(n_docs / df) + 1（+1 保证权重为正，防止 log 出现负数）
        """
        self.n_docs_ = len(texts)

        # 统计每个词的文档频率 df（含该词的文档数）
        df: Counter[str] = Counter()
        for text in texts:
            # set 去重：df 只关心"哪些文档出现过"，不关心出现几次
            df.update(set(tokenize(text)))

        # 为词典中每个词计算 IDF；V1 不限制词典大小（文本量小）
        self.vocabulary_ = {word: i for i, word in enumerate(sorted(df))}
        self.idf_ = {
            word: math.log(self.n_docs_ / count) + 1.0
            for word, count in df.items()
        }
        return self

    def transform(self, text: str) -> dict[str, float]:
        """把单篇文本转为 TF-IDF 稀疏向量。

        参数：
            text: 输入文本

        返回：
            dict[str, float]：{词: TF-IDF 权重}，仅含出现在词典中的词

        说明：
            - 不在词典中的词（fit 时没见过）直接忽略——这是 V1 简化，
              真实系统会对新词做增量 IDF 更新（技术债）
            - 空文本返回空向量（与任何向量相似度都应为 0）
        """
        if not text:
            return {}

        tokens = tokenize(text)
        if not tokens:
            return {}

        # TF：词频 / 总词数（归一化的词频，避免长文本天然权重高）
        tf_counter = Counter(tokens)
        total = len(tokens)
        tf = {word: count / total for word, count in tf_counter.items()}

        # 只保留词典内词，乘上 IDF
        vector: dict[str, float] = {}
        for word, tf_val in tf.items():
            if word in self.idf_:
                vector[word] = tf_val * self.idf_[word]
        return vector

    def transform_all(self, texts: list[str]) -> list[dict[str, float]]:
        """批量转换（列表推导，V1 顺序执行）。"""
        return [self.transform(t) for t in texts]


# ---------------------------------------------------------------------------
# 相似度
# ---------------------------------------------------------------------------

def cosine_similarity(a: dict[str, float], b: dict[str, float]) -> float:
    """两个稀疏向量的余弦相似度，取值 [-1, 1]。

    公式：cos = (a·b) / (|a| * |b|)
          a·b   = 各维度权重乘积之和
          |a|   = sqrt(Σ a_i²)

    为什么用余弦而不是欧氏距离（重点概念）：
        - 余弦只比较"方向"（各维度的相对比例），不比较"长度"。
        - 同一语义的文本即使长度差很多（一个 100 词、一个 20 词），
          向量方向接近，余弦就高——这正是检索想要的。
        - 欧氏距离会被向量长度干扰：长文本的向量"更长"，距离天然更大，
          不适合直接比较语义。

    参数：
        a, b: transform() 产出的稀疏向量

    返回：
        相似度；任一为空向量时返回 0.0（无共同信息，不算相似）
    """
    if not a or not b:
        return 0.0

    # 取两向量中更短的一个做遍历（词典相同但命中词不同，遍历少的那边更省）
    if len(a) > len(b):
        a, b = b, a

    dot = 0.0
    for word, weight in a.items():
        if word in b:
            dot += weight * b[word]

    # 两向量长度（L2 范数）
    norm_a = math.sqrt(sum(w * w for w in a.values()))
    norm_b = math.sqrt(sum(w * w for w in b.values()))

    if norm_a == 0.0 or norm_b == 0.0:
        return 0.0

    return dot / (norm_a * norm_b)


# ---------------------------------------------------------------------------
# 命令行演示
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import sys

    if len(sys.argv) < 3:
        print("用法: python tfidf.py <文本A> <文本B> [更多文本...]")
        print("示例: python tfidf.py '苹果很好吃' '我喜欢吃苹果' '今天天气晴朗'")
        sys.exit(1)

    texts = sys.argv[1:]
    vec = TfidfVectorizer().fit(texts)
    vectors = vec.transform_all(texts)

    for i, text in enumerate(texts):
        print(f"文本{i+1}: {text}")
        print(f"  向量({len(vectors[i])}维): {dict(list(vectors[i].items())[:8])}")
    print(f"\n相似度矩阵:")
    for i in range(len(texts)):
        row = []
        for j in range(len(texts)):
            row.append(f"{cosine_similarity(vectors[i], vectors[j]):.3f}")
        print(f"  {'  '.join(row)}")
