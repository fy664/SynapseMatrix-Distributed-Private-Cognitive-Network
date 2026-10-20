"""
SynapseMatrix — tests/unit/test_temporal_retrieval.py
=====================================================
Temporal Retrieval（时间感知检索）与 Memory Benchmark 指标的单元测试
（Month 2 验收）。

覆盖范围：
    - pipeline.search 的 since 时间窗过滤（只检索窗口内记忆）
    - since 过滤对纯向量模式同样生效
    - 新旧记忆排序：Memory Score 的 recency 让新记忆压过旧记忆
    - benchmark 指标函数：Recall / Precision / MRR / Top-1 计算正确

运行：cd 项目根目录 && python -m pytest tests/unit -q
"""

import sys
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest

# 允许从项目根运行
PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from memory.pipeline import MemoryPipeline  # noqa: E402
from memory.store.memory_store import MemoryStore  # noqa: E402

# 复用 benchmark 的指标函数（同一套口径，保证 Benchmark 与测试一致）
from benchmarks.memory.run_benchmark import _metrics  # noqa: E402


def _make_pipeline(tmp_path: Path) -> MemoryPipeline:
    """构造一个指向临时目录的 pipeline（不导入 PDF，直接写记忆库）。"""
    return MemoryPipeline(
        store_path=tmp_path / "m.json",
        embedder_path=tmp_path / "e.json",
        chunk_size=200,
        overlap=30,
    )


def _seed_store(pipeline: MemoryPipeline, records: list[dict]) -> None:
    """直接向记忆库写入记录（绕过 PDF 导入，测试聚焦时间维度）。

    参数：
        records: 每个 dict = {source, cid, text, vector, created_at, page_no}
    """
    store = MemoryStore(pipeline._store_path)
    for r in records:
        store.add_remote_record(
            source=r["source"],
            source_chunk_id=r["cid"],
            page_no=r.get("page_no", 1),
            text=r["text"],
            vector=r["vector"],
            created_at=r["created_at"],
        )
    store.save()


class TestTemporalFilter:
    def test_since_filters_out_old_records(self, tmp_path) -> None:
        """since 时间窗：窗口外（更早）的记忆被硬性排除。"""
        pl = _make_pipeline(tmp_path)
        now = datetime.now(timezone.utc)
        _seed_store(pl, [
            # 一条"最近"、一条"很久以前"
            {
                "source": "s.pdf", "cid": 1,
                "text": "最近写下的关于 量子计算 的笔记",
                "vector": {"量子": 1.0, "计算": 1.0},
                "created_at": now.isoformat(),
            },
            {
                "source": "s.pdf", "cid": 2,
                "text": "很久以前关于 量子计算 的旧笔记",
                "vector": {"量子": 1.0, "计算": 1.0},
                "created_at": (now - timedelta(days=90)).isoformat(),
            },
        ])

        # 模型：直接给两段文本训一个（供 encode 用）
        from memory.embedding.embedder import Embedder
        emb = Embedder().fit(["量子计算 最近 笔记", "量子计算 旧 笔记"])
        emb.save(pl._embedder_path)

        # 窗口 = 最近 30 天：旧记录（90 天前）应被排除
        since = (now - timedelta(days=30)).isoformat()
        hits = pl.search("量子计算 笔记", top_k=5, min_score=0.0, since=since)
        ids = {h.chunk_id for h in hits}
        assert 1 in ids          # 新记录保留
        assert 2 not in ids      # 旧记录被时间窗排除

    def test_since_none_returns_all(self, tmp_path) -> None:
        """默认（since=None）不过滤，新旧都在结果里。"""
        pl = _make_pipeline(tmp_path)
        now = datetime.now(timezone.utc)
        _seed_store(pl, [
            {
                "source": "s.pdf", "cid": 1,
                "text": "最近写下的关于 量子计算 的笔记",
                "vector": {"量子": 1.0, "计算": 1.0},
                "created_at": now.isoformat(),
            },
            {
                "source": "s.pdf", "cid": 2,
                "text": "很久以前关于 量子计算 的旧笔记",
                "vector": {"量子": 1.0, "计算": 1.0},
                "created_at": (now - timedelta(days=90)).isoformat(),
            },
        ])
        from memory.embedding.embedder import Embedder
        Embedder().fit(["量子计算 最近 笔记", "量子计算 旧 笔记"]).save(pl._embedder_path)

        hits = pl.search("量子计算 笔记", top_k=5, min_score=0.0)
        assert {h.chunk_id for h in hits} == {1, 2}

    def test_since_works_in_plain_vector_mode(self, tmp_path) -> None:
        """时间窗过滤对纯向量模式（对照）同样生效——过滤独立于排序。"""
        pl = _make_pipeline(tmp_path)
        now = datetime.now(timezone.utc)
        _seed_store(pl, [
            {
                "source": "s.pdf", "cid": 1,
                "text": "最近写下的关于 量子计算 的笔记",
                "vector": {"量子": 1.0, "计算": 1.0},
                "created_at": now.isoformat(),
            },
            {
                "source": "s.pdf", "cid": 2,
                "text": "很久以前关于 量子计算 的旧笔记",
                "vector": {"量子": 1.0, "计算": 1.0},
                "created_at": (now - timedelta(days=90)).isoformat(),
            },
        ])
        from memory.embedding.embedder import Embedder
        Embedder().fit(["量子计算 最近 笔记", "量子计算 旧 笔记"]).save(pl._embedder_path)

        since = (now - timedelta(days=30)).isoformat()
        hits = pl.search(
            "量子计算 笔记", top_k=5, min_score=0.0,
            use_memory_score=False, since=since,
        )
        assert {h.chunk_id for h in hits} == {1}


