"""
SynapseMatrix — Memory Retrieval 层：向量检索（Month 1 环节 4/6 最小实现）
===========================================================================

定位：承接 embedding 的输出，对应规格书第 11 章 Pipeline 的
"Embedding -> ... -> Retrieval" 中的 Retrieval，也是场景 B
"跨时间认知检索"的第一步：给定 query，返回最相关的记忆 chunk。

本模块职责（V1 范围）：
    1. 维护"chunk -> 向量"的索引（V1 为内存列表，不做持久化）
    2. 给定 query 向量，计算与全部 chunk 的相似度
    3. 用堆选出 top-k 并降序返回，附带 chunk 元信息（页码、文本）

设计决策（为什么 V1 用暴力扫描 + 堆，而不是倒排索引 / ANN）：
    - 规格书 0.2 核心原则：先正确，后性能；先手写最小实现。
    - 暴力扫描：对每个 chunk 算一次余弦相似度，复杂度 O(N * D)。
      当 N < 1 万时完全够用，且结果精确（ANN 是近似，有召回损失）。
    - 堆取 top-k：只需维护大小为 k 的小顶堆，复杂度 O(N log k)，
      而不是全排序的 O(N log N)——这是"前 k 大"问题的标准最优解，
      与 LeetCode 215（数组第 K 大）同一思想。
    - V2 换 ANN（faiss/hnswlib）时，本模块的 search 接口不变，
      只是内部从"全量扫描"换成"近似索引查询"——面向接口编程。

V1 简化（技术债，记入 TECH-DEBT）：
    - 索引只存内存，进程退出即丢失（V2 接向量数据库/持久化文件）
    - 不支持增量删除/更新（V2 加 chunk_id 级删除）
    - 不做多字段过滤（如按时间、按来源过滤，V2 加）

依赖：标准库 heapq、dataclasses（无第三方依赖）
"""

from __future__ import annotations

import heapq
from dataclasses import dataclass

from memory.embedding.tfidf import cosine_similarity
from memory.parser.chunker import Chunk


# ---------------------------------------------------------------------------
# 检索结果结构
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class RetrievalHit:
    """一条检索命中结果。

    属性说明：
        chunk_id: 命中的 chunk 编号（对应 Chunk.chunk_id）
        page_no : 来源页码（对应 Chunk.page_no，用户可据此回溯原文）
        score   : 与 query 的余弦相似度，越高越相关
        text    : chunk 原文（供上层直接引用/拼接进上下文）
    """

    chunk_id: int
    page_no: int
    score: float
    text: str


# ---------------------------------------------------------------------------
# 向量存储与检索
# ---------------------------------------------------------------------------

