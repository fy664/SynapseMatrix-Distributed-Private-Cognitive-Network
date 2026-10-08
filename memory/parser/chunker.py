"""
SynapseMatrix — Memory Parser 层：文本 Chunker（Month 1 环节 2/6 最小实现）
==========================================================================

定位：承接 pdf_parser 的输出，对应规格书第 11 章 Pipeline 的
"Parser -> Normalizer -> Chunker -> Embedding" 中的 Chunker。

本模块职责（V1 范围）：
    1. 把整段文本按"固定长度 + 重叠窗口"切分为若干 chunk
    2. 每个 chunk 保留来源页码与字符数元信息
    3. 输出结构化的 Chunk 对象，供后续 Embedding / 检索使用

设计决策（为什么 V1 用固定长度 + 重叠，而不是语义切分）：
    - 规格书 0.2 核心原则：先正确，后性能；先手写最小实现，再引入成熟框架。
    - 固定长度切分是确定性算法：同样的输入永远得到同样的输出，
      可测试、可复现、可 Benchmark（符合"可解释、可复现"的项目原则）。
    - 语义切分（按句子/段落/主题）依赖语言模型或 NLP 规则，正确性难以立刻验证，
      且是 Month 1 之后的研究方向（规格书 9.5 之后的扩展区）。
    - 重叠窗口（overlap）解决的问题：一个语义单元（如一个完整段落）如果恰好
      落在两个 chunk 的边界上，会被从中间切断，导致检索时两头都搜不全。
      让相邻 chunk 共享一部分文本，切断处的信息在前后两个 chunk 里都保留。

V1 简化（明确的技术债，记入 TECH-DEBT）：
    - 按页独立切分，不跨页合并（跨页段落会被切断，V2 处理）
    - 不感知句子/段落边界，纯按字符数切
    - 不处理空白字符占比过高的"垃圾 chunk"（V2 加过滤）

依赖：标准库 dataclasses（无第三方依赖）
"""

from __future__ import annotations

from dataclasses import dataclass, field

# 从 pdf_parser 复用 Page 类型（保持单一数据来源）
from memory.parser.pdf_parser import Page


# ---------------------------------------------------------------------------
# 数据结构
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Chunk:
    """一次切分产生的一个文本块。

    属性说明：
        chunk_id : 全局递增编号，从 1 开始（用于后续 embedding 的引用键）
        page_no  : 来源页码（对应 Page.page_no，从 1 开始）
        text     : chunk 文本（是原页文本的连续子串，未做二次改写）
        char_count: len(text)，供统计与体积判断

    不变式：同一页内各 chunk 按顺序拼接后，重叠部分会重复出现；
    单看每个 chunk，它必是原页文本的某个 [start, end) 子串。
    """

    chunk_id: int
    page_no: int
    text: str
    char_count: int = field(init=False)

    def __post_init__(self) -> None:
        # frozen dataclass 派生字段赋值方式与 Page 保持一致
        object.__setattr__(self, "char_count", len(self.text))


@dataclass
class ChunkResult:
    """一次文档切分的整体结果。

    属性说明：
        chunk_count: 切出的 chunk 总数
        total_chars: 所有 chunk 字符数之和（含重叠部分，因此可能大于源文本）
        chunks     : Chunk 列表，按 (page_no, chunk_id) 顺序排列
    """

    chunk_count: int
    total_chars: int
    chunks: list[Chunk]


# ---------------------------------------------------------------------------
# 核心切分算法（纯函数）
# ---------------------------------------------------------------------------

