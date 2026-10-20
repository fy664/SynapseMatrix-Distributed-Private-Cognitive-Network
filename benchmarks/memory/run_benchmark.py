"""
SynapseMatrix — Benchmark：Vector RAG vs Synapse Memory（Month 2 验收）
=======================================================================

定位：规格书 Month 2 验收项——"给定相同问题，Vector RAG vs
Synapse Memory 能够做 Benchmark"。

对比的两套排序：
    Vector RAG    : 纯 TF-IDF 余弦相似度 top-k（use_memory_score=False）
    Synapse Memory: 五维 Memory Score 综合排序（语义 0.5 + 新近 0.2 +
                    激活 0.15 + 重要 0.1 + 图 0.05，use_memory_score=True）

场景 A（总体质量）：
    同一语料（规格书样例 PDF）导入记忆库，10 个问题各定位 ground
    truth chunk（含关键词的 chunk 集合），比较两套排序的
    Recall@k / Precision@k / MRR / Top-1 命中。

场景 B（时间感知，Temporal Retrieval）：
    把语料里一半 chunk 的 created_at 改成 30 天前（旧记忆），一半保持
    新建。对"新旧都有相关内容"的问题：
        - Vector RAG 无时间概念，新旧一视同仁（按语义排）
        - Synapse Memory 的新近度衰减让新记忆压过旧记忆
    量化两套排序在"时间维度"上的差异——这是 Temporal Retrieval
    价值的最直接证据。

指标定义：
    Recall@k    : top-k 中命中 ground truth 的比例（覆盖度）
    Precision@k : top-k 中相关 chunk 占返回数的比例
    MRR         : 第一个相关 chunk 位置倒数的平均（排序质量）
    Top-1 命中  : 第一个返回的就是相关 chunk 的问题占比

运行：python benchmarks/memory/run_benchmark.py
"""

from __future__ import annotations

import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path

# 允许脚本从项目根直接运行（sys.path 兼容）
PROJECT_ROOT = Path(__file__).resolve().parents[2]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from memory.pipeline import MemoryPipeline

# 语料：完整规格书生成的 PDF（66 页，覆盖 Memory Score/衰减/检索/推理等全部章节）
SAMPLE_PDF = PROJECT_ROOT / "benchmarks" / "memory" / "spec_full.pdf"


@dataclass
class _Chunk:
    """ground truth 定位用的轻量 chunk（chunk_id + text）。"""

    chunk_id: int
    text: str

# 问题集：每个问题带 ground truth 关键词（出现在相关 chunk 文本中）
# 关键词已按规格书实际表述校准（均能命中 chunk）
QUESTIONS: list[tuple[str, str]] = [
    ("Memory Score 由哪几个维度组成", "Memory Score"),
    ("记忆随时间如何衰减", "衰减"),
    ("推理延迟的指标是什么", "TTFT"),
    ("设备之间如何发现彼此", "节点发现"),
    ("设备之间同步什么内容", "增量同步"),
    ("为什么强调隐私保护", "隐私"),
    ("架构为什么端侧优先", "端侧优先"),
    ("文档怎么变成向量", "Embedding"),
    ("记忆怎么被检索", "检索"),
    ("多设备怎么协同推理", "推理"),
    ("P2P 网络的特点", "P2P"),
    ("记忆的激活机制", "激活"),
]

TOP_K = 5
OLD_DAYS = 30  # 场景 B：旧记忆的"年龄"（天）


def _locate_ground_truth(chunks: list, keyword: str) -> set[int]:
    """定位含关键词的 chunk_id 集合（ground truth）。

    判定标准：chunk 文本包含关键词即"相关"。
    关键词选独有概念词，命中范围小、人工可核验。
    """
    return {c.chunk_id for c in chunks if keyword in c.text}


def _metrics(hits_ids: list[int], truth: set[int]) -> dict:
    """计算单条查询的 Recall/Precision/MRR/Top1。"""
    hits = set(hits_ids)
    hit_count = len(hits & truth)
    recall = hit_count / len(truth) if truth else 0.0
    precision = hit_count / len(hits) if hits else 0.0
    mrr = 0.0
    for rank, cid in enumerate(hits_ids, start=1):
        if cid in truth:
            mrr = 1.0 / rank
            break
    return {
        "recall": recall,
        "precision": precision,
        "mrr": mrr,
        "top1": 1.0 if hits_ids and hits_ids[0] in truth else 0.0,
    }


def _aggregate(rows: list[dict]) -> dict:
    """汇总多条查询的平均指标。"""
    n = len(rows)
    return {k: sum(r[k] for r in rows) / n for k in rows[0]}