class VectorStore:
    """极简的内存向量检索器。

    用法：
        store = VectorStore()
        store.add(chunk, vector)          # 建索引
        hits = store.search(query_vector, top_k=5)   # 检索

    内部结构：
        self._entries: list[tuple[float, Chunk, dict]] 中的一个元素 =
            (vector, chunk, vector)
        检索时对每个 entry 算余弦，再用堆选 top-k。
    """

    def __init__(self, min_score: float = 0.0) -> None:
        """初始化空索引。

        参数：
            min_score: 相似度阈值，低于该值的结果会被过滤掉。
                       默认 0.0（余弦下限）；实际使用时建议调高到 0.1~0.3，
                       避免把"只有 1 个共用词"的噪声当命中。
        """
        self._min_score = min_score
        # (chunk, vector) 二元组列表：顺序插入，检索时全量扫描
        self._entries: list[tuple[Chunk, dict[str, float]]] = []

    def add(self, chunk: Chunk, vector: dict[str, float]) -> None:
        """把一个 chunk 及其向量加入索引。

        参数：
            chunk : chunker 产出的 Chunk 对象（含 chunk_id/page_no/text）
            vector: embedder.encode() 产出的向量（与 chunk.text 对应）
        """
        if vector:  # 空向量无检索价值，跳过（如全 OOV 词文本）
            self._entries.append((chunk, vector))

    def add_batch(
        self, chunks: list[Chunk], vectors: list[dict[str, float]]
    ) -> None:
        """批量加入（要求 chunks 与 vectors 长度一致、按序对应）。"""
        if len(chunks) != len(vectors):
            raise ValueError(
                f"chunks 与 vectors 长度不一致: {len(chunks)} vs {len(vectors)}"
            )
        for chunk, vector in zip(chunks, vectors):
            self.add(chunk, vector)

    def __len__(self) -> int:
        """索引中的条目数（供统计/测试）。"""
        return len(self._entries)

    def search(
        self, query_vector: dict[str, float], top_k: int = 5
    ) -> list[RetrievalHit]:
        """暴力扫描 + 堆选 top-k 的检索。

        参数：
            query_vector: query 文本的向量（embedder.encode 产出）
            top_k       : 返回的最多命中数（>= 1）

        返回：
            list[RetrievalHit]：按 score 降序排列；空索引返回空列表

        算法：
            1. 对每个 entry 算余弦相似度（O(N * D)，D 为向量维度）
            2. 用大小为 top_k 的小顶堆维护"当前最大的 k 个"：
               - 堆顶是堆中最小的一个
               - 新分数比堆顶大时，弹出堆顶、压入新分数
               - 结束时堆里恰好是最大的 k 个，复杂度 O(N log k)
            3. 堆内元素按 score 升序，取出后反转即得降序结果

        过滤：
            - score < self._min_score 的 entry 直接不入堆
            - query 为空向量时返回 []（无法度量相似度）
        """
        if not query_vector or not self._entries:
            return []

        if top_k < 1:
            raise ValueError(f"top_k 必须 >= 1，收到 {top_k}")

        # 小顶堆：元素为 (score, index)，index 保证分数相同时比较不报错
        # （float 相等时比较 Chunk 会触发 dataclass 比较，不想要这种行为）
        heap: list[tuple[float, int]] = []
        heap_size = 0

        for index, (chunk, vector) in enumerate(self._entries):
            score = cosine_similarity(query_vector, vector)
            if score < self._min_score:
                continue  # 低于阈值：视为噪声，不入堆
            if heap_size < top_k:
                # 堆未满：直接压入（push）
                heapq.heappush(heap, (score, index))
                heap_size += 1
            elif score > heap[0][0]:
                # 堆已满且新分数大于堆顶（堆中最小的）：替换（pop + push）
                heapq.heapreplace(heap, (score, index))

        # 堆内按 score 升序，反转得到降序；再映射回完整命中信息
        hits: list[RetrievalHit] = []
        for score, index in sorted(heap, key=lambda x: x[0], reverse=True):
            chunk = self._entries[index][0]
            hits.append(
                RetrievalHit(
                    chunk_id=chunk.chunk_id,
                    page_no=chunk.page_no,
                    score=score,
                    text=chunk.text,
                )
            )
        return hits


# ---------------------------------------------------------------------------
# 命令行演示
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import sys

    if len(sys.argv) < 3:
        print("用法: python vector_store.py <query> <文档路径> [top_k]")
        sys.exit(1)

    from memory.embedding.embedder import Embedder
    from memory.parser.chunker import chunk_pages
    from memory.parser.pdf_parser import extract_pdf_text

    query = sys.argv[1]
    pdf_path = sys.argv[2]
    top_k = int(sys.argv[3]) if len(sys.argv) > 3 else 5

    # 完整链路：PDF -> chunk -> 向量 -> 索引
    parsed = extract_pdf_text(pdf_path)
    chunked = chunk_pages(parsed.pages, chunk_size=200, overlap=30)
    chunk_texts = [c.text for c in chunked.chunks]

    emb = Embedder().fit(chunk_texts)
    vectors = emb.encode_all(chunk_texts)

    store = VectorStore(min_score=0.05)
    store.add_batch(chunked.chunks, vectors)

    qv = emb.encode(query)
    hits = store.search(qv, top_k=top_k)

    print(f"索引 {len(store)} 个 chunk，query: {query}")
    for h in hits:
        print(f"  [{h.score:.4f}] 第{h.page_no}页 chunk#{h.chunk_id}: {h.text[:50]}...")
