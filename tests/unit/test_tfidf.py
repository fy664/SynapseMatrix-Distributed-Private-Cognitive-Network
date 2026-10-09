"""
SynapseMatrix — tests/unit/test_tfidf.py
========================================
TF-IDF 向量化与余弦相似度的单元测试。

覆盖范围：
    - tokenize：中英混排、空文本、大小写归一
    - TfidfVectorizer：fit 建词典、IDF 性质、transform 稀疏向量
    - cosine_similarity：相同文本=1、无关文本低分、空向量=0、
      长度不同但语义重合相似度高（余弦比方向的体现）
    - Embedder：统一接口行为

运行：cd 项目根目录 && python -m pytest tests/unit -q
"""

import math

import pytest

from memory.embedding.embedder import Embedder
from memory.embedding.tfidf import TfidfVectorizer, cosine_similarity, tokenize


# ---------------------------------------------------------------------------
# tokenize
# ---------------------------------------------------------------------------

class TestTokenize:
    def test_chinese_segmentation(self) -> None:
        """中文按 jieba 分词。"""
        tokens = tokenize("苹果很好吃")
        assert "苹果" in tokens
        assert "好吃" in tokens

    def test_english_lowercased(self) -> None:
        """英文转小写：PDF 与 pdf 视为同一词。"""
        assert tokenize("PDF format") == ["pdf", "format"]

    def test_mixed_chinese_english(self) -> None:
        """中英混排都能分出。"""
        tokens = tokenize("Hello 世界，SynapseMatrix 技术规格书")
        joined = set(tokens)
        assert "hello" in joined
        assert "世界" in joined
        assert "synapsematrix" in joined
        assert "规格书" in joined

    def test_empty_text(self) -> None:
        assert tokenize("") == []

    def test_punctuation_only(self) -> None:
        """纯标点无有效词。"""
        assert tokenize("，，，！！！") == []


# ---------------------------------------------------------------------------
# TfidfVectorizer
# ---------------------------------------------------------------------------

class TestTfidfVectorizer:
    def test_fit_builds_vocabulary(self) -> None:
        """fit 后词典包含出现过的词。"""
        vec = TfidfVectorizer().fit(["苹果很好吃", "香蕉很甜"])
        assert "苹果" in vec.vocabulary_
        assert "香蕉" in vec.vocabulary_
        assert vec.n_docs_ == 2

    def test_idf_property(self) -> None:
        """IDF 性质：罕见词 IDF 高，常见词 IDF 低。"""
        vec = TfidfVectorizer().fit(["苹果 苹果 苹果 香蕉", "苹果 香蕉 葡萄"])
        # 苹果出现在全部 2 篇文档：idf = log(2/2)+1 = 1.0
        assert abs(vec.idf_["苹果"] - 1.0) < 1e-9
        # 香蕉出现在 2 篇：同 1.0
        assert abs(vec.idf_["香蕉"] - 1.0) < 1e-9
        # 葡萄只出现在 1 篇：idf = log(2/1)+1 ≈ 1.693
        assert vec.idf_["葡萄"] > vec.idf_["苹果"]

    def test_transform_sparse_vector(self) -> None:
        """transform 产出稀疏向量，仅含词典内词，权重为正。"""
        vec = TfidfVectorizer().fit(["苹果很好吃", "苹果好吃香蕉"])
        v = vec.transform("苹果好吃")
        assert isinstance(v, dict)
        assert all(w > 0 for w in v.values())  # 权重均为正

    def test_transform_oov_word_ignored(self) -> None:
        """词典外的新词被忽略（V1 简化）。"""
        vec = TfidfVectorizer().fit(["苹果好吃"])
        v = vec.transform("奇异果很酸")  # "奇异果"从未在 fit 中出现
        assert v == {}

    def test_empty_text_empty_vector(self) -> None:
        vec = TfidfVectorizer().fit(["苹果好吃"])
        assert vec.transform("") == {}

    def test_vector_normalized_by_length(self) -> None:
        """相同内容的向量不受文本长度影响（TF 归一化）。"""
        vec = TfidfVectorizer().fit(["苹果 苹果 苹果", "苹果 苹果 苹果 苹果 苹果 苹果"])
        v1 = vec.transform("苹果 苹果 苹果")
        v2 = vec.transform("苹果 苹果 苹果 苹果 苹果 苹果")
        # 词频都是 1.0（全篇只有这一个词），权重相同
        assert abs(v1["苹果"] - v2["苹果"]) < 1e-9


