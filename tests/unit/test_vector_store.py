"""
SynapseMatrix — tests/unit/test_vector_store.py
================================================
向量检索的单元测试。

覆盖范围：
    - add / add_batch / __len__：索引构建
    - search：top-k 数量、降序、命中正确性、阈值过滤、空输入
    - 边界：空索引、空 query、top_k 非法、长度不匹配

运行：cd 项目根目录 && python -m pytest tests/unit -q
"""

import pytest

from memory.embedding.tfidf import TfidfVectorizer
from memory.parser.chunker import Chunk
from memory.retrieval.vector_store import RetrievalHit, VectorStore


def _chunk(cid: int, page: int, text: str) -> Chunk:
    """构造一个 Chunk（chunk_id/page_no/text）。"""
    return Chunk(chunk_id=cid, page_no=page, text=text)


def _vectorizer(texts: list[str]) -> TfidfVectorizer:
    """构造一个已 fit 的向量化器。"""
    return TfidfVectorizer().fit(texts)


# ---------------------------------------------------------------------------
# 索引构建
# ---------------------------------------------------------------------------

class TestAdd:
    def test_add_and_len(self) -> None:
        store = VectorStore()
        assert len(store) == 0
        store.add(_chunk(1, 1, "苹果很好吃"), {"苹果": 1.0})
        assert len(store) == 1

    def test_add_empty_vector_skipped(self) -> None:
        """空向量（全 OOV 词）无检索价值，应跳过。"""
        store = VectorStore()
        store.add(_chunk(1, 1, "苹果"), {})
        assert len(store) == 0

    def test_add_batch(self) -> None:
        store = VectorStore()
        chunks = [_chunk(1, 1, "苹果"), _chunk(2, 2, "香蕉")]
        vectors = [{"苹果": 1.0}, {"香蕉": 1.0}]
        store.add_batch(chunks, vectors)
        assert len(store) == 2

    def test_add_batch_length_mismatch(self) -> None:
        store = VectorStore()
        with pytest.raises(ValueError):
            store.add_batch([_chunk(1, 1, "苹果")], [{"苹果": 1.0}, {"香蕉": 1.0}])


# ---------------------------------------------------------------------------
# 检索
# ---------------------------------------------------------------------------

class TestSearch:
    def _build_store(self, texts: list[str], min_score: float = 0.0) -> tuple[VectorStore, TfidfVectorizer]:
        """建一个用 TF-IDF 真实向量化的 store，返回 (store, vectorizer)。"""
        vec = _vectorizer(texts)
        chunks = [_chunk(i + 1, i + 1, t) for i, t in enumerate(texts)]
        vectors = vec.transform_all(texts)
        store = VectorStore(min_score=min_score)
        store.add_batch(chunks, vectors)
        return store, vec

    def test_most_similar_chunk_ranked_first(self) -> None:
        """query 对应文本的 chunk 应排第一（命中正确性）。"""
        texts = ["苹果很好吃", "今天天气晴朗适合跑步", "SynapseMatrix 是分布式系统"]
        store, vec = self._build_store(texts)
        hits = store.search(vec.transform("苹果很好吃"), top_k=3)
        assert hits[0].chunk_id == 1
        assert hits[0].score > hits[1].score  # 严格降序

    def test_top_k_count(self) -> None:
        """top_k 限制返回数量。"""
        texts = ["苹果好吃", "香蕉好吃", "葡萄好吃", "西瓜好吃", "桃子好吃", "梨子好吃"]
        store, vec = self._build_store(texts)
        hits = store.search(vec.transform("苹果好吃"), top_k=3)
        assert len(hits) == 3

    def test_top_k_gt_docs_returns_all(self) -> None:
        """top_k 大于索引数时返回全部（不报错）。"""
        texts = ["苹果", "香蕉"]
        store, vec = self._build_store(texts)
        hits = store.search(vec.transform("苹果"), top_k=10)
        assert len(hits) == 2

    def test_results_descending(self) -> None:
        """结果必须按相似度降序。"""
        texts = ["苹果好吃", "苹果好吃香蕉", "苹果", "今天天气"]
        store, vec = self._build_store(texts)
        hits = store.search(vec.transform("苹果"), top_k=4)
        scores = [h.score for h in hits]
        assert scores == sorted(scores, reverse=True)

    def test_min_score_filter(self) -> None:
        """低于阈值的噪声结果应被过滤。"""
        texts = ["苹果很好吃", "今天天气晴朗适合跑步"]
        store, vec = self._build_store(texts, min_score=0.9)
        hits = store.search(vec.transform("苹果很好吃"), top_k=5)
        # 只有与 query 高度相似的自己（score≈1.0）能过 0.9 阈值
        assert len(hits) == 1
        assert hits[0].chunk_id == 1

    def test_empty_store(self) -> None:
        """空索引返回空列表。"""
        store = VectorStore()
        assert store.search({"苹果": 1.0}, top_k=5) == []

    def test_empty_query_vector(self) -> None:
        """空 query 向量（全 OOV）返回空列表。"""
        store, vec = self._build_store(["苹果好吃"])
        assert store.search({}, top_k=5) == []

    def test_hit_fields(self) -> None:
        """RetrievalHit 携带 chunk_id/page_no/text/score。"""
        store, vec = self._build_store(["苹果很好吃", "天气不错"])
        hits = store.search(vec.transform("苹果很好吃"), top_k=1)
        h = hits[0]
        assert isinstance(h, RetrievalHit)
        assert h.chunk_id == 1
        assert h.page_no == 1
        assert h.text == "苹果很好吃"
        assert h.score > 0.9  # 自匹配相似度接近 1

    def test_invalid_top_k(self) -> None:
        """top_k < 1 应报错。"""
        store, vec = self._build_store(["苹果"])
        with pytest.raises(ValueError):
            store.search(vec.transform("苹果"), top_k=0)


# ---------------------------------------------------------------------------
# 与真实链路的组合（无外部文件）
# ---------------------------------------------------------------------------

class TestIntegration:
    def test_pipeline_rank_consistency(self) -> None:
        """完整链路：文本 -> 向量 -> 检索，相关 chunk 应命中。"""
        texts = [
            "SynapseMatrix 是一个端侧优先 P2P 协同的认知计算系统",
            "本文介绍 PDF 文本提取与切分方法",
            "记忆衰减使用指数函数 activation(t) = a0 * exp(-lambda * t)",
        ]
        vec = _vectorizer(texts)
        chunks = [_chunk(i + 1, i + 1, t) for i, t in enumerate(texts)]
        store = VectorStore(min_score=0.0)
        store.add_batch(chunks, vec.transform_all(texts))

        hits = store.search(vec.transform("什么是记忆衰减"), top_k=2)
        assert hits[0].chunk_id == 3  # 语义上衰减相关的 chunk 排第一