class TestRecencyRanking:
    def test_new_memory_ranks_above_old(self, tmp_path) -> None:
        """同语义下，新记忆因 recency 分量压过旧记忆（时间感知排序）。"""
        pl = _make_pipeline(tmp_path)
        now = datetime.now(timezone.utc)
        # 语义相同（相同向量），时间不同：新 vs 旧
        _seed_store(pl, [
            {
                "source": "s.pdf", "cid": 1,
                "text": "新的 量子计算 笔记",
                "vector": {"量子": 1.0, "计算": 1.0},
                "created_at": now.isoformat(),
            },
            {
                "source": "s.pdf", "cid": 2,
                "text": "旧的 量子计算 笔记",
                "vector": {"量子": 1.0, "计算": 1.0},
                "created_at": (now - timedelta(days=60)).isoformat(),
            },
        ])
        from memory.embedding.embedder import Embedder
        Embedder().fit(["新的 量子计算 笔记", "旧的 量子计算 笔记"]).save(pl._embedder_path)

        hits = pl.search("量子计算 笔记", top_k=2, min_score=0.0)
        assert hits[0].chunk_id == 1      # 新的排第一
        assert hits[0].recency > hits[1].recency
        # 纯向量模式下两者同分（顺序无时间偏好）
        plain = pl.search("量子计算 笔记", top_k=2, min_score=0.0, use_memory_score=False)
        assert {h.chunk_id for h in plain} == {1, 2}


class TestBenchmarkMetrics:
    """benchmark 指标函数（与 run_benchmark.py 同源）的正确性。"""

    def test_recall_precision_mrr_top1(self) -> None:
        """指标计算：top-k 命中 ground truth 的标准定义。"""
        truth = {3, 7}
        # 第一个相关在第 2 位，共命中 2 个
        m = _metrics([5, 3, 7, 9], truth)
        assert m["recall"] == 1.0        # 2/2 全部命中
        assert m["precision"] == 0.5     # 4 个返回中 2 个相关
        assert m["mrr"] == 0.5           # 第一个相关在 rank 2 -> 1/2
        assert m["top1"] == 0.0          # 第一位不相关

    def test_no_hits(self) -> None:
        """零命中：recall/precision/mrr/top1 全 0。"""
        m = _metrics([1, 2], {9})
        assert m == {"recall": 0.0, "precision": 0.0, "mrr": 0.0, "top1": 0.0}

    def test_empty_truth_guarded(self) -> None:
        """ground truth 为空时指标不除零、不崩溃。"""
        m = _metrics([1, 2], set())
        assert m["recall"] == 0.0
        assert m["precision"] == 0.0  # 没有"相关"定义时精确率为 0
        assert m["mrr"] == 0.0
        assert m["top1"] == 0.0