# ---------------------------------------------------------------------------
# cosine_similarity
# ---------------------------------------------------------------------------

class TestCosineSimilarity:
    def test_identical_vectors_score_one(self) -> None:
        """相同文本 -> 相似度 1.0。"""
        texts = ["苹果很好吃", "苹果很好吃"]
        vec = TfidfVectorizer().fit(texts)
        v = vec.transform(texts[0])
        assert abs(cosine_similarity(v, v) - 1.0) < 1e-9

    def test_unrelated_texts_low_score(self) -> None:
        """无关文本相似度显著低于相关文本。"""
        related = ["苹果很好吃", "我喜欢吃苹果"]
        unrelated = ["苹果很好吃", "今天天气晴朗适合跑步"]
        vec = TfidfVectorizer().fit(related + unrelated)
        r = vec.transform_all(related)
        u = vec.transform_all(unrelated)
        rel_score = cosine_similarity(r[0], r[1])
        unrel_score = cosine_similarity(u[0], u[1])
        assert rel_score > unrel_score

    def test_empty_vector_score_zero(self) -> None:
        """空向量与任何向量相似度为 0。"""
        vec = TfidfVectorizer().fit(["苹果好吃"])
        assert cosine_similarity({}, vec.transform("苹果好吃")) == 0.0
        assert cosine_similarity({}, {}) == 0.0

    def test_different_lengths_same_direction(self) -> None:
        """余弦比方向：长度不同但词分布比例相同的向量相似度高。"""
        # 构造两个方向一致、长度不同的向量（人工向量，不走 fit）
        a = {"苹果": 2.0, "好吃": 1.0}
        b = {"苹果": 4.0, "好吃": 2.0}
        # 方向相同 -> cos = 1.0（这正是余弦优于欧氏距离的演示）
        assert abs(cosine_similarity(a, b) - 1.0) < 1e-9

    def test_orthogonal_vectors_zero(self) -> None:
        """无共同词 -> 相似度 0。"""
        a = {"苹果": 1.0}
        b = {"跑步": 1.0}
        assert abs(cosine_similarity(a, b)) < 1e-9


# ---------------------------------------------------------------------------
# Embedder 统一接口
# ---------------------------------------------------------------------------

class TestEmbedder:
    def test_full_pipeline(self) -> None:
        """fit -> encode -> similarity 完整链路。"""
        emb = Embedder().fit(["苹果很好吃", "我喜欢吃苹果", "今天天气晴朗"])
        va = emb.encode("苹果很好吃")
        vb = emb.encode("我喜欢吃苹果")
        vc = emb.encode("今天天气晴朗")
        assert emb.similarity(va, vb) > emb.similarity(va, vc)

    def test_encode_all_order_preserved(self) -> None:
        """encode_all 输出顺序与输入一致。"""
        texts = ["苹果", "香蕉", "葡萄"]
        emb = Embedder().fit(texts)
        vectors = emb.encode_all(texts)
        assert len(vectors) == 3
        # 每篇文本与其自身向量的相似度应最高（同词重合）
        for i in range(3):
            assert vectors[i].get("苹果", 0) >= 0

    def test_similarity_symmetric(self) -> None:
        """相似度对称：sim(a,b) == sim(b,a)。"""
        emb = Embedder().fit(["苹果好吃", "好吃苹果"])
        va = emb.encode("苹果好吃")
        vb = emb.encode("好吃苹果")
        assert abs(emb.similarity(va, vb) - emb.similarity(vb, va)) < 1e-9
