"""
SynapseMatrix — Pipeline 层：编排入口（Month 1 环节 6/6）
===========================================================

定位：规格书 L2 Local Cognitive Core 的统一入口。
把散落的命令行调用（parse / chunk / embed / index / store）串成
可重复调用的 pipeline——"PDF 进，记忆出"。

编排层的职责边界（本环节的核心知识点）：
    编排只负责"串"，不负责"实现"：
        - 不重复实现任何环节的算法（解析/切分/向量化/检索都有专门模块）
        - 只负责：决定调用顺序、管理状态（store/embedder 的加载与保存）、
          报告结果
    这就是分层思想：每层做一件事，上层编排、下层实现。
    好处：任一环节替换（如 V2 把 TF-IDF 换 BGE），pipeline 零改动。

流程（幂等设计）：
    ingest_pdf(pdf_path):
        1. 加载/创建记忆库（data/memory.json）
        2. 同源已导入且非 force -> 直接跳过（不产生重复记忆）
        3. 加载/训练向量化模型（data/embedder.json）
           首次导入需 fit（词典/IDF 来自全部 chunk 文本）
        4. 解析 PDF -> 切分 -> 向量化 -> 写入记忆库 -> 落盘

    search(query):
        1. 加载记忆库 + 模型（保证向量空间一致）
        2. 重建 VectorStore 索引（从持久化记录）
        3. 查询向量化 -> 检索 -> 返回

    rebuild_index(store_path, embedder_path):
        从记忆库重建内存检索索引（进程重启后的恢复步骤）
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

from memory.activation.decay import exponential_decay
from memory.activation.memory_score import (
    MemoryScoreInput,
    MemoryScoreWeights,
    ScoredMemory,
    compute,
    rank,
)
from memory.embedding.embedder import Embedder
from memory.parser.chunker import ChunkResult, chunk_pages
from memory.parser.pdf_parser import ParseResult, extract_pdf_text
from memory.retrieval.vector_store import RetrievalHit, VectorStore
from memory.store.memory_store import MemoryStore

# 默认数据目录（相对项目根；可通过 MemoryPipeline 构造参数覆盖）
DEFAULT_DATA_DIR = Path("data")
DEFAULT_STORE_PATH = DEFAULT_DATA_DIR / "memory.json"
DEFAULT_EMBEDDER_PATH = DEFAULT_DATA_DIR / "embedder.json"


@dataclass
class IngestResult:
    """一次导入的结果摘要（供日志/测试断言）。"""

    source: str
    pages: int = 0
    chunks_added: int = 0
    skipped: bool = False  # True = 同源已存在，本次跳过


@dataclass
class RankedHit:
    """检索命中 + Memory Score 综合排序后的结果。

    score     : Memory Score 综合分（语义+新近+激活+重要+图）
    raw_score : 纯向量余弦相似度（保留，便于对比两套排序）
    recency   : 新近度分量（由 created_at 经衰减计算）
    chunk_id / page_no / text : 同 RetrievalHit
    """

    chunk_id: int
    page_no: int
    score: float
    raw_score: float
    recency: float
    text: str


class MemoryPipeline:
    """PDF 记忆库的编排入口（串流程，不实现算法）。"""

    def __init__(
        self,
        store_path: str | Path = DEFAULT_STORE_PATH,
        embedder_path: str | Path = DEFAULT_EMBEDDER_PATH,
        chunk_size: int = 512,
        overlap: int = 64,
        half_life_seconds: float = 30 * 24 * 3600,  # 记忆半衰期 30 天
    ) -> None:
        """初始化编排器。

        参数：
            store_path      : 记忆库文件路径
            embedder_path   : 向量化模型文件路径
            chunk_size      : 切分块长（透传给 chunker）
            overlap         : 切分重叠（透传给 chunker）
            half_life_seconds: recency 衰减的半衰期（Memory Score 用）
        """
        self._store_path = Path(store_path)
        self._embedder_path = Path(embedder_path)
        self._chunk_size = chunk_size
        self._overlap = overlap
        # lambda = ln2 / T（由半衰期换算，见 decay.half_life）
        self._lambda = math.log(2) / half_life_seconds

    # ------------------------------------------------------------------
    # 导入
    # ------------------------------------------------------------------

    def ingest_pdf(
        self,
        pdf_path: str | Path,
        source: str | None = None,
        force: bool = False,
    ) -> IngestResult:
        """把一份 PDF 导入记忆库（幂等：同源二次导入默认跳过）。

        参数：
            pdf_path: PDF 文件路径
            source  : 文档来源标识；为 None 时用文件名（含扩展名）
            force   : True 时先删除该源旧记忆再全量重建
                      （文档内容更新后重新导入用）

        返回：
            IngestResult 摘要（页数/新增 chunk 数/是否跳过）
        """
        pdf_path = Path(pdf_path)
        source = source or pdf_path.name
        if not pdf_path.exists():
            raise FileNotFoundError(f"PDF 不存在: {pdf_path}")

        # 1) 加载/创建记忆库
        store = MemoryStore(self._store_path)
        if store.exists():
            store.load()

        # 2) 幂等检查：同源已导入且未要求重建 -> 跳过
        if store.has_source(source) and not force:
            return IngestResult(source=source, skipped=True)
        if force:
            store.delete_by_source(source)

        # 3) 加载/训练向量化模型
        embedder = Embedder()
        if self._embedder_path.exists():
            embedder.load(self._embedder_path)
        # 模型未训练（首次导入）：先用本次文本 fit；已训练则沿用旧空间
        # （V1 简化：不增量更新 IDF，新词忽略——技术债 #16）

        # 4) 解析 -> 切分
        parsed: ParseResult = extract_pdf_text(str(pdf_path))
        chunked: ChunkResult = chunk_pages(
            parsed.pages, self._chunk_size, self._overlap
        )
        chunks = chunked.chunks
        if not chunks:
            return IngestResult(source=source, pages=len(parsed.pages), chunks_added=0)

        # 5) 向量化
        if not embedder.is_fitted():
            embedder.fit([c.text for c in chunks])
        vectors = embedder.encode_all([c.text for c in chunks])

        # 6) 写入记忆库
        added = 0
        for chunk, vector in zip(chunks, vectors):
            if not vector:  # 空向量无检索价值，跳过（与 VectorStore.add 一致）
                continue
            store.add_record(
                page_no=chunk.page_no, text=chunk.text, vector=vector, source=source
            )
            added += 1
        store.save()

        # 7) 模型落盘（首次训练后）
        if not self._embedder_path.exists():
            embedder.save(self._embedder_path)

        return IngestResult(source=source, pages=len(parsed.pages), chunks_added=added)

    # ------------------------------------------------------------------
    # 检索
    # ------------------------------------------------------------------

    def _load(self) -> tuple[MemoryStore, Embedder]:
        """加载记忆库 + 模型（检索前必须保证向量空间一致）。"""
        store = MemoryStore(self._store_path)
        if not store.exists():
            raise FileNotFoundError(
                f"记忆库不存在: {self._store_path}（请先 ingest_pdf 导入）"
            )
        store.load()

        embedder = Embedder()
        if not self._embedder_path.exists():
            raise FileNotFoundError(
                f"模型不存在: {self._embedder_path}（请先 ingest_pdf 导入）"
            )
        embedder.load(self._embedder_path)
        return store, embedder

    def rebuild_index(
        self,
        store: MemoryStore,
        embedder: Embedder,
        min_score: float = 0.05,
    ) -> VectorStore:
        """从持久化记忆重建内存检索索引。

        为什么需要这一步：VectorStore 本身不持久化（技术债 #9），
        重启后索引为空——从记忆库记录重建，正是"持久化救活了检索"。
        """
        vs = VectorStore(min_score=min_score)
        for r in store:
            # 重建需要 Chunk 对象（VectorStore 接口依赖其字段）；
            # 从 MemoryRecord 组装，接口兼容
            chunk = _ChunkLike(r.chunk_id, r.page_no, r.text)
            vs.add(chunk, r.vector)
        return vs

    def search(
        self,
        query: str,
        top_k: int = 5,
        min_score: float = 0.05,
        use_memory_score: bool = True,
        weights: MemoryScoreWeights | None = None,
        since: str | None = None,
    ) -> list[RankedHit]:
        """检索 + 可选 Memory Score 综合排序（Temporal Retrieval）。

        参数：
            query            : 查询文本
            top_k            : 返回条数
            min_score        : 向量相似度下限（先过滤噪声）
            use_memory_score : True 时用五维综合排序（默认），
                               False 时退回纯相似度（对照实验用）
            weights          : Memory Score 权重（None = 默认权重）
            since            : ISO 时间字符串（含时区）。非空时只检索
                               created_at >= since 的记忆——时间窗过滤，
                               "不检索 30 天前的记忆"这类需求直接可表达。
                               默认 None = 全量检索。

        返回：
            RankedHit 列表，按分数降序

        时间维度（Temporal Retrieval）的两层语义：
            1. 过滤层（since）：显式时间窗，硬性排除窗口外记忆
               ——对应规格书"只查最近 N 天的记忆"场景
            2. 排序层（recency）：Memory Score 的新近度分量按
               created_at 衰减，新记忆天然压过旧记忆
               ——对应规格书 9.4 Memory Score 的 Recency 项
        """
        store, embedder = self._load()
        vs = self.rebuild_index(store, embedder, min_score=min_score)

        qvec = embedder.encode(query)
        hits: list[RetrievalHit] = vs.search(qvec, top_k=top_k * 3)  # 取多，排序中筛

        # chunk_id -> 记录映射（一次构建，过滤与打分共用）
        record_by_id = {r.chunk_id: r for r in store}

        # 时间窗过滤（Temporal Retrieval 过滤层）：按 created_at 硬性排除
        if since is not None:
            since_dt = datetime.fromisoformat(since)
            hits = [
                h
                for h in hits
                if h.chunk_id in record_by_id
                and datetime.fromisoformat(record_by_id[h.chunk_id].created_at)
                >= since_dt
            ]

        if not use_memory_score:
            # 对照模式：退回纯向量相似度排序（原样返回，不计算 Memory Score）
            return [
                RankedHit(
                    chunk_id=h.chunk_id,
                    page_no=h.page_no,
                    score=h.score,
                    raw_score=h.score,
                    recency=0.0,
                    text=h.text,
                )
                for h in hits[:top_k]
            ]

        # Memory Score 模式：为每个 hit 组装五维输入
        now = datetime.now(timezone.utc)

        scored: list[ScoredMemory] = []
        recency_map: dict[int, float] = {}
        for h in hits:
            # 从记录映射找回该 chunk 的 created_at（时间感知检索的数据源）
            record = record_by_id.get(h.chunk_id)
            recency = 0.0
            if record is not None:
                created = datetime.fromisoformat(record.created_at)
                dt = max(0.0, (now - created).total_seconds())
                # 新近度 = 衰减后的激活度（刚创建=1，越久越趋近 0）
                recency = exponential_decay(1.0, self._lambda, dt)
            recency_map[h.chunk_id] = recency

            inp = MemoryScoreInput(
                semantic=h.score,       # 向量相似度
                recency=recency,        # 时间衰减
                activation=recency,     # V1 简化：激活度暂用新近度近似（技术债 #17）
                importance=0.5,         # V1 无来源，取默认（技术债 #15）
                graph=0.0,              # V1 图谱未接 chunk 级实体（技术债 #13）
            )
            scored.append(compute(str(h.chunk_id), inp, weights=weights))

        ordered = rank(scored)
        result: list[RankedHit] = []
        for m in ordered[:top_k]:
            hit = next(h for h in hits if str(h.chunk_id) == m.memory_id)
            result.append(
                RankedHit(
                    chunk_id=hit.chunk_id,
                    page_no=hit.page_no,
                    score=m.score,
                    raw_score=hit.score,
                    recency=recency_map[hit.chunk_id],
                    text=hit.text,
                )
            )
        return result


# VectorStore.add 依赖 chunker.Chunk 的字段；用轻量替代避免跨层强耦合
@dataclass(frozen=True)
class _ChunkLike:
    """VectorStore 接口所需的 Chunk 最小形态（chunk_id/page_no/text）。"""

    chunk_id: int
    page_no: int
    text: str


if __name__ == "__main__":
    import sys
    import tempfile

    if len(sys.argv) < 2:
        print("用法: python pipeline.py <path-to-pdf> [query]")
        sys.exit(1)

    pdf = sys.argv[1]
    query = sys.argv[2] if len(sys.argv) > 2 else "这个项目的定位是什么"

    with tempfile.TemporaryDirectory() as tmp:
        pl = MemoryPipeline(
            store_path=Path(tmp) / "memory.json",
            embedder_path=Path(tmp) / "embedder.json",
        )
        first = pl.ingest_pdf(pdf)
        second = pl.ingest_pdf(pdf)  # 幂等验证
        print(f"首次导入: 页{first.pages} chunk+{first.chunks_added}")
        print(f"二次导入: skipped={second.skipped}（幂等）")

        for hit in pl.search(query, top_k=3):
            print(
                f"  #{hit.chunk_id} p{hit.page_no} 综合分{hit.score:.4f} "
                f"(纯相似度{hit.raw_score:.4f}, 新近度{hit.recency:.3f})"
            )
            print(f"    {hit.text[:40]}...")
