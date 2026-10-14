"""
SynapseMatrix — Memory Embedding 层：统一 Embedder 接口（Month 1 环节 3/6）
===========================================================================

定位：对上层（retrieval）暴露稳定接口，屏蔽底层向量化实现。

为什么需要这一层（抽象动机）：
    - retrieval 只关心"给文本 -> 拿向量 -> 算相似度"，
      不关心向量来自 TF-IDF 还是语义模型。
    - V1 用 TF-IDF（本目录 tfidf.py），V2 换 sentence-transformers（BGE）
      时只改 Embedder 内部实现，检索层零改动——这就是"面向接口编程"。

本模块提供：
    - Embedder：统一入口（默认 TfidfVectorizer，保留后端切换点）
    - save / load：模型持久化（Month 1 环节 6/6 新增）
      向量空间 = 词典 + IDF + 文档数，三者缺一不可：
      不存词典，下次 encode 时无法还原同一向量空间，检索必然失配。
"""

from __future__ import annotations

import json
from pathlib import Path

from memory.embedding.tfidf import TfidfVectorizer, cosine_similarity


class Embedder:
    """统一的文本向量化入口。

    用法：
        emb = Embedder()
        emb.fit(chunk_texts)            # 学习词典/IDF（训练阶段）
        v = emb.encode("一段文本")       # 单文本 -> 稀疏向量
        vs = emb.encode_all(texts)      # 批量
        score = emb.similarity(v, other)

        # 持久化（6/6 新增）
        emb.save("data/embedder.json")  # 存模型（词典+IDF+文档数）
        emb2 = Embedder().load("data/embedder.json")  # 重启后还原
    """

    def __init__(self) -> None:
        # V1 后端：手写 TF-IDF。
        # V2 切换点：换成 SentenceTransformerBackend（加载 BGE 模型），
        # 只需保证新后端实现 fit/encode/encode_all 三个方法即可。
        self._backend = TfidfVectorizer()

    def fit(self, texts: list[str]) -> "Embedder":
        """训练阶段：在文档集合上建立词表与 IDF。"""
        self._backend.fit(texts)
        return self

    def encode(self, text: str) -> dict[str, float]:
        """单文本 -> 向量（V1 为稀疏向量 {词: 权重}）。"""
        return self._backend.transform(text)

    def encode_all(self, texts: list[str]) -> list[dict[str, float]]:
        """批量文本 -> 向量列表（顺序与输入一致）。"""
        return self._backend.transform_all(texts)

    def similarity(self, a: dict[str, float], b: dict[str, float]) -> float:
        """两个向量的余弦相似度。"""
        return cosine_similarity(a, b)

    # ------------------------------------------------------------------
    # 模型持久化（Month 1 环节 6/6）
    # ------------------------------------------------------------------

    def save(self, path: str | Path) -> None:
        """把向量化模型（词典/IDF/文档数）写入 JSON。

        为什么必须存这三样：
            - vocabulary_：词 -> 下标的映射（V1 稀疏表示可不用下标，保留对齐）
            - idf_       ：每个词的逆文档频率（权重计算的依据）
            - n_docs_    ：fit 时的文档总数（IDF 公式的分母来源）
        三者共同定义"向量空间"；缺任一，加载后 encode 结果都与原空间不一致。
        """
        backend = self._backend
        payload = {
            "type": "tfidf",
            "vocabulary": backend.vocabulary_,
            "idf": backend.idf_,
            "n_docs": backend.n_docs_,
        }
        p = Path(path)
        p.parent.mkdir(parents=True, exist_ok=True)
        # ensure_ascii=False：中文词典以明文存储（可读、可 diff）
        p.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")

    def load(self, path: str | Path) -> "Embedder":
        """从 JSON 加载向量化模型并还原后端（自返回，支持链式）。

        抛错：
            FileNotFoundError：模型文件不存在（首次使用应先 fit 再 save）
            ValueError       ：文件类型不是 tfidf（如误传记忆库文件）
        """
        p = Path(path)
        if not p.exists():
            raise FileNotFoundError(f"模型文件不存在: {p}")

        payload = json.loads(p.read_text(encoding="utf-8"))
        if payload.get("type") != "tfidf":
            raise ValueError(f"不支持的模型类型: {payload.get('type')}")

        backend = TfidfVectorizer()
        backend.vocabulary_ = dict(payload["vocabulary"])
        backend.idf_ = {str(k): float(v) for k, v in payload["idf"].items()}
        backend.n_docs_ = int(payload["n_docs"])
        self._backend = backend
        return self

    def is_fitted(self) -> bool:
        """是否已完成 fit/load（有词典）。pipeline 据此决定跳过训练。"""
        return bool(self._backend.vocabulary_)


if __name__ == "__main__":
    import sys

    if len(sys.argv) < 3:
        print("用法: python embedder.py <文本A> <文本B>")
        sys.exit(1)

    emb = Embedder().fit([sys.argv[1], sys.argv[2]])
    va, vb = emb.encode(sys.argv[1]), emb.encode(sys.argv[2])
    print(f"相似度: {emb.similarity(va, vb):.4f}")