def _age_records(store_path: Path, old_chunk_ids: set[int], days: int) -> None:
    """把指定 chunk 的 created_at 改成 N 天前（模拟旧记忆）。"""
    store = MemoryStore(store_path).load()
    old_dt = (datetime.now(timezone.utc) - timedelta(days=days)).isoformat()
    changed = 0
    for r in store:
        if r.chunk_id in old_chunk_ids:
            r.created_at = old_dt  # 直接改时间戳（V1 演示用）
            changed += 1
    store.save()
    return changed


def main() -> int:
    if not SAMPLE_PDF.exists():
        print(f"[!] 样例 PDF 不存在: {SAMPLE_PDF}")
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        pl = MemoryPipeline(
            store_path=tmp / "memory.json",
            embedder_path=tmp / "embedder.json",
            chunk_size=200,  # 小 chunk：问题更容易定位到单一主题
            overlap=30,
        )

        # ---- 导入语料 ----
        result = pl.ingest_pdf(SAMPLE_PDF, source="sample_spec.pdf")
        print(f"导入: 页{result.pages}, chunk +{result.chunks_added}")

        store = MemoryStore(pl._store_path).load()
        chunks = [_Chunk(r.chunk_id, r.text) for r in store]
        all_truth = {c.chunk_id for c in chunks}

        # ---- 场景 A：总体质量对比 ----
        rows_rag, rows_syn = [], []
        for query, kw in QUESTIONS:
            truth = _locate_ground_truth(chunks, kw)
            if not truth:
                print(f"[!] 关键词未命中任何 chunk: {kw}")
                continue
            rag = pl.search(query, top_k=TOP_K, use_memory_score=False)
            syn = pl.search(query, top_k=TOP_K, use_memory_score=True)
            rows_rag.append(_metrics([h.chunk_id for h in rag], truth))
            rows_syn.append(_metrics([h.chunk_id for h in syn], truth))

        agg_rag = _aggregate(rows_rag)
        agg_syn = _aggregate(rows_syn)

        print(f"\n===== 场景 A：总体质量（{len(rows_rag)} 问平均） =====")
        print(f"{'指标':<10}{'Vector RAG':>12}{'Synapse Memory':>16}")
        for k in ("recall", "precision", "mrr", "top1"):
            print(f"{k:<10}{agg_rag[k]:>12.3f}{agg_syn[k]:>16.3f}")

        # ---- 场景 B：时间感知（Temporal Retrieval）----
        # 造"新旧冲突"：一半 chunk 变成 30 天前
        import random
        rng = random.Random(42)
        old_ids = set(rng.sample(sorted(all_truth), k=len(all_truth) // 2))
        _age_records(pl._store_path, old_ids, OLD_DAYS)

        rows_b_rag, rows_b_syn = [], []
        for query, kw in QUESTIONS:
            truth = _locate_ground_truth(chunks, kw)
            if not truth:
                continue
            # 只保留"新旧都有相关内容"的问题（同主题同时存在新旧记忆）
            truth_old = truth & old_ids
            truth_new = truth - old_ids
            if not (truth_old and truth_new):
                continue
            rag = pl.search(query, top_k=TOP_K, use_memory_score=False)
            syn = pl.search(query, top_k=TOP_K, use_memory_score=True)
            rows_b_rag.append(_metrics([h.chunk_id for h in rag], truth))
            rows_b_syn.append(_metrics([h.chunk_id for h in syn], truth))

        if rows_b_rag:
            agg_b_rag = _aggregate(rows_b_rag)
            agg_b_syn = _aggregate(rows_b_syn)
            print(f"\n===== 场景 B：新旧冲突（{len(rows_b_rag)} 问平均，旧记忆 30 天前） =====")
            print(f"{'指标':<10}{'Vector RAG':>12}{'Synapse Memory':>16}")
            for k in ("recall", "precision", "mrr", "top1"):
                print(f"{k:<10}{agg_b_rag[k]:>12.3f}{agg_b_syn[k]:>16.3f}")

            # 新记忆命中占比：Synapse Memory 是否优先返回新记忆
            new_share_rag = sum(
                1 for q, kw in QUESTIONS
                if _locate_ground_truth(chunks, kw) - old_ids
            )
            # 直接看 top-1 里新记忆的占比
            top1_new_rag = sum(
                1 for query, kw in QUESTIONS
                if (r := pl.search(query, top_k=1, use_memory_score=False))
                and r[0].chunk_id not in old_ids
            )
            top1_new_syn = sum(
                1 for query, kw in QUESTIONS
                if (r := pl.search(query, top_k=1, use_memory_score=True))
                and r[0].chunk_id not in old_ids
            )
            n_q = len(QUESTIONS)
            print(f"\nTop-1 返回【新记忆】的问题占比（时间感知的直接证据）：")
            print(f"  Vector RAG     : {top1_new_rag}/{n_q}")
            print(f"  Synapse Memory : {top1_new_syn}/{n_q}")

    return 0


if __name__ == "__main__":
    from memory.store.memory_store import MemoryStore  # noqa: F401 (用于 _age_records)
    sys.exit(main())
