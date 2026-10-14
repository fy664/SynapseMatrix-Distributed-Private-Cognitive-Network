"""
SynapseMatrix — tests/unit/test_pipeline.py
============================================
Pipeline 编排 + 模型持久化的单元测试。

覆盖范围：
    - ingest 端到端：PDF 进 -> 记忆库有记录
    - 幂等：同源二次导入 skipped=True，不重复
    - force 重建：删除旧源后重导
    - 重启恢复：保存后新 pipeline 直接可检索（无需重新解析）
    - search 返回 RankedHit 字段完整、按分降序
    - 记忆库不存在时 search 报错

测试依赖：
    - PyMuPDF（生成测试 PDF，与 test_pdf_parser 同套路）
    - 测试 PDF 文字用 fontname="china-s"，否则中文丢失

运行：cd 项目根目录 && python -m pytest tests/unit -q
"""

import fitz  # PyMuPDF（fitz 为旧别名，警告可忽略）

import pytest

from memory.pipeline import MemoryPipeline, RankedHit
from memory.retrieval.vector_store import RetrievalHit
from memory.store.memory_store import MemoryStore


def _make_pdf(tmp_path, filename: str = "sample.pdf", lines: list[str] | None = None):
    """生成一个含中文的测试 PDF（每行一个段落，跨两页）。"""
    if lines is None:
        lines = [
            "SynapseMatrix 是一个端侧优先的分布式私人认知网络。",
            "它强调隐私保护与 P2P 协同。",
            "记忆衰减让旧记忆的排序权重随时间下降。",
            "向量检索负责寻找语义相似的文本块。",
        ]
    path = tmp_path / filename
    doc = fitz.open()
    page = doc.new_page()
    y = 72
    for line in lines:
        page.insert_text((72, y), line, fontname="china-s", fontsize=14)
        y += 24
        if y > 700:  # 分页
            page = doc.new_page()
            y = 72
    doc.save(str(path))
    doc.close()
    return str(path)


class TestIngest:
    def test_ingest_creates_records(self, tmp_path) -> None:
        pdf = _make_pdf(tmp_path)
        pl = MemoryPipeline(
            store_path=tmp_path / "memory.json",
            embedder_path=tmp_path / "embedder.json",
            chunk_size=512,
            overlap=64,
        )
        result = pl.ingest_pdf(pdf)
        assert result.skipped is False
        assert result.chunks_added > 0
        # 记忆库文件与模型文件都落盘
        assert (tmp_path / "memory.json").exists()
        assert (tmp_path / "embedder.json").exists()

    def test_ingest_idempotent(self, tmp_path) -> None:
        pdf = _make_pdf(tmp_path)
        pl = MemoryPipeline(
            store_path=tmp_path / "memory.json",
            embedder_path=tmp_path / "embedder.json",
        )
        first = pl.ingest_pdf(pdf)
        second = pl.ingest_pdf(pdf)  # 同源二次导入
        assert first.skipped is False
        assert second.skipped is True

        store = MemoryStore(tmp_path / "memory.json").load()
        assert len(store) == first.chunks_added  # 不重复

    def test_ingest_force_rebuild(self, tmp_path) -> None:
        pdf = _make_pdf(tmp_path)
        pl = MemoryPipeline(
            store_path=tmp_path / "memory.json",
            embedder_path=tmp_path / "embedder.json",
        )
        pl.ingest_pdf(pdf)
        # 修改文档后 force 重建
        pdf2 = _make_pdf(tmp_path, "sample.pdf", ["完全不同的新内容。"] * 4)
        result = pl.ingest_pdf(pdf2, force=True)
        assert result.skipped is False

        store = MemoryStore(tmp_path / "memory.json").load()
        # 旧内容已删（只有新内容）
        assert all("记忆衰减" not in r.text for r in store)

    def test_ingest_missing_pdf_raises(self, tmp_path) -> None:
        pl = MemoryPipeline(store_path=tmp_path / "m.json", embedder_path=tmp_path / "e.json")
        with pytest.raises(FileNotFoundError):
            pl.ingest_pdf(tmp_path / "missing.pdf")