def chunk_text(text: str, chunk_size: int, overlap: int = 0) -> list[str]:
    """把单段文本按固定长度 + 重叠窗口切分为若干子串（纯函数，无副作用）。

    参数：
        text      : 待切分文本；空字符串返回空列表
        chunk_size: 每个 chunk 的目标字符数，必须 >= 1
        overlap   : 相邻 chunk 共享的字符数，必须 >= 0 且 < chunk_size

    返回：
        list[str]：按顺序排列的 chunk 文本列表

    算法（滑动窗口）：
        step = chunk_size - overlap        # 每次窗口前进的步长
        start 从 0 开始，每次取 [start, start+chunk_size)，
        然后 start += step，直到窗口覆盖到文本末尾为止。

    边界情况：
        - 文本为空            -> []
        - 文本长度 <= chunk_size -> [text]（只切出一个 chunk）
        - 最后一块不足 chunk_size -> 取剩余全部（不丢弃尾部）

    异常：
        ValueError: chunk_size < 1 或 overlap < 0 或 overlap >= chunk_size
    """
    # 1) 参数校验：非法参数必须在切分前暴露，不能静默接受
    if chunk_size < 1:
        raise ValueError(f"chunk_size 必须 >= 1，收到 {chunk_size}")
    if overlap < 0:
        raise ValueError(f"overlap 必须 >= 0，收到 {overlap}")
    if overlap >= chunk_size:
        raise ValueError(
            f"overlap 必须小于 chunk_size（{overlap} >= {chunk_size}）"
            "，否则 step <= 0，窗口永远无法前进，会死循环"
        )

    # 2) 空文本：没有可切的内容
    if not text:
        return []

    # 3) 文本不超过一个 chunk：整体作为一个 chunk，避免无意义的切分
    if len(text) <= chunk_size:
        return [text]

    step = chunk_size - overlap  # 窗口每次前进的步长（>= 1，已由校验保证）
    chunks: list[str] = []
    start = 0
    text_len = len(text)

    while start < text_len:
        end = min(start + chunk_size, text_len)  # 最后一块可能不足 chunk_size
        chunks.append(text[start:end])
        if end == text_len:
            break  # 已覆盖到末尾，终止循环
        start += step

    return chunks


# ---------------------------------------------------------------------------
# 页级批量接口（供 ingestion 调用）
# ---------------------------------------------------------------------------

def chunk_pages(pages: list[Page], chunk_size: int, overlap: int = 0) -> ChunkResult:
    """把 Page 列表逐页切分为 Chunk 列表（V1：按页独立切分，不跨页）。

    参数：
        pages     : pdf_parser 产出的 Page 列表（保持页码顺序）
        chunk_size: 传给 chunk_text 的块长
        overlap   : 传给 chunk_text 的重叠长度

    返回：
        ChunkResult：含统计与有序 chunk 列表

    行为说明（V1 简化，技术债）：
        - 每页独立切分：跨页连续文本会在页边界被切断，V2 引入跨页合并
        - 空文本页不产出 chunk（跳过）
        - chunk_id 全局递增，不随页重置，保证全文档唯一
    """
    chunks: list[Chunk] = []
    next_id = 1

    for page in pages:
        # 空页直接跳过（parser 已规范化，这里再防御一次）
        if not page.text:
            continue
        for text_chunk in chunk_text(page.text, chunk_size, overlap):
            chunks.append(Chunk(chunk_id=next_id, page_no=page.page_no, text=text_chunk))
            next_id += 1

    return ChunkResult(
        chunk_count=len(chunks),
        total_chars=sum(c.char_count for c in chunks),
        chunks=chunks,
    )


# ---------------------------------------------------------------------------
# 命令行演示
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import sys

    if len(sys.argv) < 2:
        print("用法: python chunker.py <pdf路径> [chunk_size] [overlap]")
        sys.exit(1)

    from memory.parser.pdf_parser import extract_pdf_text

    size = int(sys.argv[2]) if len(sys.argv) > 2 else 512
    ov = int(sys.argv[3]) if len(sys.argv) > 3 else 64

    parsed = extract_pdf_text(sys.argv[1])
    result = chunk_pages(parsed.pages, chunk_size=size, overlap=ov)
    print(f"页数: {parsed.page_count}, 有效页: {parsed.non_empty}")
    print(f"chunk 数: {result.chunk_count}, 总字符(含重叠): {result.total_chars}")
    for c in result.chunks[:10]:
        print(f"--- chunk #{c.chunk_id} (第 {c.page_no} 页, {c.char_count} 字符) ---")
        print(c.text[:80])
