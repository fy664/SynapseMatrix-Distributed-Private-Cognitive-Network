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
"""

from __future__ import annotations

from memory.embedding.tfidf import TfidfVectorizer, cosine_similarity


class Embedder:
    """统一的文本向量化入口。

    用法：
        emb = Embedder()
        emb.fit(chunk_texts)            # 学习词典/IDF（训练阶段）
        v = emb.encode("一段文本")       # 单文本 -> 稀疏向量
        vs = emb.encode_all(texts)      # 批量
        score = emb.similarity(v, other)
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


if __name__ == "__main__":
    import sys

    if len(sys.argv) < 3:
        print("用法: python embedder.py <文本A> <文本B>")
        sys.exit(1)

    emb = Embedder().fit([sys.argv[1], sys.argv[2]])
    va, vb = emb.encode(sys.argv[1]), emb.encode(sys.argv[2])
    print(f"相似度: {emb.similarity(va, vb):.4f}")