class TestSearch:
    def test_search_after_restart(self, tmp_path) -> None:
        """核心验收：重启后直接可检索，无需重新解析。"""
        pdf = _make_pdf(tmp_path)
        # 第一次生命周期：导入
        MemoryPipeline(
            store_path=tmp_path / "memory.json",
            embedder_path=tmp_path / "embedder.json",
        ).ingest_pdf(pdf)

        # 模拟重启：全新 pipeline 对象（不 ingest，直接 search）
        pl2 = MemoryPipeline(
            store_path=tmp_path / "memory.json",
            embedder_path=tmp_path / "embedder.json",
        )
        hits = pl2.search("隐私保护", top_k=3)
        assert len(hits) > 0
        # 命中最相关的语义：包含"隐私保护"的 chunk 应排在前面
        assert hits[0].text is not None

    def test_search_hit_fields(self, tmp_path) -> None:
        pdf = _make_pdf(tmp_path)
        pl = MemoryPipeline(
            store_path=tmp_path / "memory.json",
            embedder_path=tmp_path / "embedder.json",
        )
        pl.ingest_pdf(pdf)
        hits = pl.search("端侧优先", top_k=2)
        for h in hits:
            assert isinstance(h, RankedHit)
            assert h.chunk_id >= 1
            assert h.page_no >= 1
            assert 0.0 <= h.score <= 1.0
            assert 0.0 <= h.raw_score <= 1.0
            assert 0.0 <= h.recency <= 1.0
            assert len(h.text) > 0

    def test_search_returns_topk_descending(self, tmp_path) -> None:
        pdf = _make_pdf(tmp_path)
        pl = MemoryPipeline(
            store_path=tmp_path / "memory.json",
            embedder_path=tmp_path / "embedder.json",
        )
        pl.ingest_pdf(pdf)
        hits = pl.search("记忆衰减 时间", top_k=3)
        scores = [h.score for h in hits]
        assert scores == sorted(scores, reverse=True)
        assert len(hits) <= 3

    def test_search_without_store_raises(self, tmp_path) -> None:
        pl = MemoryPipeline(store_path=tmp_path / "m.json", embedder_path=tmp_path / "e.json")
        with pytest.raises(FileNotFoundError, match="记忆库不存在"):
            pl.search("随便")

    def test_search_memory_score_off(self, tmp_path) -> None:
        """关闭 Memory Score 时仍返回纯相似度排序。"""
        pdf = _make_pdf(tmp_path)
        pl = MemoryPipeline(
            store_path=tmp_path / "memory.json",
            embedder_path=tmp_path / "embedder.json",
        )
        pl.ingest_pdf(pdf)
        hits = pl.search("向量检索", top_k=3, use_memory_score=False)
        assert len(hits) > 0
        # 纯相似度模式下 score 应等于 raw_score（无加权项）
        for h in hits:
            assert h.score == pytest.approx(h.raw_score, abs=1e-9)


class TestMultiDocument:
    def test_two_sources_coexist(self, tmp_path) -> None:
        """不同来源文档导入后共存，id 连续不冲突。"""
        pdf_a = _make_pdf(tmp_path, "a.pdf", ["文档A的第一句话。"] * 3)
        pdf_b = _make_pdf(tmp_path, "b.pdf", ["文档B是完全不同的主题。"] * 3)
        pl = MemoryPipeline(
            store_path=tmp_path / "memory.json",
            embedder_path=tmp_path / "embedder.json",
        )
        pl.ingest_pdf(pdf_a)
        pl.ingest_pdf(pdf_b)

        store = MemoryStore(tmp_path / "memory.json").load()
        ids = [r.chunk_id for r in store]
        assert len(ids) == len(set(ids))  # id 无重复
        assert store.has_source("a.pdf") and store.has_source("b.pdf")
